#include "offline_echo.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pcm_ring.h"
#include "remote_event_queue.h"
#include "remote_response.h"

#define SAMPLE_RATE_HZ 24000
#define BITS_PER_SAMPLE 16
#define CHANNEL_COUNT 1
#define FRAME_SAMPLES 256
#define MAX_RECORDING_SECONDS 6
#define RECORDING_COMMIT_MS 180
#define PRECOMMIT_RING_MS 220
#define PLAYBACK_RING_MS 500
#define PREPARE_DELAY_MS 220
#define REMOTE_RESPONSE_DEADLINE_MS 6000
#define REMOTE_EVENT_STORAGE_COUNT 256
#define DEFAULT_OUTPUT_VOLUME_PERCENT 20
#define MIN_OUTPUT_VOLUME_PERCENT 10
#define MAX_OUTPUT_VOLUME_PERCENT 100
#define INPUT_GAIN_DB 24.0f
#define COMMAND_QUEUE_LENGTH 8

static const char *TAG = "offline_echo";

typedef enum {
    COMMAND_RECORD_START = 0,
    COMMAND_RECORD_STOP,
    COMMAND_SET_MUTED,
    COMMAND_SET_VOLUME,
} command_type_t;

typedef struct {
    command_type_t type;
    bool muted;
    uint8_t volume_percent;
} command_t;

struct offline_echo {
    i2s_chan_handle_t tx_handle;
    i2s_chan_handle_t rx_handle;
    esp_codec_dev_handle_t codec;
    int16_t *capture_storage;
    int16_t *precommit_storage;
    int16_t *playback_storage;
    int16_t *remote_response_storage;
    remote_event_t *remote_event_storage;
    pcm_ring_t capture_ring;
    pcm_ring_t precommit_ring;
    pcm_ring_t playback_ring;
    remote_response_t remote_response;
    remote_event_queue_t remote_events;
    size_t recorded_samples;
    bool recording_committed;
    QueueHandle_t commands;
    TaskHandle_t task;
    portMUX_TYPE snapshot_lock;
    offline_echo_snapshot_t snapshot;
    offline_echo_stream_sink_t stream_sink;
    void *stream_context;
    bool stream_turn_accepted;
    bool remote_attempted;
    uint32_t turn_counter;
    uint32_t active_turn_token;
    int64_t prepare_until_us;
    int64_t remote_deadline_us;
};

static esp_codec_dev_sample_info_t sample_info(void)
{
    return (esp_codec_dev_sample_info_t) {
        .bits_per_sample = BITS_PER_SAMPLE,
        .channel = CHANNEL_COUNT,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE_HZ,
        .mclk_multiple = 256,
    };
}

static void update_state(offline_echo_t *echo, offline_echo_state_t state,
                         esp_err_t error)
{
    portENTER_CRITICAL(&echo->snapshot_lock);
    echo->snapshot.state = state;
    echo->snapshot.last_error = error;
    portEXIT_CRITICAL(&echo->snapshot_lock);
}

static bool is_muted(offline_echo_t *echo)
{
    portENTER_CRITICAL(&echo->snapshot_lock);
    const bool muted = echo->snapshot.muted;
    portEXIT_CRITICAL(&echo->snapshot_lock);
    return muted;
}

static bool emit_stream_event(offline_echo_t *echo,
                              offline_echo_stream_event_t event,
                              const int16_t *samples, size_t sample_count)
{
    offline_echo_stream_sink_t sink;
    void *context;
    portENTER_CRITICAL(&echo->snapshot_lock);
    sink = echo->stream_sink;
    context = echo->stream_context;
    portEXIT_CRITICAL(&echo->snapshot_lock);

    if (sink == NULL) {
        return false;
    }
    const bool accepted = sink(context, event, echo->active_turn_token,
                               samples, sample_count);
    portENTER_CRITICAL(&echo->snapshot_lock);
    if (accepted && event == OFFLINE_ECHO_STREAM_AUDIO) {
        echo->snapshot.stream_audio_frames++;
    } else if (!accepted) {
        echo->snapshot.stream_drops++;
    }
    portEXIT_CRITICAL(&echo->snapshot_lock);
    return accepted;
}

static void emit_stream_audio_or_cancel(offline_echo_t *echo,
                                        const int16_t *samples,
                                        size_t sample_count)
{
    if (!echo->stream_turn_accepted) {
        return;
    }
    if (!emit_stream_event(echo, OFFLINE_ECHO_STREAM_AUDIO,
                           samples, sample_count)) {
        emit_stream_event(echo, OFFLINE_ECHO_STREAM_CANCEL, NULL, 0);
        echo->stream_turn_accepted = false;
    }
}

static void finish_stream_turn(offline_echo_t *echo,
                               offline_echo_stream_event_t event)
{
    if (echo->stream_turn_accepted) {
        emit_stream_event(echo, event, NULL, 0);
        echo->stream_turn_accepted = false;
    }
}

static void cancel_pending_remote(offline_echo_t *echo)
{
    if (echo->remote_attempted && echo->active_turn_token != 0
            && remote_response_status(
                &echo->remote_response, echo->active_turn_token)
                == REMOTE_RESPONSE_RECEIVING) {
        emit_stream_event(echo, OFFLINE_ECHO_STREAM_RESPONSE_CANCEL,
                          NULL, 0);
    }
}

static void process_remote_events(offline_echo_t *echo)
{
    const remote_event_t *event;
    while ((event = remote_event_queue_peek(&echo->remote_events)) != NULL) {
        if (event->turn_token == echo->active_turn_token) {
            bool accepted = true;
            switch (event->type) {
            case REMOTE_EVENT_AUDIO:
                accepted = remote_response_append(
                    &echo->remote_response, event->turn_token,
                    event->samples, event->sample_count);
                if (accepted) {
                    portENTER_CRITICAL(&echo->snapshot_lock);
                    echo->snapshot.remote_audio_frames++;
                    portEXIT_CRITICAL(&echo->snapshot_lock);
                }
                break;
            case REMOTE_EVENT_DONE:
                accepted = remote_response_complete(
                    &echo->remote_response, event->turn_token,
                    event->value_count, event->input_count,
                    echo->recorded_samples);
                break;
            case REMOTE_EVENT_CANCELLED:
                remote_response_cancel(&echo->remote_response,
                                       event->turn_token);
                break;
            case REMOTE_EVENT_INVALID:
                remote_response_invalidate(&echo->remote_response,
                                           event->turn_token);
                break;
            }
            if (!accepted) {
                portENTER_CRITICAL(&echo->snapshot_lock);
                echo->snapshot.remote_event_drops++;
                portEXIT_CRITICAL(&echo->snapshot_lock);
            }
        }
        remote_event_queue_consume(&echo->remote_events);
    }
}

static void set_recorded_samples(offline_echo_t *echo, size_t samples)
{
    echo->recorded_samples = samples;
    portENTER_CRITICAL(&echo->snapshot_lock);
    echo->snapshot.recorded_ms = (uint32_t)(samples * 1000 / SAMPLE_RATE_HZ);
    portEXIT_CRITICAL(&echo->snapshot_lock);
}

static void set_recording_committed(offline_echo_t *echo, bool committed)
{
    echo->recording_committed = committed;
    portENTER_CRITICAL(&echo->snapshot_lock);
    echo->snapshot.recording_committed = committed;
    portEXIT_CRITICAL(&echo->snapshot_lock);
}

static void update_ring_depths(offline_echo_t *echo)
{
    portENTER_CRITICAL(&echo->snapshot_lock);
    echo->snapshot.precommit_buffered_ms = (uint32_t)(
        pcm_ring_count(&echo->precommit_ring) * 1000 / SAMPLE_RATE_HZ);
    echo->snapshot.capture_queued_ms = (uint32_t)(
        pcm_ring_count(&echo->capture_ring) * 1000 / SAMPLE_RATE_HZ);
    echo->snapshot.playback_queued_ms = (uint32_t)(
        pcm_ring_count(&echo->playback_ring) * 1000 / SAMPLE_RATE_HZ);
    portEXIT_CRITICAL(&echo->snapshot_lock);
}

static void reset_audio_rings(offline_echo_t *echo)
{
    pcm_ring_reset(&echo->precommit_ring);
    pcm_ring_reset(&echo->capture_ring);
    pcm_ring_reset(&echo->playback_ring);
    update_ring_depths(echo);
}

static void add_capture_overrun(offline_echo_t *echo)
{
    portENTER_CRITICAL(&echo->snapshot_lock);
    echo->snapshot.capture_overruns++;
    portEXIT_CRITICAL(&echo->snapshot_lock);
}

static void set_playback_level(offline_echo_t *echo, float level)
{
    portENTER_CRITICAL(&echo->snapshot_lock);
    echo->snapshot.playback_level = level;
    portEXIT_CRITICAL(&echo->snapshot_lock);
}

static void add_read_error(offline_echo_t *echo, int error)
{
    portENTER_CRITICAL(&echo->snapshot_lock);
    echo->snapshot.read_errors++;
    echo->snapshot.last_error = error == ESP_CODEC_DEV_OK ? ESP_FAIL : (esp_err_t)error;
    portEXIT_CRITICAL(&echo->snapshot_lock);
}

static void add_write_error(offline_echo_t *echo, int error)
{
    portENTER_CRITICAL(&echo->snapshot_lock);
    echo->snapshot.write_errors++;
    echo->snapshot.last_error = error == ESP_CODEC_DEV_OK ? ESP_FAIL : (esp_err_t)error;
    portEXIT_CRITICAL(&echo->snapshot_lock);
}

static bool apply_output_volume(offline_echo_t *echo, uint8_t volume_percent)
{
    const int result = esp_codec_dev_set_out_vol(echo->codec, volume_percent);
    if (result != ESP_CODEC_DEV_OK) {
        add_write_error(echo, result);
        ESP_LOGE(TAG, "Set output volume failed: %d", result);
        return false;
    }
    portENTER_CRITICAL(&echo->snapshot_lock);
    echo->snapshot.output_volume_percent = volume_percent;
    portEXIT_CRITICAL(&echo->snapshot_lock);
    ESP_LOGI(TAG, "Output volume=%u%%", (unsigned)volume_percent);
    return true;
}

static esp_err_t initialize_codec(offline_echo_t *echo)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &echo->tx_handle,
                                        &echo->rx_handle),
                        TAG, "create duplex I2S channels");

    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(echo->tx_handle, &i2s_config),
                        TAG, "initialize I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(echo->rx_handle, &i2s_config),
                        TAG, "initialize I2S RX");

    audio_codec_i2s_cfg_t data_config = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = echo->rx_handle,
        .tx_handle = echo->tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_config);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create I2S codec interface");

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "initialize board I2C");
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create codec GPIO interface");

    audio_codec_i2c_cfg_t control_config = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *control_if =
        audio_codec_new_i2c_ctrl(&control_config);
    ESP_RETURN_ON_FALSE(control_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create codec control interface");

    const esp_codec_dev_hw_gain_t hardware_gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = control_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = hardware_gain,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&codec_config);
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_ERR_NO_MEM, TAG,
                        "create ES8311 interface");

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    echo->codec = esp_codec_dev_new(&device_config);
    ESP_RETURN_ON_FALSE(echo->codec != NULL, ESP_ERR_NO_MEM, TAG,
                        "create duplex codec device");

    esp_codec_dev_sample_info_t format = sample_info();
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_open(echo->codec, &format) == ESP_CODEC_DEV_OK,
        ESP_FAIL, TAG, "open duplex codec");
    ESP_RETURN_ON_FALSE(
        apply_output_volume(echo, DEFAULT_OUTPUT_VOLUME_PERCENT),
        ESP_FAIL, TAG, "set output volume");
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_set_in_gain(echo->codec, INPUT_GAIN_DB) == ESP_CODEC_DEV_OK,
        ESP_FAIL, TAG, "set input gain");
    ESP_RETURN_ON_FALSE(
        esp_codec_dev_set_out_mute(echo->codec, true) == ESP_CODEC_DEV_OK,
        ESP_FAIL, TAG, "mute output");
    return ESP_OK;
}

static float frame_level(const int16_t *samples, size_t count)
{
    uint64_t sum_squares = 0;
    for (size_t index = 0; index < count; index++) {
        const int32_t sample = samples[index];
        sum_squares += (uint64_t)((int64_t)sample * sample);
    }
    const float rms = sqrtf((float)sum_squares / count);
    return fminf(1.0f, rms / 2600.0f);
}

static void begin_recording(offline_echo_t *echo)
{
    cancel_pending_remote(echo);
    if (echo->active_turn_token != 0) {
        remote_response_cancel(&echo->remote_response,
                               echo->active_turn_token);
    }
    echo->turn_counter++;
    if (echo->turn_counter == 0) {
        echo->turn_counter++;
    }
    echo->active_turn_token = echo->turn_counter;
    echo->remote_attempted = false;
    reset_audio_rings(echo);
    set_recorded_samples(echo, 0);
    set_recording_committed(echo, false);
    echo->stream_turn_accepted = false;
    update_state(echo, OFFLINE_ECHO_RECORDING, ESP_OK);
    ESP_LOGI(TAG, "Recording pending commit");
}

static bool commit_recording(offline_echo_t *echo, int16_t *scratch)
{
    remote_response_begin(&echo->remote_response,
                          echo->active_turn_token);
    echo->stream_turn_accepted = emit_stream_event(
        echo, OFFLINE_ECHO_STREAM_START, NULL, 0);
    echo->remote_attempted = echo->stream_turn_accepted;
    if (!echo->remote_attempted) {
        remote_response_cancel(&echo->remote_response,
                               echo->active_turn_token);
    }
    while (pcm_ring_count(&echo->precommit_ring) > 0) {
        const size_t available = pcm_ring_count(&echo->precommit_ring);
        const size_t count = available < FRAME_SAMPLES
            ? available : FRAME_SAMPLES;
        const size_t read = pcm_ring_read(&echo->precommit_ring, scratch,
                                          count);
        const size_t written = pcm_ring_write(&echo->capture_ring, scratch,
                                               read);
        if (written != read) {
            add_capture_overrun(echo);
            update_ring_depths(echo);
            update_state(echo, OFFLINE_ECHO_ERROR, ESP_ERR_NO_MEM);
            finish_stream_turn(echo, OFFLINE_ECHO_STREAM_CANCEL);
            return false;
        }
        emit_stream_audio_or_cancel(echo, scratch, written);
    }
    set_recorded_samples(echo, pcm_ring_count(&echo->capture_ring));
    set_recording_committed(echo, true);
    update_ring_depths(echo);
    ESP_LOGI(TAG, "Recording committed with %ums buffered",
             (unsigned)echo->snapshot.recorded_ms);
    return true;
}

typedef enum {
    PLAYBACK_COMMAND_CONTINUE = 0,
    PLAYBACK_COMMAND_CANCEL,
    PLAYBACK_COMMAND_START_RECORDING,
} playback_command_result_t;

static playback_command_result_t handle_playback_command(offline_echo_t *echo)
{
    command_t command;
    while (xQueueReceive(echo->commands, &command, 0) == pdTRUE) {
        switch (command.type) {
        case COMMAND_SET_MUTED:
            portENTER_CRITICAL(&echo->snapshot_lock);
            echo->snapshot.muted = command.muted;
            portEXIT_CRITICAL(&echo->snapshot_lock);
            if (command.muted) {
                return PLAYBACK_COMMAND_CANCEL;
            }
            break;
        case COMMAND_SET_VOLUME:
            apply_output_volume(echo, command.volume_percent);
            break;
        case COMMAND_RECORD_START:
            if (!is_muted(echo)) {
                begin_recording(echo);
                return PLAYBACK_COMMAND_START_RECORDING;
            }
            break;
        case COMMAND_RECORD_STOP:
            break;
        }
    }
    return is_muted(echo)
        ? PLAYBACK_COMMAND_CANCEL : PLAYBACK_COMMAND_CONTINUE;
}

static bool playback_source_available(offline_echo_t *echo,
                                      bool use_remote,
                                      uint32_t turn_token)
{
    return use_remote
        ? remote_response_status(&echo->remote_response, turn_token)
            == REMOTE_RESPONSE_READY
        : pcm_ring_count(&echo->capture_ring) > 0;
}

static bool refill_playback_ring(offline_echo_t *echo, int16_t *scratch,
                                 bool use_remote, uint32_t turn_token)
{
    while (playback_source_available(echo, use_remote, turn_token)
            && pcm_ring_free(&echo->playback_ring) > 0) {
        size_t count = pcm_ring_free(&echo->playback_ring);
        if (count > FRAME_SAMPLES) {
            count = FRAME_SAMPLES;
        }
        const size_t read = use_remote
            ? remote_response_read(&echo->remote_response, turn_token,
                                   scratch, count)
            : pcm_ring_read(&echo->capture_ring, scratch, count);
        if (read == 0
                || pcm_ring_write(&echo->playback_ring, scratch, read)
                    != read) {
            add_capture_overrun(echo);
            update_ring_depths(echo);
            return false;
        }
    }
    update_ring_depths(echo);
    return true;
}

static void play_recording(offline_echo_t *echo, bool use_remote)
{
    const uint32_t turn_token = echo->active_turn_token;
    const size_t expected_samples = use_remote
        ? remote_response_sample_count(&echo->remote_response, turn_token)
        : echo->recorded_samples;
    const bool remote_attempted = echo->remote_attempted;
    if (is_muted(echo) || expected_samples == 0 || turn_token == 0) {
        reset_audio_rings(echo);
        set_recorded_samples(echo, 0);
        set_recording_committed(echo, false);
        update_state(echo, OFFLINE_ECHO_IDLE, ESP_OK);
        return;
    }
    if (!use_remote) {
        remote_response_cancel(&echo->remote_response, turn_token);
    }

    pcm_ring_reset(&echo->playback_ring);
    update_ring_depths(echo);
    update_state(echo, OFFLINE_ECHO_PLAYING, ESP_OK);
    if (esp_codec_dev_set_out_mute(echo->codec, false) != ESP_CODEC_DEV_OK) {
        add_write_error(echo, ESP_FAIL);
        update_state(echo, OFFLINE_ECHO_ERROR, ESP_FAIL);
        return;
    }

    int16_t samples[FRAME_SAMPLES];
    float smoothed_level = 0.0f;
    size_t played_samples = 0;
    bool write_failed = false;
    bool start_recording = false;
    bool cancelled = false;
    while (playback_source_available(echo, use_remote, turn_token)
            || pcm_ring_count(&echo->playback_ring) > 0) {
        const playback_command_result_t command =
            handle_playback_command(echo);
        if (command == PLAYBACK_COMMAND_START_RECORDING) {
            start_recording = true;
            break;
        }
        if (command == PLAYBACK_COMMAND_CANCEL) {
            cancelled = true;
            break;
        }
        if (!refill_playback_ring(echo, samples, use_remote, turn_token)) {
            write_failed = true;
            break;
        }

        const size_t count = pcm_ring_read(&echo->playback_ring, samples,
                                           FRAME_SAMPLES);
        update_ring_depths(echo);
        if (count == 0) {
            portENTER_CRITICAL(&echo->snapshot_lock);
            echo->snapshot.playback_underruns++;
            portEXIT_CRITICAL(&echo->snapshot_lock);
            break;
        }

        const float level = frame_level(samples, count);
        const float smoothing = level > smoothed_level ? 0.58f : 0.24f;
        smoothed_level += (level - smoothed_level) * smoothing;
        set_playback_level(echo, smoothed_level);

        const int result = esp_codec_dev_write(
            echo->codec, samples, count * sizeof(*samples));
        if (result != ESP_CODEC_DEV_OK) {
            add_write_error(echo, result);
            write_failed = true;
            break;
        }
        played_samples += count;
    }

    esp_codec_dev_set_out_mute(echo->codec, true);
    set_playback_level(echo, 0.0f);
    const bool completed = !write_failed && !cancelled && !start_recording
        && played_samples == expected_samples;
    if (completed) {
        if (use_remote) {
            portENTER_CRITICAL(&echo->snapshot_lock);
            echo->snapshot.remote_playbacks++;
            portEXIT_CRITICAL(&echo->snapshot_lock);
            emit_stream_event(echo, OFFLINE_ECHO_STREAM_REMOTE_PLAYED,
                              NULL, played_samples);
        } else if (remote_attempted) {
            portENTER_CRITICAL(&echo->snapshot_lock);
            echo->snapshot.local_fallbacks++;
            portEXIT_CRITICAL(&echo->snapshot_lock);
            emit_stream_event(echo, OFFLINE_ECHO_STREAM_LOCAL_FALLBACK,
                              NULL, played_samples);
        }
    }
    if (!start_recording) {
        remote_response_cancel(&echo->remote_response, turn_token);
        reset_audio_rings(echo);
        set_recorded_samples(echo, 0);
        set_recording_committed(echo, false);
        echo->active_turn_token = 0;
        echo->remote_attempted = false;
        update_state(echo,
                     write_failed ? OFFLINE_ECHO_ERROR : OFFLINE_ECHO_IDLE,
                     write_failed ? ESP_FAIL : ESP_OK);
    }
    ESP_LOGI(TAG,
             "Playback complete source=%s samples=%u failed=%d cancelled=%d interrupted=%d",
             use_remote ? "remote" : "local", (unsigned)played_samples,
             write_failed, cancelled, start_recording);
}

static void apply_command(offline_echo_t *echo, const command_t *command)
{
    offline_echo_snapshot_t snapshot;
    offline_echo_get_snapshot(echo, &snapshot);

    switch (command->type) {
    case COMMAND_RECORD_START:
        if (!snapshot.muted
                && (snapshot.state == OFFLINE_ECHO_IDLE
                    || snapshot.state == OFFLINE_ECHO_PREPARING)) {
            begin_recording(echo);
        }
        break;
    case COMMAND_RECORD_STOP:
        if (snapshot.state == OFFLINE_ECHO_RECORDING) {
            if (!snapshot.recording_committed) {
                ESP_LOGI(TAG, "Discarding uncommitted tap duration=%ums",
                         (unsigned)snapshot.recorded_ms);
                reset_audio_rings(echo);
                set_recorded_samples(echo, 0);
                set_recording_committed(echo, false);
                echo->active_turn_token = 0;
                update_state(echo, OFFLINE_ECHO_IDLE,
                             ESP_ERR_INVALID_SIZE);
            } else {
                finish_stream_turn(echo, OFFLINE_ECHO_STREAM_COMMIT);
                const int64_t stopped_us = esp_timer_get_time();
                echo->prepare_until_us = stopped_us
                    + PREPARE_DELAY_MS * 1000LL;
                echo->remote_deadline_us = stopped_us
                    + REMOTE_RESPONSE_DEADLINE_MS * 1000LL;
                update_state(echo, OFFLINE_ECHO_PREPARING, ESP_OK);
                ESP_LOGI(TAG, "Recording stopped duration=%ums",
                         (unsigned)snapshot.recorded_ms);
            }
        }
        break;
    case COMMAND_SET_MUTED:
        portENTER_CRITICAL(&echo->snapshot_lock);
        echo->snapshot.muted = command->muted;
        portEXIT_CRITICAL(&echo->snapshot_lock);
        if (command->muted) {
            if (snapshot.state == OFFLINE_ECHO_RECORDING
                    && snapshot.recording_committed) {
                finish_stream_turn(echo, OFFLINE_ECHO_STREAM_CANCEL);
            }
            if (snapshot.state == OFFLINE_ECHO_PREPARING) {
                cancel_pending_remote(echo);
            }
            esp_codec_dev_set_out_mute(echo->codec, true);
            set_playback_level(echo, 0.0f);
            remote_response_cancel(&echo->remote_response,
                                   echo->active_turn_token);
            reset_audio_rings(echo);
            set_recorded_samples(echo, 0);
            set_recording_committed(echo, false);
            echo->active_turn_token = 0;
            echo->remote_attempted = false;
            update_state(echo, OFFLINE_ECHO_IDLE, ESP_OK);
        }
        ESP_LOGI(TAG, "Muted=%d", command->muted);
        break;
    case COMMAND_SET_VOLUME:
        apply_output_volume(echo, command->volume_percent);
        break;
    }
}

static void audio_task(void *argument)
{
    offline_echo_t *echo = argument;
    int16_t samples[FRAME_SAMPLES];

    while (true) {
        command_t command;
        while (xQueueReceive(echo->commands, &command, 0) == pdTRUE) {
            apply_command(echo, &command);
        }

        process_remote_events(echo);
        offline_echo_snapshot_t snapshot;
        offline_echo_get_snapshot(echo, &snapshot);
        if (snapshot.state == OFFLINE_ECHO_PREPARING) {
            const int64_t now_us = esp_timer_get_time();
            const bool expiring = echo->remote_attempted
                && now_us >= echo->remote_deadline_us
                && remote_response_status(
                    &echo->remote_response, echo->active_turn_token)
                    == REMOTE_RESPONSE_RECEIVING;
            const remote_response_decision_t decision =
                remote_response_select(
                    &echo->remote_response, echo->active_turn_token,
                    echo->remote_attempted, now_us / 1000,
                    echo->prepare_until_us / 1000,
                    echo->remote_deadline_us / 1000);
            if (decision == REMOTE_RESPONSE_USE_REMOTE) {
                play_recording(echo, true);
                continue;
            }
            if (decision == REMOTE_RESPONSE_USE_LOCAL) {
                if (expiring) {
                    cancel_pending_remote(echo);
                    portENTER_CRITICAL(&echo->snapshot_lock);
                    echo->snapshot.remote_timeouts++;
                    portEXIT_CRITICAL(&echo->snapshot_lock);
                }
                play_recording(echo, false);
                continue;
            }
        }

        const int result = esp_codec_dev_read(echo->codec, samples,
                                              sizeof(samples));
        if (result != ESP_CODEC_DEV_OK) {
            add_read_error(echo, result);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        offline_echo_get_snapshot(echo, &snapshot);
        if (snapshot.state != OFFLINE_ECHO_RECORDING || snapshot.muted) {
            continue;
        }

        if (!echo->recording_committed) {
            const size_t written = pcm_ring_write(
                &echo->precommit_ring, samples, FRAME_SAMPLES);
            set_recorded_samples(echo,
                                 pcm_ring_count(&echo->precommit_ring));
            update_ring_depths(echo);
            if (written != FRAME_SAMPLES) {
                add_capture_overrun(echo);
                reset_audio_rings(echo);
                set_recorded_samples(echo, 0);
                update_state(echo, OFFLINE_ECHO_ERROR, ESP_ERR_NO_MEM);
                continue;
            }
            if (echo->snapshot.recorded_ms >= RECORDING_COMMIT_MS
                    && !commit_recording(echo, samples)) {
                continue;
            }
        } else {
            const size_t writable = pcm_ring_free(&echo->capture_ring)
                < FRAME_SAMPLES
                    ? pcm_ring_free(&echo->capture_ring)
                    : FRAME_SAMPLES;
            const size_t written = pcm_ring_write(
                &echo->capture_ring, samples, writable);
            set_recorded_samples(echo,
                                 echo->recorded_samples + written);
            update_ring_depths(echo);
            if (written != writable) {
                add_capture_overrun(echo);
            }
            if (written > 0) {
                emit_stream_audio_or_cancel(echo, samples, written);
            }
        }

        if (pcm_ring_free(&echo->capture_ring) == 0) {
            finish_stream_turn(echo, OFFLINE_ECHO_STREAM_COMMIT);
            const int64_t stopped_us = esp_timer_get_time();
            echo->prepare_until_us = stopped_us
                + PREPARE_DELAY_MS * 1000LL;
            echo->remote_deadline_us = stopped_us
                + REMOTE_RESPONSE_DEADLINE_MS * 1000LL;
            update_state(echo, OFFLINE_ECHO_PREPARING, ESP_OK);
            ESP_LOGI(TAG, "Recording reached six-second ring limit");
        }
    }
}

static void free_ring_storage(offline_echo_t *echo)
{
    heap_caps_free(echo->capture_storage);
    heap_caps_free(echo->precommit_storage);
    heap_caps_free(echo->playback_storage);
    heap_caps_free(echo->remote_response_storage);
    heap_caps_free(echo->remote_event_storage);
    echo->capture_storage = NULL;
    echo->precommit_storage = NULL;
    echo->playback_storage = NULL;
    echo->remote_response_storage = NULL;
    echo->remote_event_storage = NULL;
}

esp_err_t offline_echo_create(offline_echo_t **out_echo)
{
    ESP_RETURN_ON_FALSE(out_echo != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "missing output handle");
    *out_echo = NULL;

    offline_echo_t *echo = calloc(1, sizeof(*echo));
    ESP_RETURN_ON_FALSE(echo != NULL, ESP_ERR_NO_MEM, TAG,
                        "allocate echo state");
    portMUX_INITIALIZE(&echo->snapshot_lock);
    const size_t capture_capacity = SAMPLE_RATE_HZ * MAX_RECORDING_SECONDS;
    const size_t precommit_capacity =
        SAMPLE_RATE_HZ * PRECOMMIT_RING_MS / 1000;
    const size_t playback_capacity =
        SAMPLE_RATE_HZ * PLAYBACK_RING_MS / 1000;
    echo->capture_storage = heap_caps_malloc(
        capture_capacity * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    echo->precommit_storage = heap_caps_malloc(
        precommit_capacity * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    echo->playback_storage = heap_caps_malloc(
        playback_capacity * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    echo->remote_response_storage = heap_caps_malloc(
        capture_capacity * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    echo->remote_event_storage = heap_caps_malloc(
        REMOTE_EVENT_STORAGE_COUNT * sizeof(remote_event_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (echo->capture_storage == NULL || echo->precommit_storage == NULL
            || echo->playback_storage == NULL
            || echo->remote_response_storage == NULL
            || echo->remote_event_storage == NULL
            || !remote_response_init(
                &echo->remote_response, echo->remote_response_storage,
                capture_capacity)
            || !remote_event_queue_init(
                &echo->remote_events, echo->remote_event_storage,
                REMOTE_EVENT_STORAGE_COUNT)) {
        free_ring_storage(echo);
        free(echo);
        return ESP_ERR_NO_MEM;
    }
    pcm_ring_init(&echo->capture_ring, echo->capture_storage,
                  capture_capacity);
    pcm_ring_init(&echo->precommit_ring, echo->precommit_storage,
                  precommit_capacity);
    pcm_ring_init(&echo->playback_ring, echo->playback_storage,
                  playback_capacity);

    echo->commands = xQueueCreate(COMMAND_QUEUE_LENGTH, sizeof(command_t));
    if (echo->commands == NULL) {
        free_ring_storage(echo);
        free(echo);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = initialize_codec(echo);
    if (error != ESP_OK) {
        vQueueDelete(echo->commands);
        free_ring_storage(echo);
        free(echo);
        return error;
    }

    echo->snapshot.state = OFFLINE_ECHO_IDLE;
    const BaseType_t created = xTaskCreatePinnedToCore(
        audio_task, "offline_echo", 6144, echo, 6, &echo->task, 0);
    if (created != pdPASS) {
        esp_codec_dev_set_out_mute(echo->codec, true);
        esp_codec_dev_close(echo->codec);
        esp_codec_dev_delete(echo->codec);
        vQueueDelete(echo->commands);
        free_ring_storage(echo);
        free(echo);
        return ESP_ERR_NO_MEM;
    }

    *out_echo = echo;
    ESP_LOGI(TAG,
             "Ready: 24 kHz mono, capture=%ums precommit=%ums playback=%ums total=%u bytes",
             MAX_RECORDING_SECONDS * 1000, PRECOMMIT_RING_MS,
             PLAYBACK_RING_MS,
             (unsigned)((capture_capacity * 2 + precommit_capacity
                         + playback_capacity) * sizeof(int16_t)
                        + REMOTE_EVENT_STORAGE_COUNT
                            * sizeof(remote_event_t)));
    return ESP_OK;
}

static esp_err_t send_command(offline_echo_t *echo, command_t command,
                              bool urgent)
{
    ESP_RETURN_ON_FALSE(echo != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "missing echo handle");
    const BaseType_t sent = urgent
        ? xQueueSendToFront(echo->commands, &command, 0)
        : xQueueSend(echo->commands, &command, 0);
    return sent == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t offline_echo_set_stream_sink(offline_echo_t *echo,
                                       offline_echo_stream_sink_t sink,
                                       void *context)
{
    ESP_RETURN_ON_FALSE(echo != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "missing echo handle");
    portENTER_CRITICAL(&echo->snapshot_lock);
    echo->stream_sink = sink;
    echo->stream_context = context;
    portEXIT_CRITICAL(&echo->snapshot_lock);
    return ESP_OK;
}

bool offline_echo_receive_remote(offline_echo_t *echo,
                                 offline_echo_remote_event_t event,
                                 uint32_t turn_token,
                                 const int16_t *samples,
                                 size_t sample_count,
                                 uint32_t value_count,
                                 uint32_t input_count)
{
    if (echo == NULL || event < OFFLINE_ECHO_REMOTE_AUDIO
            || event > OFFLINE_ECHO_REMOTE_INVALID) {
        return false;
    }
    const bool queued = remote_event_queue_try_push(
        &echo->remote_events, (remote_event_type_t)event, turn_token,
        samples, sample_count, value_count, input_count);
    if (!queued) {
        portENTER_CRITICAL(&echo->snapshot_lock);
        echo->snapshot.remote_event_drops++;
        portEXIT_CRITICAL(&echo->snapshot_lock);
    }
    return queued;
}

esp_err_t offline_echo_record_start(offline_echo_t *echo)
{
    ESP_RETURN_ON_FALSE(echo != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "missing echo handle");
    offline_echo_snapshot_t snapshot;
    offline_echo_get_snapshot(echo, &snapshot);
    const bool can_start = snapshot.state == OFFLINE_ECHO_IDLE
        || snapshot.state == OFFLINE_ECHO_PREPARING
        || snapshot.state == OFFLINE_ECHO_PLAYING;
    ESP_RETURN_ON_FALSE(!snapshot.muted && can_start,
                        ESP_ERR_INVALID_STATE, TAG, "echo is muted or recording");
    return send_command(echo, (command_t){.type = COMMAND_RECORD_START},
                        snapshot.state != OFFLINE_ECHO_IDLE);
}

esp_err_t offline_echo_record_stop(offline_echo_t *echo)
{
    return send_command(echo, (command_t){.type = COMMAND_RECORD_STOP}, false);
}

esp_err_t offline_echo_set_muted(offline_echo_t *echo, bool muted)
{
    ESP_RETURN_ON_FALSE(echo != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "missing echo handle");
    portENTER_CRITICAL(&echo->snapshot_lock);
    echo->snapshot.muted = muted;
    portEXIT_CRITICAL(&echo->snapshot_lock);
    return send_command(echo,
                        (command_t){.type = COMMAND_SET_MUTED, .muted = muted},
                        true);
}

esp_err_t offline_echo_set_output_volume(offline_echo_t *echo,
                                         uint8_t volume_percent)
{
    ESP_RETURN_ON_FALSE(echo != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "missing echo handle");
    ESP_RETURN_ON_FALSE(volume_percent >= MIN_OUTPUT_VOLUME_PERCENT
                        && volume_percent <= MAX_OUTPUT_VOLUME_PERCENT,
                        ESP_ERR_INVALID_ARG, TAG, "volume must be 10–100%%");
    return send_command(
        echo,
        (command_t){
            .type = COMMAND_SET_VOLUME,
            .volume_percent = volume_percent,
        },
        false);
}

void offline_echo_get_snapshot(offline_echo_t *echo,
                               offline_echo_snapshot_t *snapshot)
{
    if (echo == NULL || snapshot == NULL) {
        return;
    }
    portENTER_CRITICAL(&echo->snapshot_lock);
    *snapshot = echo->snapshot;
    portEXIT_CRITICAL(&echo->snapshot_lock);
}
