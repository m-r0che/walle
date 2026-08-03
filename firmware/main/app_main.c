#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "display_port.h"
#include "esp_log.h"
#include "face.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "motion_sensor.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "network_relay.h"
#include "offline_echo.h"

static const char *TAG = "walle";

#define DEFAULT_OUTPUT_VOLUME 20
#define MIN_OUTPUT_VOLUME 10
#define MAX_OUTPUT_VOLUME 100
#define VOLUME_STEP 10
#define SETTINGS_NAMESPACE "walle"
#define VOLUME_KEY "volume"
#define SLEEP_AFTER_STILL_MS 60000

typedef struct {
    face_t *face;
    offline_echo_t *echo;
    network_relay_t *network;
    motion_sensor_t *motion;
    lv_obj_t *volume_label;
    lv_obj_t *volume_down_button;
    lv_obj_t *volume_up_button;
    lv_timer_t *volume_hide_timer;
    uint8_t output_volume;
    nvs_handle_t settings;
    bool settings_open;
} app_context_t;

static uint8_t load_output_volume(app_context_t *context)
{
    esp_err_t error = nvs_flash_init();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "NVS unavailable: %s", esp_err_to_name(error));
        return DEFAULT_OUTPUT_VOLUME;
    }
    error = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &context->settings);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Settings unavailable: %s", esp_err_to_name(error));
        return DEFAULT_OUTPUT_VOLUME;
    }
    context->settings_open = true;

    uint8_t volume = DEFAULT_OUTPUT_VOLUME;
    error = nvs_get_u8(context->settings, VOLUME_KEY, &volume);
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Volume setting unreadable: %s", esp_err_to_name(error));
        return DEFAULT_OUTPUT_VOLUME;
    }
    if (volume < MIN_OUTPUT_VOLUME || volume > MAX_OUTPUT_VOLUME
            || volume % VOLUME_STEP != 0) {
        return DEFAULT_OUTPUT_VOLUME;
    }
    return volume;
}

static void persist_output_volume(app_context_t *context)
{
    if (!context->settings_open) {
        return;
    }
    esp_err_t error = nvs_set_u8(context->settings, VOLUME_KEY,
                                 context->output_volume);
    if (error == ESP_OK) {
        error = nvs_commit(context->settings);
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Volume setting was not saved: %s",
                 esp_err_to_name(error));
    }
}

static void update_volume_label(app_context_t *context)
{
    if (context->volume_label != NULL) {
        lv_label_set_text_fmt(context->volume_label, "VOL %u",
                              (unsigned)context->output_volume);
    }
}

static void hide_volume_label(lv_timer_t *timer)
{
    app_context_t *context = lv_timer_get_user_data(timer);
    if (context->volume_label != NULL) {
        lv_obj_add_flag(context->volume_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_pause(timer);
}

static void handle_volume_button(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) {
        return;
    }
    app_context_t *context = lv_event_get_user_data(event);
    const bool increase = lv_event_get_target(event)
        == context->volume_up_button;
    uint8_t next_volume = context->output_volume;
    if (increase && next_volume < MAX_OUTPUT_VOLUME) {
        next_volume += VOLUME_STEP;
    } else if (!increase && next_volume > MIN_OUTPUT_VOLUME) {
        next_volume -= VOLUME_STEP;
    }
    if (next_volume == context->output_volume) {
        return;
    }

    const esp_err_t error = offline_echo_set_output_volume(
        context->echo, next_volume);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Volume change failed: %s", esp_err_to_name(error));
        return;
    }
    context->output_volume = next_volume;
    update_volume_label(context);
    lv_obj_clear_flag(context->volume_label, LV_OBJ_FLAG_HIDDEN);
    if (context->volume_hide_timer != NULL) {
        lv_timer_reset(context->volume_hide_timer);
        lv_timer_resume(context->volume_hide_timer);
    }
    persist_output_volume(context);
    ESP_LOGI(TAG, "Volume selected=%u%%", (unsigned)next_volume);
}

static lv_obj_t *create_control_button(lv_obj_t *parent, const char *text,
                                       int32_t x, int32_t width,
                                       lv_event_cb_t callback,
                                       app_context_t *context)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, width, 30);
    lv_obj_set_pos(button, x, 326);
    lv_obj_set_ext_click_area(button, 22);
    lv_obj_set_style_radius(button, 15, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(button, callback, LV_EVENT_PRESSED, context);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xa8ffff), LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

static bool start_volume_controls(app_context_t *context)
{
    if (!bsp_display_lock(1000)) {
        return false;
    }
    lv_obj_t *screen = lv_screen_active();
    context->volume_down_button = create_control_button(
        screen, "-", 20, 48, handle_volume_button, context);
    context->volume_up_button = create_control_button(
        screen, "+", 380, 48, handle_volume_button, context);
    context->volume_label = lv_label_create(screen);
    lv_obj_set_width(context->volume_label, 96);
    lv_obj_set_pos(context->volume_label, 176, 332);
    lv_obj_set_style_text_align(context->volume_label, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(context->volume_label,
                                lv_color_hex(0x68d9e3), LV_PART_MAIN);
    update_volume_label(context);
    lv_obj_add_flag(context->volume_label, LV_OBJ_FLAG_HIDDEN);
    context->volume_hide_timer = lv_timer_create(
        hide_volume_label, 1800, context);
    lv_timer_pause(context->volume_hide_timer);
    bsp_display_unlock();
    return true;
}

static void handle_face_input(face_input_event_t event, void *opaque_context)
{
    app_context_t *context = opaque_context;
    esp_err_t error = ESP_OK;

    switch (event) {
    case FACE_INPUT_PTT_START:
        error = offline_echo_record_start(context->echo);
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "PTT pressed");
        }
        break;
    case FACE_INPUT_PTT_STOP:
        error = offline_echo_record_stop(context->echo);
        if (error == ESP_OK) {
            ESP_LOGI(TAG, "PTT released");
        }
        break;
    }

    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Input event %d failed: %s", event, esp_err_to_name(error));
    }
}

static bool handle_committed_audio_stream(
    void *opaque_context, offline_echo_stream_event_t event,
    uint32_t turn_token, const int16_t *samples, size_t sample_count)
{
    app_context_t *context = opaque_context;
    if (context == NULL || context->network == NULL) {
        return false;
    }

    network_relay_capture_event_t relay_event;
    switch (event) {
    case OFFLINE_ECHO_STREAM_START:
        relay_event = NETWORK_RELAY_CAPTURE_START;
        break;
    case OFFLINE_ECHO_STREAM_AUDIO:
        relay_event = NETWORK_RELAY_CAPTURE_AUDIO;
        break;
    case OFFLINE_ECHO_STREAM_COMMIT:
        relay_event = NETWORK_RELAY_CAPTURE_COMMIT;
        break;
    case OFFLINE_ECHO_STREAM_CANCEL:
        relay_event = NETWORK_RELAY_CAPTURE_CANCEL;
        break;
    case OFFLINE_ECHO_STREAM_REMOTE_PLAYED:
        return network_relay_report_playback(
            context->network, turn_token, true, sample_count);
    case OFFLINE_ECHO_STREAM_LOCAL_FALLBACK:
        return network_relay_report_playback(
            context->network, turn_token, false, sample_count);
    case OFFLINE_ECHO_STREAM_RESPONSE_CANCEL:
        return network_relay_cancel_response(
            context->network, turn_token);
    default:
        return false;
    }
    return network_relay_capture(context->network, relay_event,
                                 turn_token, samples, sample_count);
}

static bool handle_remote_output(
    void *opaque_context, network_relay_output_event_t event,
    uint32_t turn_token, const int16_t *samples,
    size_t sample_count, uint32_t value_count,
    uint32_t input_count)
{
    app_context_t *context = opaque_context;
    if (context == NULL || context->echo == NULL) {
        return false;
    }
    offline_echo_remote_event_t remote_event;
    switch (event) {
    case NETWORK_RELAY_OUTPUT_AUDIO:
        remote_event = OFFLINE_ECHO_REMOTE_AUDIO;
        break;
    case NETWORK_RELAY_OUTPUT_DONE:
        remote_event = OFFLINE_ECHO_REMOTE_DONE;
        break;
    case NETWORK_RELAY_OUTPUT_CANCELLED:
        remote_event = OFFLINE_ECHO_REMOTE_CANCELLED;
        break;
    case NETWORK_RELAY_OUTPUT_INVALID:
        remote_event = OFFLINE_ECHO_REMOTE_INVALID;
        break;
    default:
        return false;
    }
    return offline_echo_receive_remote(
        context->echo, remote_event, turn_token, samples,
        sample_count, value_count, input_count);
}

static face_activity_t activity_for_audio(offline_echo_state_t state)
{
    switch (state) {
    case OFFLINE_ECHO_RECORDING:
        return FACE_ACTIVITY_LISTENING;
    case OFFLINE_ECHO_PREPARING:
        return FACE_ACTIVITY_THINKING;
    case OFFLINE_ECHO_PLAYING:
        return FACE_ACTIVITY_SPEAKING;
    case OFFLINE_ECHO_ERROR:
        return FACE_ACTIVITY_ERROR;
    case OFFLINE_ECHO_IDLE:
    default:
        return FACE_ACTIVITY_IDLE;
    }
}

static face_t *start_face(void)
{
    lv_display_t *display = display_port_start();
    if (display == NULL) {
        return NULL;
    }
    if (!bsp_display_lock(1000)) {
        return NULL;
    }
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    face_t *face = face_create(screen);
    bsp_display_unlock();
    return face;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting offline push-to-talk echo prototype");

    app_context_t context = {0};
    context.output_volume = DEFAULT_OUTPUT_VOLUME;
    context.face = start_face();
    if (context.face == NULL) {
        ESP_LOGE(TAG, "Face initialization failed");
        abort();
    }

    context.output_volume = load_output_volume(&context);
    const esp_err_t audio_error = offline_echo_create(&context.echo);
    if (audio_error != ESP_OK) {
        ESP_LOGE(TAG, "Audio initialization failed: %s",
                 esp_err_to_name(audio_error));
        face_set_activity(context.face, FACE_ACTIVITY_ERROR);
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    if (context.output_volume != DEFAULT_OUTPUT_VOLUME) {
        const esp_err_t volume_error = offline_echo_set_output_volume(
            context.echo, context.output_volume);
        if (volume_error != ESP_OK) {
            ESP_LOGW(TAG, "Saved volume could not be applied: %s",
                     esp_err_to_name(volume_error));
            context.output_volume = DEFAULT_OUTPUT_VOLUME;
        }
    }
    face_set_input_callback(context.face, handle_face_input, &context);
    if (!start_volume_controls(&context)) {
        ESP_LOGW(TAG, "Volume controls could not be created");
    }
    // A second reset/init after the first visible LVGL frame reproducibly
    // blanks the CO5300 on a cold power-on. The initial display_port_start()
    // initialization is authoritative; transfer pacing begins immediately.
    ESP_LOGI(TAG, "Ready: hold face for PTT; corner buttons set volume (%u%%)",
             (unsigned)context.output_volume);

    const esp_err_t motion_error = motion_sensor_create(&context.motion);
    if (motion_error != ESP_OK) {
        ESP_LOGW(TAG, "Motion sensor initialization failed: %s",
                 esp_err_to_name(motion_error));
    }

    const esp_err_t network_error = network_relay_create(&context.network);
    if (network_error != ESP_OK && network_error != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "Network relay initialization failed: %s",
                 esp_err_to_name(network_error));
    } else if (context.network != NULL) {
        const esp_err_t output_error = network_relay_set_output_sink(
            context.network, handle_remote_output, &context);
        if (output_error != ESP_OK) {
            ESP_LOGW(TAG, "Remote output observer failed: %s",
                     esp_err_to_name(output_error));
        }
        const esp_err_t sink_error = offline_echo_set_stream_sink(
            context.echo, handle_committed_audio_stream, &context);
        if (sink_error != ESP_OK) {
            ESP_LOGW(TAG, "Capture stream observer failed: %s",
                     esp_err_to_name(sink_error));
        }
    }

    offline_echo_state_t previous_state = OFFLINE_ECHO_ERROR;
    uint32_t observed_motion_events = 0;
    bool sleeping = false;
    TickType_t last_presence = xTaskGetTickCount();
    TickType_t next_display_report = last_presence + pdMS_TO_TICKS(5000);
    while (true) {
        const TickType_t now = xTaskGetTickCount();
        offline_echo_snapshot_t snapshot;
        offline_echo_get_snapshot(context.echo, &snapshot);
        motion_sensor_snapshot_t motion_snapshot;
        motion_sensor_get_snapshot(context.motion, &motion_snapshot);

        if (motion_snapshot.motion_events != observed_motion_events) {
            observed_motion_events = motion_snapshot.motion_events;
            last_presence = now;
            if (sleeping) {
                sleeping = false;
                face_react(context.face, FACE_REACTION_FOCUS, 0.92f);
                ESP_LOGI(TAG, "Woke from local motion");
            }
        }
        if (snapshot.state != OFFLINE_ECHO_IDLE) {
            last_presence = now;
            sleeping = false;
        } else if (!sleeping && motion_snapshot.ready
                   && (now - last_presence)
                       >= pdMS_TO_TICKS(SLEEP_AFTER_STILL_MS)) {
            sleeping = true;
            ESP_LOGI(TAG, "Sleeping after local stillness");
        }

        face_set_activity(context.face, sleeping
                          ? FACE_ACTIVITY_SLEEPING
                          : activity_for_audio(snapshot.state));
        face_set_playback_level(context.face, snapshot.playback_level);

        if (snapshot.state != previous_state) {
            if (snapshot.state == OFFLINE_ECHO_PLAYING
                    && previous_state == OFFLINE_ECHO_PREPARING) {
                face_react(context.face, FACE_REACTION_REALISE, 0.62f);
            }
            ESP_LOGI(TAG,
                     "Audio state=%d committed=%d recorded=%ums rings=%u/%u/%ums overruns=%u underruns=%u muted=%d read_errors=%u write_errors=%u stream=%u/%u remote=%u/%u playback=%u/%u timeout=%u",
                     snapshot.state, snapshot.recording_committed,
                     (unsigned)snapshot.recorded_ms,
                     (unsigned)snapshot.precommit_buffered_ms,
                     (unsigned)snapshot.capture_queued_ms,
                     (unsigned)snapshot.playback_queued_ms,
                     (unsigned)snapshot.capture_overruns,
                     (unsigned)snapshot.playback_underruns,
                     snapshot.muted, (unsigned)snapshot.read_errors,
                     (unsigned)snapshot.write_errors,
                     (unsigned)snapshot.stream_audio_frames,
                     (unsigned)snapshot.stream_drops,
                     (unsigned)snapshot.remote_audio_frames,
                     (unsigned)snapshot.remote_event_drops,
                     (unsigned)snapshot.remote_playbacks,
                     (unsigned)snapshot.local_fallbacks,
                     (unsigned)snapshot.remote_timeouts);
            previous_state = snapshot.state;
        }

        if ((int32_t)(now - next_display_report) >= 0) {
            display_port_snapshot_t display_snapshot;
            display_port_get_snapshot(&display_snapshot);
            ESP_LOGI(TAG,
                     "Display submitted=%u completed=%u submit_errors=%u overlaps=%u paced=%u wait_ms=%u motion=%d/%u/%.2f/%u sleeping=%d",
                     (unsigned)display_snapshot.submitted,
                     (unsigned)display_snapshot.completed,
                     (unsigned)display_snapshot.submit_errors,
                     (unsigned)display_snapshot.overlap_errors,
                     (unsigned)display_snapshot.pacing_delays,
                     (unsigned)display_snapshot.pacing_wait_ms,
                     motion_snapshot.ready,
                     (unsigned)motion_snapshot.motion_events,
                     motion_snapshot.motion_score,
                     (unsigned)motion_snapshot.read_errors,
                     sleeping);
            if (context.network != NULL) {
                network_relay_snapshot_t network_snapshot;
                network_relay_get_snapshot(context.network, &network_snapshot);
                ESP_LOGI(TAG,
                         "Network state=%d selected=%d rssi=%d wifi=%u/%u socket=%u/%u restart=%u epoch=%u timeout=%u/%u heartbeat=%u/%u stale=%u protocol_errors=%u turns=%u/%u/%u audio=%u/%u/%u echo=%u/%u output=%u/%u/%u reports=%u/%u queue_high=%u internal=%u minimum=%u",
                         network_snapshot.state,
                         network_snapshot.active_network + 1,
                         network_snapshot.rssi,
                         (unsigned)network_snapshot.wifi_connects,
                         (unsigned)network_snapshot.wifi_disconnects,
                         (unsigned)network_snapshot.socket_connects,
                         (unsigned)network_snapshot.socket_disconnects,
                         (unsigned)network_snapshot.socket_restarts,
                         (unsigned)network_snapshot.connection_epoch,
                         (unsigned)network_snapshot.connect_timeouts,
                         (unsigned)network_snapshot.ready_timeouts,
                         (unsigned)network_snapshot.heartbeat_pongs,
                         (unsigned)network_snapshot.heartbeat_timeouts,
                         (unsigned)network_snapshot.stale_socket_events,
                         (unsigned)network_snapshot.protocol_errors,
                         (unsigned)network_snapshot.turns_started,
                         (unsigned)network_snapshot.turns_committed,
                         (unsigned)network_snapshot.turns_cancelled,
                         (unsigned)network_snapshot.audio_frames_queued,
                         (unsigned)network_snapshot.audio_frames_sent,
                         (unsigned)network_snapshot.audio_frames_dropped,
                         (unsigned)network_snapshot.echo_frames_received,
                         (unsigned)network_snapshot.echo_mismatches,
                         (unsigned)network_snapshot.output_frames_forwarded,
                         (unsigned)network_snapshot.output_events_dropped,
                         (unsigned)network_snapshot.output_turns_done,
                         (unsigned)network_snapshot.remote_playback_reports,
                         (unsigned)network_snapshot.local_fallback_reports,
                         (unsigned)network_snapshot.stream_queue_high_water,
                         (unsigned)network_snapshot.free_internal_bytes,
                         (unsigned)network_snapshot.minimum_internal_bytes);
            }
            next_display_report = now + pdMS_TO_TICKS(5000);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
