#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_duplex";

#define SAMPLE_RATE_HZ 24000
#define BITS_PER_SAMPLE 16
#define CHANNEL_COUNT 1
#define FRAME_SAMPLES 256
#define SETTLE_FRAMES 32
#define TEST_SECONDS 4
#define TONE_SECONDS 2
#define TONE_FREQUENCY_HZ 440
#define OUTPUT_VOLUME_PERCENT 20
#define INPUT_GAIN_DB 24.0f
#define TWO_PI 6.28318530717958647692f

typedef enum {
    PHASE_BASELINE = 0,
    PHASE_TONE,
    PHASE_TAIL,
    PHASE_COUNT,
} audio_phase_t;

typedef struct {
    uint64_t sum_squares;
    uint64_t sample_count;
    uint32_t peak;
} phase_metrics_t;

static i2s_chan_handle_t s_tx_handle;
static i2s_chan_handle_t s_rx_handle;
static esp_codec_dev_handle_t s_codec;
static volatile audio_phase_t s_phase = PHASE_BASELINE;
static volatile bool s_tx_done;
static volatile bool s_rx_done;
static phase_metrics_t s_metrics[PHASE_COUNT];
static uint32_t s_read_errors;
static uint32_t s_write_errors;

static const char *phase_name(audio_phase_t phase)
{
    switch (phase) {
    case PHASE_BASELINE:
        return "baseline";
    case PHASE_TONE:
        return "tone";
    case PHASE_TAIL:
        return "tail";
    default:
        return "unknown";
    }
}

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

static esp_err_t initialize_codec(void)
{
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel_config.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx_handle, &s_rx_handle), TAG,
                        "create duplex I2S channels");

    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
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
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &i2s_config), TAG, "initialize I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &i2s_config), TAG, "initialize I2S RX");

    audio_codec_i2s_cfg_t data_config = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_config);
    ESP_RETURN_ON_FALSE(data_if != NULL, ESP_ERR_NO_MEM, TAG, "create I2S codec interface");

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "initialize board I2C");
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec GPIO interface");

    audio_codec_i2c_cfg_t control_config = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *control_if = audio_codec_new_i2c_ctrl(&control_config);
    ESP_RETURN_ON_FALSE(control_if != NULL, ESP_ERR_NO_MEM, TAG, "create codec control interface");

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
    ESP_RETURN_ON_FALSE(codec_if != NULL, ESP_ERR_NO_MEM, TAG, "create ES8311 interface");

    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&device_config);
    ESP_RETURN_ON_FALSE(s_codec != NULL, ESP_ERR_NO_MEM, TAG, "create duplex codec device");

    esp_codec_dev_sample_info_t format = sample_info();
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &format) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG,
                        "open duplex codec");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_codec, OUTPUT_VOLUME_PERCENT) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set output volume");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec, INPUT_GAIN_DB) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "set input gain");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_mute(s_codec, false) == ESP_CODEC_DEV_OK,
                        ESP_FAIL, TAG, "unmute output");
    // ES8311 input mute is not exposed by this esp_codec_dev implementation.
    // Opening the codec in BOTH mode enables the input path.
    return ESP_OK;
}

static void add_metrics(audio_phase_t phase, const int16_t *samples, size_t sample_count)
{
    phase_metrics_t *metrics = &s_metrics[phase];
    for (size_t index = 0; index < sample_count; index++) {
        const int32_t value = samples[index];
        const uint32_t magnitude = value < 0 ? (uint32_t)(-value) : (uint32_t)value;
        metrics->sum_squares += (uint64_t)((int64_t)value * value);
        metrics->sample_count++;
        if (magnitude > metrics->peak) {
            metrics->peak = magnitude;
        }
    }
}

static void microphone_task(void *unused)
{
    (void)unused;
    int16_t samples[FRAME_SAMPLES];
    for (int frame = 0; frame < SETTLE_FRAMES; frame++) {
        if (esp_codec_dev_read(s_codec, samples, sizeof(samples)) != ESP_CODEC_DEV_OK) {
            s_read_errors++;
        }
    }
    ESP_LOGI(TAG, "Microphone startup samples discarded");

    uint64_t window_sum_squares = 0;
    uint32_t window_samples = 0;
    uint32_t window_peak = 0;
    const int total_frames = (SAMPLE_RATE_HZ * TEST_SECONDS) / FRAME_SAMPLES;
    const int log_every_frames = SAMPLE_RATE_HZ / FRAME_SAMPLES / 4;

    for (int frame = 0; frame < total_frames; frame++) {
        const int result = esp_codec_dev_read(s_codec, samples, sizeof(samples));
        if (result != ESP_CODEC_DEV_OK) {
            s_read_errors++;
            ESP_LOGE(TAG, "microphone read failed: %d", result);
            continue;
        }

        const audio_phase_t phase = s_phase;
        add_metrics(phase, samples, FRAME_SAMPLES);
        for (size_t index = 0; index < FRAME_SAMPLES; index++) {
            const int32_t value = samples[index];
            const uint32_t magnitude = value < 0 ? (uint32_t)(-value) : (uint32_t)value;
            window_sum_squares += (uint64_t)((int64_t)value * value);
            window_samples++;
            if (magnitude > window_peak) {
                window_peak = magnitude;
            }
        }

        if ((frame + 1) % log_every_frames == 0) {
            const double rms = sqrt((double)window_sum_squares / window_samples);
            ESP_LOGI(TAG, "mic phase=%s rms=%.1f peak=%" PRIu32, phase_name(phase), rms, window_peak);
            window_sum_squares = 0;
            window_samples = 0;
            window_peak = 0;
        }
    }

    s_rx_done = true;
    vTaskDelete(NULL);
}

static void speaker_task(void *unused)
{
    (void)unused;
    int16_t samples[FRAME_SAMPLES];
    float phase = 0.0f;
    const float step = TWO_PI * TONE_FREQUENCY_HZ / SAMPLE_RATE_HZ;
    const int total_frames = (SAMPLE_RATE_HZ * TONE_SECONDS) / FRAME_SAMPLES;

    for (int frame = 0; frame < total_frames; frame++) {
        for (size_t index = 0; index < FRAME_SAMPLES; index++) {
            samples[index] = (int16_t)(sinf(phase) * 8000.0f);
            phase += step;
            if (phase >= TWO_PI) {
                phase -= TWO_PI;
            }
        }
        const int result = esp_codec_dev_write(s_codec, samples, sizeof(samples));
        if (result != ESP_CODEC_DEV_OK) {
            s_write_errors++;
            ESP_LOGE(TAG, "speaker write failed: %d", result);
            break;
        }
    }

    esp_codec_dev_set_out_mute(s_codec, true);
    s_tx_done = true;
    vTaskDelete(NULL);
}

static double metrics_rms(const phase_metrics_t *metrics)
{
    return metrics->sample_count == 0
        ? 0.0
        : sqrt((double)metrics->sum_squares / metrics->sample_count);
}

static void log_summary(void)
{
    for (audio_phase_t phase = PHASE_BASELINE; phase < PHASE_COUNT; phase++) {
        const phase_metrics_t *metrics = &s_metrics[phase];
        const double rms = metrics_rms(metrics);
        ESP_LOGI(TAG, "summary phase=%s samples=%llu rms=%.1f peak=%" PRIu32,
                 phase_name(phase), (unsigned long long)metrics->sample_count, rms, metrics->peak);
    }
    ESP_LOGI(TAG, "summary read_errors=%" PRIu32 " write_errors=%" PRIu32,
             s_read_errors, s_write_errors);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting finite 24 kHz duplex audio diagnostic");
    ESP_LOGI(TAG, "Output volume=%d%% input gain=%.1f dB", OUTPUT_VOLUME_PERCENT, INPUT_GAIN_DB);
    ESP_ERROR_CHECK(initialize_codec());

    xTaskCreatePinnedToCore(microphone_task, "microphone", 4096, NULL, 6, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));

    s_phase = PHASE_TONE;
    xTaskCreatePinnedToCore(speaker_task, "speaker", 4096, NULL, 6, NULL, 1);
    while (!s_tx_done) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_phase = PHASE_TAIL;

    while (!s_rx_done) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    log_summary();
    const double baseline_rms = metrics_rms(&s_metrics[PHASE_BASELINE]);
    const double tone_rms = metrics_rms(&s_metrics[PHASE_TONE]);
    const bool passed = s_read_errors == 0 && s_write_errors == 0 &&
                        s_metrics[PHASE_BASELINE].sample_count > 0 &&
                        s_metrics[PHASE_TONE].sample_count > 0 &&
                        tone_rms > baseline_rms + 100.0;
    ESP_LOGI(TAG, "AUDIO_DUPLEX_RESULT=%s", passed ? "PASS" : "FAIL");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
