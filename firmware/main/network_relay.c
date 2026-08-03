#include "network_relay.h"

#include "pcm_batcher.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if __has_include("credentials.h")
#include "credentials.h"
#define WALLE_HAS_CREDENTIALS 1
#else
#define WALLE_HAS_CREDENTIALS 0
#endif

#define WIFI_STARTED_BIT BIT0
#define WIFI_GOT_IP_BIT BIT1
#define WIFI_DISCONNECTED_BIT BIT2
#define SOCKET_CONNECTED_BIT BIT3
#define SOCKET_READY_BIT BIT4
#define SOCKET_DISCONNECTED_BIT BIT5
#define SOCKET_RESTART_BIT BIT6
#define SOCKET_PONG_BIT BIT7

#define MAX_SCAN_RESULTS 48
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define RESCAN_DELAY_MS 5000
#define SOCKET_CONNECT_DEADLINE_MS 20000
#define PROTOCOL_READY_DEADLINE_MS 15000
#define HEARTBEAT_INTERVAL_MS 10000
#define HEARTBEAT_TIMEOUT_MS 12000
/* Refresh before the documented 60-minute OpenAI Realtime session limit. */
#define SOCKET_MAX_LIFETIME_MS (55 * 60 * 1000)
#define STABLE_CONNECTION_MS 30000
#define RECONNECT_BASE_MS 1000
#define RECONNECT_CAP_MS 30000
#define PLANNED_REFRESH_DELAY_MS 250
#define SOCKET_BUFFER_BYTES 2048
#define CONTROL_BUFFER_BYTES 512
#define MANAGER_STACK_BYTES 5120
#define WEBSOCKET_STACK_BYTES 6144
#define AUDIO_HEADER_BYTES 12
#define CAPTURE_CHUNK_SAMPLES 256
#define NETWORK_FRAME_SAMPLES 960
#define MAX_INPUT_TURN_SAMPLES 720000
#define MAX_OUTPUT_TURN_SAMPLES 7200000
#define MAX_PLAYBACK_START_LATENCY_MS 330000
#define STREAM_QUEUE_LENGTH 640
#define STREAM_CONTROL_RESERVE 2
#define EXPECTED_ECHO_SLOTS 64

_Static_assert(CAPTURE_CHUNK_SAMPLES <= NETWORK_FRAME_SAMPLES,
               "capture chunks must fit network frames");
_Static_assert(AUDIO_HEADER_BYTES
                       + NETWORK_FRAME_SAMPLES * sizeof(int16_t)
                   <= SOCKET_BUFFER_BYTES,
               "network audio frames must fit the WebSocket buffer");

static const char *TAG = "network_relay";

#if WALLE_HAS_CREDENTIALS
#if WALLE_WIFI_NETWORK_COUNT < 1 || WALLE_WIFI_NETWORK_COUNT > 2
#error "The current credential generator supports one or two Wi-Fi networks"
#endif

typedef struct {
    const char *ssid;
    const char *password;
} known_network_t;

static const known_network_t s_known_networks[] = {
    {WALLE_WIFI_SSID_0, WALLE_WIFI_PASSWORD_0},
#if WALLE_WIFI_NETWORK_COUNT >= 2
    {WALLE_WIFI_SSID_1, WALLE_WIFI_PASSWORD_1},
#endif
};

_Static_assert(
    sizeof(s_known_networks) / sizeof(s_known_networks[0])
        == WALLE_WIFI_NETWORK_COUNT,
    "credential count does not match network definitions");
#endif

typedef enum {
    STREAM_EVENT_CAPTURE_START = NETWORK_RELAY_CAPTURE_START,
    STREAM_EVENT_CAPTURE_AUDIO = NETWORK_RELAY_CAPTURE_AUDIO,
    STREAM_EVENT_CAPTURE_COMMIT = NETWORK_RELAY_CAPTURE_COMMIT,
    STREAM_EVENT_CAPTURE_CANCEL = NETWORK_RELAY_CAPTURE_CANCEL,
    STREAM_EVENT_RESPONSE_CANCEL,
    STREAM_EVENT_REMOTE_PLAYED,
    STREAM_EVENT_LOCAL_FALLBACK,
} stream_event_t;

typedef struct {
    stream_event_t event;
    uint16_t sample_count;
    uint32_t epoch;
    uint32_t turn_token;
    uint32_t value_count;
    uint32_t first_codec_write_ms;
    int16_t samples[CAPTURE_CHUNK_SAMPLES];
} stream_item_t;

typedef struct {
    uint32_t sequence;
    uint32_t hash;
    uint32_t epoch;
    uint16_t sample_count;
    bool valid;
} expected_echo_t;

struct network_relay;

typedef struct {
    struct network_relay *relay;
    uint32_t epoch;
    atomic_bool disconnect_reported;
} websocket_session_t;

struct network_relay {
    EventGroupHandle_t events;
    esp_netif_t *station_netif;
    esp_websocket_client_handle_t websocket;
    websocket_session_t *websocket_session;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    TaskHandle_t task;
    portMUX_TYPE snapshot_lock;
    portMUX_TYPE stream_lock;
    network_relay_snapshot_t snapshot;
    QueueHandle_t stream_queue;
    SemaphoreHandle_t stream_mutex;
    StaticQueue_t stream_queue_control;
    uint8_t *stream_queue_storage;
    atomic_bool capture_accepting;
    atomic_bool stream_turn_active;
    atomic_uint active_epoch;
    atomic_uint heartbeat_nonce;
    atomic_uint active_output_token;
    atomic_uint completed_output_token;
    atomic_uint output_input_samples;
    atomic_uint next_output_sequence;
    atomic_bool output_turn_active;
    atomic_bool generated_output_mode;
    atomic_bool buffered_output_mode;
    atomic_bool telemetry_dirty;
    uint32_t epoch_counter;
    uint32_t stream_turn_epoch;
    uint32_t stream_turn_token;
    uint32_t boot_nonce;
    uint32_t turn_counter;
    uint32_t next_input_sequence;
    expected_echo_t expected_echoes[EXPECTED_ECHO_SLOTS];
    pcm_batcher_t pcm_batcher;
    uint8_t *audio_tx_frame;
    network_relay_output_sink_t output_sink;
    void *output_context;
    char active_turn_id[64];
    char output_turn_id[64];
    char relay_uri[384];
    char authorization_header[640];
    char control_rx[CONTROL_BUFFER_BYTES];
};

static void set_state(network_relay_t *relay, network_relay_state_t state)
{
    portENTER_CRITICAL(&relay->snapshot_lock);
    relay->snapshot.state = state;
    portEXIT_CRITICAL(&relay->snapshot_lock);
}

static void set_active_network(network_relay_t *relay, int index, int rssi)
{
    portENTER_CRITICAL(&relay->snapshot_lock);
    relay->snapshot.active_network = (int8_t)index;
    relay->snapshot.rssi = (int8_t)rssi;
    portEXIT_CRITICAL(&relay->snapshot_lock);
}

static void increment_counter(network_relay_t *relay, uint32_t *counter)
{
    portENTER_CRITICAL(&relay->snapshot_lock);
    (*counter)++;
    portEXIT_CRITICAL(&relay->snapshot_lock);
}

static void set_connection_epoch(network_relay_t *relay, uint32_t epoch)
{
    portENTER_CRITICAL(&relay->snapshot_lock);
    relay->snapshot.connection_epoch = epoch;
    portEXIT_CRITICAL(&relay->snapshot_lock);
}

static bool session_is_current(const websocket_session_t *session)
{
    return session != NULL
        && session->epoch == atomic_load(&session->relay->active_epoch);
}

static uint16_t read_u16_le(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8)
        | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void write_u16_le(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t audio_hash(const uint8_t *bytes, size_t length)
{
    uint32_t hash = 2166136261U;
    for (size_t index = 0; index < length; index++) {
        hash ^= bytes[index];
        hash *= 16777619U;
    }
    return hash;
}

static void reset_stream_state(network_relay_t *relay)
{
    xSemaphoreTake(relay->stream_mutex, portMAX_DELAY);
    relay->capture_accepting = false;
    relay->stream_turn_active = false;
    relay->stream_turn_epoch = 0;
    relay->stream_turn_token = 0;
    atomic_store(&relay->output_turn_active, false);
    atomic_store(&relay->active_output_token, 0);
    atomic_store(&relay->completed_output_token, 0);
    atomic_store(&relay->output_input_samples, 0);
    atomic_store(&relay->next_output_sequence, 0);
    atomic_store(&relay->generated_output_mode, false);
    atomic_store(&relay->buffered_output_mode, false);
    pcm_batcher_abort(&relay->pcm_batcher);
    xQueueReset(relay->stream_queue);
    xSemaphoreGive(relay->stream_mutex);

    portENTER_CRITICAL(&relay->stream_lock);
    for (size_t index = 0; index < EXPECTED_ECHO_SLOTS; index++) {
        relay->expected_echoes[index].valid = false;
    }
    portEXIT_CRITICAL(&relay->stream_lock);
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    network_relay_t *relay = argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        xEventGroupSetBits(relay->events, WIFI_STARTED_BIT);
        return;
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        increment_counter(relay, &relay->snapshot.wifi_disconnects);
        xEventGroupClearBits(relay->events, WIFI_GOT_IP_BIT);
        xEventGroupSetBits(relay->events, WIFI_DISCONNECTED_BIT);
        ESP_LOGW(TAG, "Wi-Fi disconnected reason=%u",
                 disconnected == NULL ? 0U : (unsigned)disconnected->reason);
        return;
    }
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        increment_counter(relay, &relay->snapshot.wifi_connects);
        xEventGroupClearBits(relay->events, WIFI_DISCONNECTED_BIT);
        xEventGroupSetBits(relay->events, WIFI_GOT_IP_BIT);
        set_state(relay, NETWORK_RELAY_WIFI_CONNECTED);
        ESP_LOGI(TAG, "Wi-Fi obtained an IP address");
    }
}

static bool emit_output_event(network_relay_t *relay,
                              network_relay_output_event_t event,
                              uint32_t turn_token,
                              const int16_t *samples,
                              size_t sample_count,
                              uint32_t value_count,
                              uint32_t input_count)
{
    network_relay_output_sink_t sink;
    void *context;
    portENTER_CRITICAL(&relay->stream_lock);
    sink = relay->output_sink;
    context = relay->output_context;
    portEXIT_CRITICAL(&relay->stream_lock);
    if (sink == NULL || turn_token == 0
            || !sink(context, event, turn_token, samples,
                     sample_count, value_count, input_count)) {
        increment_counter(relay,
                          &relay->snapshot.output_events_dropped);
        return false;
    }
    if (event == NETWORK_RELAY_OUTPUT_AUDIO) {
        increment_counter(relay,
                          &relay->snapshot.output_frames_forwarded);
    } else if (event == NETWORK_RELAY_OUTPUT_DONE) {
        increment_counter(relay, &relay->snapshot.output_turns_done);
    }
    return true;
}

static void invalidate_output_turn(network_relay_t *relay, bool notify)
{
    const bool active = atomic_exchange(
        &relay->output_turn_active, false);
    const uint32_t token = atomic_exchange(
        &relay->active_output_token, 0);
    atomic_store(&relay->completed_output_token, 0);
    atomic_store(&relay->output_input_samples, 0);
    if (active && notify) {
        emit_output_event(relay, NETWORK_RELAY_OUTPUT_INVALID,
                          token, NULL, 0, 0, 0);
    }
}

static bool control_matches_output_turn(network_relay_t *relay)
{
    if (!atomic_load(&relay->output_turn_active)) {
        return false;
    }
    char turn_id[sizeof(relay->output_turn_id)];
    portENTER_CRITICAL(&relay->stream_lock);
    memcpy(turn_id, relay->output_turn_id, sizeof(turn_id));
    portEXIT_CRITICAL(&relay->stream_lock);
    turn_id[sizeof(turn_id) - 1] = '\0';
    char expected[96];
    const int length = snprintf(
        expected, sizeof(expected), "\"turnId\":\"%s\"", turn_id);
    return length > 0 && length < (int)sizeof(expected)
        && strstr(relay->control_rx, expected) != NULL;
}

static bool control_named_count(network_relay_t *relay,
                                const char *name,
                                uint32_t maximum,
                                uint32_t *sample_count)
{
    char label[32];
    const int label_length = snprintf(
        label, sizeof(label), "\"%s\":", name);
    if (label_length <= 0 || label_length >= (int)sizeof(label)) {
        return false;
    }
    const char *field = strstr(relay->control_rx, label);
    if (field == NULL) {
        return false;
    }
    const char *digits = field + label_length;
    char *end = NULL;
    const unsigned long parsed = strtoul(digits, &end, 10);
    if (end == digits || parsed == 0 || parsed > maximum
            || (*end != ',' && *end != '}')) {
        return false;
    }
    *sample_count = (uint32_t)parsed;
    return true;
}

static void handle_control_payload(network_relay_t *relay,
                                   const esp_websocket_event_data_t *data)
{
    if (data->op_code != 0x1) {
        return;
    }
    if (data->payload_len <= 0
            || data->payload_len >= CONTROL_BUFFER_BYTES
            || data->payload_offset < 0 || data->data_len < 0
            || data->payload_offset + data->data_len > data->payload_len) {
        increment_counter(relay, &relay->snapshot.protocol_errors);
        return;
    }

    memcpy(relay->control_rx + data->payload_offset, data->data_ptr,
           (size_t)data->data_len);
    if (!data->fin
            || data->payload_offset + data->data_len != data->payload_len) {
        return;
    }
    relay->control_rx[data->payload_len] = '\0';
    if (strstr(relay->control_rx, "\"v\":1") == NULL) {
        increment_counter(relay, &relay->snapshot.protocol_errors);
        return;
    }
    if (strstr(relay->control_rx, "\"type\":\"ready\"") != NULL) {
        char expected[48];
        const unsigned epoch = atomic_load(&relay->active_epoch);
        snprintf(expected, sizeof(expected), "\"sessionEpoch\":%u", epoch);
        if (epoch == 0 || strstr(relay->control_rx, expected) == NULL) {
            increment_counter(relay, &relay->snapshot.protocol_errors);
            xEventGroupSetBits(relay->events, SOCKET_RESTART_BIT);
            return;
        }
        const bool mode_openai = strstr(
            relay->control_rx, "\"mode\":\"openai\"") != NULL;
        const bool mode_echo = strstr(
            relay->control_rx, "\"mode\":\"echo\"") != NULL;
        if (!mode_openai && !mode_echo) {
            increment_counter(relay, &relay->snapshot.protocol_errors);
            xEventGroupSetBits(relay->events, SOCKET_RESTART_BIT);
            return;
        }
        const bool buffered = mode_openai && strstr(
            relay->control_rx, "\"playback\":\"buffered\"") != NULL;
        atomic_store(&relay->generated_output_mode, mode_openai);
        atomic_store(&relay->buffered_output_mode, buffered);
        xEventGroupSetBits(relay->events, SOCKET_READY_BIT);
        set_state(relay, NETWORK_RELAY_READY);
        ESP_LOGI(TAG, "Relay protocol epoch=%u ready", epoch);
    } else if (strstr(relay->control_rx,
                      "\"type\":\"turn.done\"") != NULL) {
        uint32_t input_samples = 0;
        uint32_t output_samples = 0;
        const bool generated_counts = strstr(
            relay->control_rx, "\"inputSamples\":") != NULL;
        const bool counts_valid = generated_counts
            ? control_named_count(
                  relay, "inputSamples", MAX_INPUT_TURN_SAMPLES,
                  &input_samples)
                && control_named_count(
                    relay, "outputSamples", MAX_OUTPUT_TURN_SAMPLES,
                    &output_samples)
            : control_named_count(
                  relay, "samples", MAX_INPUT_TURN_SAMPLES,
                  &input_samples);
        if (!generated_counts) {
            output_samples = input_samples;
        }
        if (!control_matches_output_turn(relay) || !counts_valid
                || input_samples != atomic_load(
                    &relay->output_input_samples)) {
            increment_counter(relay, &relay->snapshot.protocol_errors);
            invalidate_output_turn(relay, true);
            return;
        }
        const uint32_t token = atomic_exchange(
            &relay->active_output_token, 0);
        atomic_store(&relay->output_turn_active, false);
        atomic_store(&relay->completed_output_token, token);
        emit_output_event(relay, NETWORK_RELAY_OUTPUT_DONE,
                          token, NULL, 0, output_samples,
                          input_samples);
    } else if (strstr(relay->control_rx,
                      "\"type\":\"turn.cancelled\"") != NULL) {
        if (!control_matches_output_turn(relay)) {
            increment_counter(relay, &relay->snapshot.protocol_errors);
            return;
        }
        const uint32_t token = atomic_exchange(
            &relay->active_output_token, 0);
        atomic_store(&relay->output_turn_active, false);
        atomic_store(&relay->completed_output_token, 0);
        emit_output_event(relay, NETWORK_RELAY_OUTPUT_CANCELLED,
                          token, NULL, 0, 0, 0);
    } else if (strstr(relay->control_rx, "\"type\":\"pong\"") != NULL) {
        char expected[48];
        const unsigned nonce = atomic_load(&relay->heartbeat_nonce);
        snprintf(expected, sizeof(expected), "\"nonce\":\"%08x\"", nonce);
        if (nonce != 0 && strstr(relay->control_rx, expected) != NULL) {
            xEventGroupSetBits(relay->events, SOCKET_PONG_BIT);
        } else {
            increment_counter(relay, &relay->snapshot.protocol_errors);
        }
    }
}

static void handle_audio_payload(network_relay_t *relay, uint32_t epoch,
                                 const esp_websocket_event_data_t *data)
{
    if (data->op_code != 0x2) {
        return;
    }
    if (!data->fin || data->payload_offset != 0
            || data->data_len != data->payload_len
            || data->data_len < AUDIO_HEADER_BYTES) {
        increment_counter(relay, &relay->snapshot.protocol_errors);
        return;
    }

    const uint8_t *bytes = (const uint8_t *)data->data_ptr;
    if (bytes[0] != 0x57 || bytes[1] != 0x41 || bytes[2] != 1
            || bytes[3] != 2) {
        increment_counter(relay, &relay->snapshot.protocol_errors);
        return;
    }
    const uint32_t sequence = read_u32_le(&bytes[4]);
    const uint16_t sample_count = read_u16_le(&bytes[8]);
    const size_t pcm_bytes = (size_t)sample_count * sizeof(int16_t);
    if (sample_count == 0 || sample_count > NETWORK_FRAME_SAMPLES
            || data->data_len != AUDIO_HEADER_BYTES + (int)pcm_bytes) {
        increment_counter(relay, &relay->snapshot.protocol_errors);
        return;
    }

    bool matches = false;
    if (atomic_load(&relay->generated_output_mode)) {
        const uint32_t expected_sequence = atomic_load(
            &relay->next_output_sequence);
        if (atomic_load(&relay->output_turn_active)
                && sequence == expected_sequence) {
            atomic_store(&relay->next_output_sequence,
                         expected_sequence + 1);
            matches = true;
        }
    } else {
        const uint32_t received_hash = audio_hash(
            &bytes[AUDIO_HEADER_BYTES], pcm_bytes);
        portENTER_CRITICAL(&relay->stream_lock);
        expected_echo_t *expected =
            &relay->expected_echoes[sequence % EXPECTED_ECHO_SLOTS];
        if (expected->valid && expected->sequence == sequence
                && expected->epoch == epoch
                && expected->sample_count == sample_count
                && expected->hash == received_hash) {
            matches = true;
        }
        expected->valid = false;
        portEXIT_CRITICAL(&relay->stream_lock);
    }

    if (matches) {
        increment_counter(relay, &relay->snapshot.echo_frames_received);
        if (atomic_load(&relay->output_turn_active)) {
            const uint32_t token = atomic_load(
                &relay->active_output_token);
            if (!emit_output_event(
                    relay, NETWORK_RELAY_OUTPUT_AUDIO, token,
                    (const int16_t *)&bytes[AUDIO_HEADER_BYTES],
                    sample_count, 0,
                    atomic_load(&relay->buffered_output_mode) ? 1U : 0U)) {
                invalidate_output_turn(relay, false);
            }
        } else {
            increment_counter(relay,
                              &relay->snapshot.output_events_dropped);
        }
    } else {
        increment_counter(relay, &relay->snapshot.echo_mismatches);
        invalidate_output_turn(relay, true);
    }
}

static void websocket_event_handler(void *argument, esp_event_base_t event_base,
                                    int32_t event_id, void *event_data)
{
    (void)event_base;
    websocket_session_t *session = argument;
    network_relay_t *relay = session->relay;
    esp_websocket_event_data_t *data = event_data;

    if (!session_is_current(session)) {
        if (event_id != WEBSOCKET_EVENT_FINISH) {
            increment_counter(relay, &relay->snapshot.stale_socket_events);
        }
        return;
    }

    switch ((esp_websocket_event_id_t)event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        increment_counter(relay, &relay->snapshot.socket_connects);
        xEventGroupClearBits(relay->events,
                             SOCKET_DISCONNECTED_BIT | SOCKET_READY_BIT
                                 | SOCKET_RESTART_BIT | SOCKET_PONG_BIT);
        xEventGroupSetBits(relay->events, SOCKET_CONNECTED_BIT);
        set_state(relay, NETWORK_RELAY_SOCKET_CONNECTED);
        ESP_LOGI(TAG, "Authenticated WSS epoch=%u established",
                 (unsigned)session->epoch);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (!atomic_exchange(&session->disconnect_reported, true)) {
            increment_counter(relay, &relay->snapshot.socket_disconnects);
            xEventGroupClearBits(relay->events,
                                 SOCKET_CONNECTED_BIT | SOCKET_READY_BIT
                                     | SOCKET_PONG_BIT);
            xEventGroupSetBits(relay->events,
                               SOCKET_DISCONNECTED_BIT | SOCKET_RESTART_BIT);
            set_state(relay, NETWORK_RELAY_SOCKET_CONNECTING);
            ESP_LOGW(TAG, "WSS epoch=%u closed",
                     (unsigned)session->epoch);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data != NULL) {
            if (data->op_code == 0x1) {
                handle_control_payload(relay, data);
            } else if (data->op_code == 0x2) {
                handle_audio_payload(relay, session->epoch, data);
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (data != NULL
                && data->error_handle.error_type
                    != WEBSOCKET_ERROR_TYPE_NONE) {
            ESP_LOGE(TAG,
                     "WSS epoch=%u error type=%d tls=%s stack=%d verify=%d status=%d socket=%d",
                     (unsigned)session->epoch,
                     data->error_handle.error_type,
                     esp_err_to_name(data->error_handle.esp_tls_last_esp_err),
                     data->error_handle.esp_tls_stack_err,
                     data->error_handle.esp_tls_cert_verify_flags,
                     data->error_handle.esp_ws_handshake_status_code,
                     data->error_handle.esp_transport_sock_errno);
        } else {
            ESP_LOGE(TAG, "WSS epoch=%u transport error",
                     (unsigned)session->epoch);
        }
        xEventGroupSetBits(relay->events, SOCKET_RESTART_BIT);
        set_state(relay, NETWORK_RELAY_ERROR);
        break;
    default:
        break;
    }
}

#if WALLE_HAS_CREDENTIALS
static int network_index_for_ssid(const uint8_t *ssid)
{
    const size_t found_length = strnlen((const char *)ssid, 32);
    for (size_t index = 0; index < WALLE_WIFI_NETWORK_COUNT; index++) {
        const size_t known_length = strlen(s_known_networks[index].ssid);
        if (known_length == found_length
                && memcmp(ssid, s_known_networks[index].ssid,
                          found_length) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int scan_for_known_network(network_relay_t *relay, int *selected_rssi)
{
    set_state(relay, NETWORK_RELAY_SCANNING);
    const wifi_scan_config_t scan = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    const esp_err_t scan_error = esp_wifi_scan_start(&scan, true);
    if (scan_error != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(scan_error));
        return -1;
    }

    uint16_t count = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_num(&count));
    if (count > MAX_SCAN_RESULTS) {
        count = MAX_SCAN_RESULTS;
    }
    if (count == 0) {
        ESP_LOGI(TAG, "No access points visible; retrying");
        return -1;
    }

    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    if (records == NULL) {
        set_state(relay, NETWORK_RELAY_ERROR);
        return -1;
    }
    const esp_err_t records_error = esp_wifi_scan_get_ap_records(
        &count, records);
    if (records_error != ESP_OK) {
        ESP_LOGW(TAG, "Could not read scan results: %s",
                 esp_err_to_name(records_error));
        free(records);
        return -1;
    }

    int selected = -1;
    int rssi = -128;
    for (uint16_t index = 0; index < count; index++) {
        const int known = network_index_for_ssid(records[index].ssid);
        if (known >= 0 && records[index].rssi > rssi) {
            selected = known;
            rssi = records[index].rssi;
        }
    }
    free(records);
    if (selected >= 0) {
        *selected_rssi = rssi;
        ESP_LOGI(TAG, "Selected configured Wi-Fi network %d (RSSI %d)",
                 selected + 1, rssi);
    } else {
        ESP_LOGI(TAG, "No configured Wi-Fi network visible; retrying");
    }
    return selected;
}

static bool connect_wifi(network_relay_t *relay, int index, int rssi)
{
    const known_network_t *network = &s_known_networks[index];
    const size_t ssid_length = strlen(network->ssid);
    const size_t password_length = strlen(network->password);
    wifi_config_t config = {0};
    memcpy(config.sta.ssid, network->ssid, ssid_length);
    memcpy(config.sta.password, network->password, password_length);
    config.sta.threshold.authmode = password_length == 0
        ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    xEventGroupClearBits(relay->events,
                         WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT);
    set_active_network(relay, index, rssi);
    set_state(relay, NETWORK_RELAY_WIFI_CONNECTING);
    esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (error == ESP_OK) {
        error = esp_wifi_connect();
    }
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi connection start failed: %s",
                 esp_err_to_name(error));
        return false;
    }

    const EventBits_t result = xEventGroupWaitBits(
        relay->events, WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if ((result & WIFI_GOT_IP_BIT) != 0) {
        return true;
    }
    ESP_LOGW(TAG, "Configured Wi-Fi network %d did not connect", index + 1);
    esp_wifi_disconnect();
    return false;
}

static bool start_websocket(network_relay_t *relay)
{
    websocket_session_t *session = calloc(1, sizeof(*session));
    if (session == NULL) {
        return false;
    }
    relay->epoch_counter++;
    if (relay->epoch_counter == 0) {
        relay->epoch_counter++;
    }
    session->relay = relay;
    session->epoch = relay->epoch_counter;
    atomic_init(&session->disconnect_reported, false);
    relay->websocket_session = session;
    atomic_store(&relay->active_epoch, session->epoch);
    set_connection_epoch(relay, session->epoch);

    const esp_websocket_client_config_t config = {
        .uri = relay->relay_uri,
        .disable_auto_reconnect = true,
        .user_context = session,
        .task_core_id_set = true,
        .task_core_id = 0,
        .task_prio = 4,
        .task_name = "walle_wss",
        .task_stack = WEBSOCKET_STACK_BYTES,
        .buffer_size = SOCKET_BUFFER_BYTES,
        .headers = relay->authorization_header,
        .pingpong_timeout_sec = 15,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .cert_common_name = WALLE_RELAY_HOST,
        .keep_alive_enable = true,
        .keep_alive_idle = 20,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 10,
    };
    relay->websocket = esp_websocket_client_init(&config);
    if (relay->websocket == NULL) {
        ESP_LOGE(TAG, "WSS client allocation failed");
        atomic_store(&relay->active_epoch, 0);
        set_connection_epoch(relay, 0);
        relay->websocket_session = NULL;
        free(session);
        return false;
    }
    esp_err_t error = esp_websocket_register_events(
        relay->websocket, WEBSOCKET_EVENT_ANY, websocket_event_handler,
        session);
    if (error == ESP_OK) {
        error = esp_websocket_client_start(relay->websocket);
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "WSS client start failed: %s", esp_err_to_name(error));
        esp_websocket_client_destroy(relay->websocket);
        relay->websocket = NULL;
        atomic_store(&relay->active_epoch, 0);
        set_connection_epoch(relay, 0);
        relay->websocket_session = NULL;
        free(session);
        return false;
    }
    set_state(relay, NETWORK_RELAY_SOCKET_CONNECTING);
    return true;
}

static void stop_websocket(network_relay_t *relay)
{
    websocket_session_t *session = relay->websocket_session;
    atomic_store(&relay->active_epoch, 0);
    set_connection_epoch(relay, 0);
    if (relay->websocket != NULL) {
        esp_websocket_client_stop(relay->websocket);
        esp_websocket_client_destroy(relay->websocket);
        relay->websocket = NULL;
    }
    relay->websocket_session = NULL;
    free(session);
    xEventGroupClearBits(relay->events,
                         SOCKET_CONNECTED_BIT | SOCKET_READY_BIT
                             | SOCKET_DISCONNECTED_BIT | SOCKET_RESTART_BIT
                             | SOCKET_PONG_BIT);
    reset_stream_state(relay);
}

static bool send_control(network_relay_t *relay, const char *message)
{
    const int length = (int)strlen(message);
    const int written = esp_websocket_client_send_text(
        relay->websocket, message, length, pdMS_TO_TICKS(2000));
    const bool sent = written == length;
    if (!sent) {
        xEventGroupSetBits(relay->events, SOCKET_RESTART_BIT);
    }
    return sent;
}

static bool send_network_telemetry(network_relay_t *relay)
{
    const bool capture_accepting = atomic_load(
        &relay->capture_accepting);
    const UBaseType_t queue_depth = uxQueueMessagesWaiting(
        relay->stream_queue);

    network_relay_snapshot_t snapshot;
    network_relay_get_snapshot(relay, &snapshot);
    char message[768];
    const int length = snprintf(
        message, sizeof(message),
        "{\"v\":1,\"type\":\"telemetry.report\","
        "\"epoch\":%u,\"state\":%u,\"captureAccepting\":%s,"
        "\"queueDepth\":%u,\"startsQueued\":%u,"
        "\"startUnready\":%u,\"startMutexBusy\":%u,"
        "\"startAlreadyActive\":%u,\"turnsStarted\":%u,"
        "\"turnsCommitted\":%u,\"audioDropped\":%u,"
        "\"audioUnready\":%u,\"audioMutexBusy\":%u,"
        "\"audioNotAccepting\":%u,\"audioBackpressure\":%u,"
        "\"audioQueueFull\":%u,\"audioStreamInactive\":%u,"
        "\"audioSendFailures\":%u,\"outputEventDrops\":%u,"
        "\"protocolErrors\":%u,\"socketRestarts\":%u}",
        (unsigned)snapshot.connection_epoch,
        (unsigned)snapshot.state,
        capture_accepting ? "true" : "false",
        (unsigned)queue_depth,
        (unsigned)snapshot.capture_starts_queued,
        (unsigned)snapshot.capture_start_unready,
        (unsigned)snapshot.capture_start_mutex_busy,
        (unsigned)snapshot.capture_start_already_active,
        (unsigned)snapshot.turns_started,
        (unsigned)snapshot.turns_committed,
        (unsigned)snapshot.audio_frames_dropped,
        (unsigned)snapshot.audio_drop_unready,
        (unsigned)snapshot.audio_drop_mutex_busy,
        (unsigned)snapshot.audio_drop_not_accepting,
        (unsigned)snapshot.audio_drop_backpressure,
        (unsigned)snapshot.audio_drop_queue_full,
        (unsigned)snapshot.audio_drop_stream_inactive,
        (unsigned)snapshot.audio_send_failures,
        (unsigned)snapshot.output_events_dropped,
        (unsigned)snapshot.protocol_errors,
        (unsigned)snapshot.socket_restarts);
    return length > 0 && length < (int)sizeof(message)
        && send_control(relay, message);
}

static bool send_audio_frame(void *context, const int16_t *samples,
                             size_t sample_count)
{
    network_relay_t *relay = context;
    if (sample_count == 0 || sample_count > NETWORK_FRAME_SAMPLES
            || samples != (const int16_t *)&relay->audio_tx_frame[
                AUDIO_HEADER_BYTES]) {
        increment_counter(relay, &relay->snapshot.protocol_errors);
        return false;
    }

    uint8_t *frame = relay->audio_tx_frame;
    const size_t pcm_bytes = sample_count * sizeof(*samples);
    const uint32_t sequence = relay->next_input_sequence++;
    frame[0] = 0x57;
    frame[1] = 0x41;
    frame[2] = 1;
    frame[3] = 1;
    write_u32_le(&frame[4], sequence);
    write_u16_le(&frame[8], (uint16_t)sample_count);
    write_u16_le(&frame[10], 0);

    const uint32_t hash = audio_hash(&frame[AUDIO_HEADER_BYTES], pcm_bytes);
    portENTER_CRITICAL(&relay->stream_lock);
    expected_echo_t *expected =
        &relay->expected_echoes[sequence % EXPECTED_ECHO_SLOTS];
    expected->sequence = sequence;
    expected->hash = hash;
    expected->epoch = relay->stream_turn_epoch;
    expected->sample_count = (uint16_t)sample_count;
    expected->valid = true;
    portEXIT_CRITICAL(&relay->stream_lock);

    const int frame_bytes = (int)(AUDIO_HEADER_BYTES + pcm_bytes);
    const int written = esp_websocket_client_send_bin(
        relay->websocket, (const char *)frame, frame_bytes,
        pdMS_TO_TICKS(2000));
    if (written == frame_bytes) {
        atomic_fetch_add(&relay->output_input_samples,
                         (uint32_t)sample_count);
        increment_counter(relay, &relay->snapshot.audio_frames_sent);
        return true;
    }

    portENTER_CRITICAL(&relay->stream_lock);
    expected->valid = false;
    portEXIT_CRITICAL(&relay->stream_lock);
    increment_counter(relay, &relay->snapshot.audio_frames_dropped);
    increment_counter(relay, &relay->snapshot.audio_send_failures);
    atomic_store(&relay->telemetry_dirty, true);
    xEventGroupSetBits(relay->events, SOCKET_RESTART_BIT);
    return false;
}

static void service_stream_queue(network_relay_t *relay)
{
    if ((xEventGroupGetBits(relay->events) & SOCKET_READY_BIT) == 0) {
        return;
    }

    const uint32_t epoch = atomic_load(&relay->active_epoch);
    stream_item_t item;
    while (xQueueReceive(relay->stream_queue, &item, 0) == pdTRUE) {
        if (epoch == 0 || item.epoch != epoch) {
            if (item.event == STREAM_EVENT_CAPTURE_AUDIO) {
                increment_counter(relay,
                                  &relay->snapshot.audio_frames_dropped);
            }
            continue;
        }
        switch (item.event) {
        case STREAM_EVENT_CAPTURE_START: {
            pcm_batcher_abort(&relay->pcm_batcher);
            invalidate_output_turn(relay, false);
            relay->turn_counter++;
            snprintf(relay->active_turn_id, sizeof(relay->active_turn_id),
                     "%08lx-%08lx", (unsigned long)relay->boot_nonce,
                     (unsigned long)relay->turn_counter);
            char message[128];
            snprintf(message, sizeof(message),
                     "{\"v\":1,\"type\":\"turn.start\",\"turnId\":\"%s\"}",
                     relay->active_turn_id);
            relay->stream_turn_active = send_control(relay, message);
            relay->stream_turn_epoch = relay->stream_turn_active ? epoch : 0;
            relay->stream_turn_token = relay->stream_turn_active
                ? item.turn_token : 0;
            if (relay->stream_turn_active) {
                portENTER_CRITICAL(&relay->stream_lock);
                memcpy(relay->output_turn_id, relay->active_turn_id,
                       sizeof(relay->output_turn_id));
                portEXIT_CRITICAL(&relay->stream_lock);
                atomic_store(&relay->output_input_samples, 0);
                atomic_store(&relay->next_output_sequence, 0);
                atomic_store(&relay->active_output_token,
                             item.turn_token);
                atomic_store(&relay->output_turn_active, true);
                increment_counter(relay, &relay->snapshot.turns_started);
            }
            break;
        }
        case STREAM_EVENT_CAPTURE_AUDIO:
            if (relay->stream_turn_active
                    && relay->stream_turn_epoch == epoch
                    && relay->stream_turn_token == item.turn_token) {
                if (!pcm_batcher_write(
                        &relay->pcm_batcher, item.samples,
                        item.sample_count, send_audio_frame, relay)) {
                    relay->stream_turn_active = false;
                    relay->stream_turn_epoch = 0;
                    invalidate_output_turn(relay, false);
                }
            } else {
                increment_counter(relay,
                                  &relay->snapshot.audio_frames_dropped);
                increment_counter(
                    relay, &relay->snapshot.audio_drop_stream_inactive);
                atomic_store(&relay->telemetry_dirty, true);
            }
            break;
        case STREAM_EVENT_CAPTURE_COMMIT:
        case STREAM_EVENT_CAPTURE_CANCEL:
            if (relay->stream_turn_active
                    && relay->stream_turn_epoch == epoch
                    && relay->stream_turn_token == item.turn_token) {
                char message[128];
                const bool commit =
                    item.event == STREAM_EVENT_CAPTURE_COMMIT;
                bool audio_complete = true;
                if (commit) {
                    audio_complete = pcm_batcher_finish(
                        &relay->pcm_batcher, send_audio_frame, relay);
                } else {
                    pcm_batcher_abort(&relay->pcm_batcher);
                }
                snprintf(
                    message, sizeof(message),
                    "{\"v\":1,\"type\":\"turn.%s\",\"turnId\":\"%s\"}",
                    commit ? "commit" : "cancel", relay->active_turn_id);
                if (audio_complete && send_control(relay, message)) {
                    increment_counter(
                        relay,
                        commit ? &relay->snapshot.turns_committed
                               : &relay->snapshot.turns_cancelled);
                } else if (!audio_complete) {
                    invalidate_output_turn(relay, false);
                }
                relay->stream_turn_active = false;
                relay->stream_turn_epoch = 0;
                if (!commit) {
                    relay->stream_turn_token = 0;
                }
            }
            break;
        case STREAM_EVENT_RESPONSE_CANCEL:
            if (item.turn_token == relay->stream_turn_token
                    && atomic_load(&relay->generated_output_mode)
                    && atomic_load(&relay->output_turn_active)) {
                char cancel[128];
                snprintf(
                    cancel, sizeof(cancel),
                    "{\"v\":1,\"type\":\"response.cancel\","
                    "\"turnId\":\"%s\"}", relay->active_turn_id);
                send_control(relay, cancel);
            }
            break;
        case STREAM_EVENT_REMOTE_PLAYED:
        case STREAM_EVENT_LOCAL_FALLBACK:
            if (item.turn_token == relay->stream_turn_token
                    && item.turn_token == atomic_load(
                        &relay->completed_output_token)) {
                const bool remote =
                    item.event == STREAM_EVENT_REMOTE_PLAYED;
                char report[224];
                snprintf(
                    report, sizeof(report),
                    "{\"v\":1,\"type\":\"playback.report\","
                    "\"turnId\":\"%s\",\"source\":\"%s\","
                    "\"samples\":%u,\"firstCodecWriteMs\":%u}",
                    relay->active_turn_id, remote ? "remote" : "local",
                    (unsigned)item.value_count,
                    (unsigned)item.first_codec_write_ms);
                if (send_control(relay, report)) {
                    increment_counter(
                        relay,
                        remote ? &relay->snapshot.remote_playback_reports
                               : &relay->snapshot.local_fallback_reports);
                    relay->stream_turn_token = 0;
                    atomic_store(&relay->completed_output_token, 0);
                }
            }
            break;
        }
    }
}

typedef enum {
    SOCKET_EXIT_TRANSPORT = 0,
    SOCKET_EXIT_CONNECT_TIMEOUT,
    SOCKET_EXIT_READY_TIMEOUT,
    SOCKET_EXIT_HEARTBEAT_TIMEOUT,
    SOCKET_EXIT_PLANNED_REFRESH,
} socket_exit_reason_t;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t reconnect_delay_ms(uint32_t failures)
{
    uint32_t delay = RECONNECT_BASE_MS;
    for (uint32_t index = 1; index < failures
            && delay < RECONNECT_CAP_MS; index++) {
        delay = delay > RECONNECT_CAP_MS / 2
            ? RECONNECT_CAP_MS : delay * 2;
    }
    const uint32_t jitter_range = delay / 2 + 1;
    return delay + esp_random() % jitter_range;
}

static void wait_while_wifi_connected(network_relay_t *relay,
                                      uint32_t delay_ms)
{
    uint32_t remaining = delay_ms;
    while (remaining > 0
            && (xEventGroupGetBits(relay->events) & WIFI_GOT_IP_BIT) != 0) {
        const uint32_t slice = remaining > 100 ? 100 : remaining;
        vTaskDelay(pdMS_TO_TICKS(slice));
        remaining -= slice;
    }
}

static void service_websocket(network_relay_t *relay)
{
    uint32_t consecutive_failures = 0;
    while ((xEventGroupGetBits(relay->events) & WIFI_GOT_IP_BIT) != 0) {
        reset_stream_state(relay);
        atomic_store(&relay->heartbeat_nonce, 0);
        xEventGroupClearBits(relay->events,
                             SOCKET_CONNECTED_BIT | SOCKET_READY_BIT
                                 | SOCKET_DISCONNECTED_BIT
                                 | SOCKET_RESTART_BIT | SOCKET_PONG_BIT);
        if (!start_websocket(relay)) {
            set_state(relay, NETWORK_RELAY_ERROR);
            consecutive_failures++;
            wait_while_wifi_connected(
                relay, reconnect_delay_ms(consecutive_failures));
            continue;
        }

        const int64_t attempt_started_ms = now_ms();
        int64_t connected_ms = 0;
        int64_t ready_ms = 0;
        int64_t next_ping_ms = 0;
        int64_t pong_deadline_ms = 0;
        bool hello_sent = false;
        bool awaiting_pong = false;
        socket_exit_reason_t reason = SOCKET_EXIT_TRANSPORT;

        while ((xEventGroupGetBits(relay->events) & WIFI_GOT_IP_BIT) != 0) {
            EventBits_t bits = xEventGroupGetBits(relay->events);
            const int64_t now = now_ms();
            if ((bits & SOCKET_RESTART_BIT) != 0) {
                break;
            }
            if ((bits & SOCKET_CONNECTED_BIT) == 0) {
                if (now - attempt_started_ms >= SOCKET_CONNECT_DEADLINE_MS) {
                    reason = SOCKET_EXIT_CONNECT_TIMEOUT;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            if (connected_ms == 0) {
                connected_ms = now;
            }
            if (!hello_sent) {
                char hello[192];
                const esp_app_desc_t *description = esp_app_get_description();
                const int length = snprintf(
                    hello, sizeof(hello),
                    "{\"v\":1,\"type\":\"hello\",\"firmware\":\"%.64s\","
                    "\"sampleRate\":24000,\"channels\":1,"
                    "\"sampleFormat\":\"pcm16le\",\"sessionEpoch\":%u}",
                    description->version,
                    (unsigned)atomic_load(&relay->active_epoch));
                if (length <= 0 || length >= (int)sizeof(hello)
                        || !send_control(relay, hello)) {
                    ESP_LOGE(TAG, "Relay hello send failed");
                    break;
                }
                hello_sent = true;
            }

            bits = xEventGroupGetBits(relay->events);
            if ((bits & SOCKET_READY_BIT) == 0) {
                if (now - connected_ms >= PROTOCOL_READY_DEADLINE_MS) {
                    reason = SOCKET_EXIT_READY_TIMEOUT;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            if (ready_ms == 0) {
                ready_ms = now;
                next_ping_ms = now;
            }

            if ((bits & SOCKET_PONG_BIT) != 0) {
                xEventGroupClearBits(relay->events, SOCKET_PONG_BIT);
                if (awaiting_pong) {
                    awaiting_pong = false;
                    next_ping_ms = now + HEARTBEAT_INTERVAL_MS;
                    increment_counter(relay,
                                      &relay->snapshot.heartbeat_pongs);
                }
            }
            if (awaiting_pong && now >= pong_deadline_ms) {
                reason = SOCKET_EXIT_HEARTBEAT_TIMEOUT;
                break;
            }
            if (!awaiting_pong && now >= next_ping_ms) {
                uint32_t nonce = esp_random();
                if (nonce == 0) {
                    nonce = 1;
                }
                atomic_store(&relay->heartbeat_nonce, nonce);
                xEventGroupClearBits(relay->events, SOCKET_PONG_BIT);
                char ping[80];
                snprintf(ping, sizeof(ping),
                         "{\"v\":1,\"type\":\"ping\","
                         "\"nonce\":\"%08x\"}", (unsigned)nonce);
                if (!send_control(relay, ping)) {
                    break;
                }
                awaiting_pong = true;
                pong_deadline_ms = now + HEARTBEAT_TIMEOUT_MS;
            }

            service_stream_queue(relay);
            const bool telemetry_requested = atomic_exchange(
                &relay->telemetry_dirty, false);
            if (telemetry_requested) {
                if (atomic_load(&relay->stream_turn_active)) {
                    atomic_store(&relay->telemetry_dirty, true);
                } else if (!send_network_telemetry(relay)) {
                    atomic_store(&relay->telemetry_dirty, true);
                }
            }
            if (now - connected_ms >= SOCKET_MAX_LIFETIME_MS) {
                reason = SOCKET_EXIT_PLANNED_REFRESH;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        const int64_t stopped_ms = now_ms();
        const bool stable = ready_ms != 0
            && stopped_ms - ready_ms >= STABLE_CONNECTION_MS;
        stop_websocket(relay);
        if ((xEventGroupGetBits(relay->events) & WIFI_GOT_IP_BIT) == 0) {
            return;
        }

        increment_counter(relay, &relay->snapshot.socket_restarts);
        switch (reason) {
        case SOCKET_EXIT_CONNECT_TIMEOUT:
            increment_counter(relay, &relay->snapshot.connect_timeouts);
            ESP_LOGW(TAG, "WSS connect deadline expired");
            break;
        case SOCKET_EXIT_READY_TIMEOUT:
            increment_counter(relay, &relay->snapshot.ready_timeouts);
            ESP_LOGW(TAG, "Relay ready deadline expired");
            break;
        case SOCKET_EXIT_HEARTBEAT_TIMEOUT:
            increment_counter(relay, &relay->snapshot.heartbeat_timeouts);
            ESP_LOGW(TAG, "Relay heartbeat deadline expired");
            break;
        case SOCKET_EXIT_PLANNED_REFRESH:
            ESP_LOGI(TAG, "Refreshing WSS after bounded session lifetime");
            break;
        case SOCKET_EXIT_TRANSPORT:
            ESP_LOGW(TAG, "Restarting WSS after transport event");
            break;
        }

        consecutive_failures = stable ? 0 : consecutive_failures + 1;
        const uint32_t delay = reason == SOCKET_EXIT_PLANNED_REFRESH
            ? PLANNED_REFRESH_DELAY_MS + esp_random() % 251
            : reconnect_delay_ms(consecutive_failures);
        set_state(relay, NETWORK_RELAY_SOCKET_CONNECTING);
        wait_while_wifi_connected(relay, delay);
    }
    stop_websocket(relay);
}

static void manager_task(void *argument)
{
    network_relay_t *relay = argument;
    xEventGroupWaitBits(relay->events, WIFI_STARTED_BIT, pdFALSE, pdTRUE,
                        portMAX_DELAY);

    while (true) {
        int rssi = -128;
        const int network = scan_for_known_network(relay, &rssi);
        if (network < 0 || !connect_wifi(relay, network, rssi)) {
            vTaskDelay(pdMS_TO_TICKS(RESCAN_DELAY_MS));
            continue;
        }
        service_websocket(relay);
        set_state(relay, NETWORK_RELAY_SCANNING);
        vTaskDelay(pdMS_TO_TICKS(RESCAN_DELAY_MS));
    }
}
#endif

esp_err_t network_relay_create(network_relay_t **out_relay)
{
    if (out_relay == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_relay = NULL;
#if !WALLE_HAS_CREDENTIALS
    ESP_LOGW(TAG, "credentials.h absent; network relay disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    const int uri_length = snprintf(NULL, 0, "wss://%s%s",
                                    WALLE_RELAY_HOST, WALLE_RELAY_PATH);
    const int header_length = snprintf(NULL, 0, "Authorization: Bearer %s\r\n",
                                       WALLE_DEVICE_TOKEN);
    if (uri_length <= 0 || uri_length >= 384
            || header_length <= 0 || header_length >= 640) {
        return ESP_ERR_INVALID_SIZE;
    }

    network_relay_t *relay = calloc(1, sizeof(*relay));
    if (relay == NULL) {
        return ESP_ERR_NO_MEM;
    }
    relay->snapshot_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    relay->stream_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    atomic_init(&relay->capture_accepting, false);
    atomic_init(&relay->stream_turn_active, false);
    atomic_init(&relay->active_epoch, 0);
    atomic_init(&relay->heartbeat_nonce, 0);
    atomic_init(&relay->active_output_token, 0);
    atomic_init(&relay->completed_output_token, 0);
    atomic_init(&relay->output_input_samples, 0);
    atomic_init(&relay->next_output_sequence, 0);
    atomic_init(&relay->output_turn_active, false);
    atomic_init(&relay->generated_output_mode, false);
    atomic_init(&relay->buffered_output_mode, false);
    atomic_init(&relay->telemetry_dirty, true);
    relay->snapshot.state = NETWORK_RELAY_DISABLED;
    relay->snapshot.active_network = -1;
    relay->snapshot.rssi = -128;
    relay->boot_nonce = esp_random();
    snprintf(relay->relay_uri, sizeof(relay->relay_uri), "wss://%s%s",
             WALLE_RELAY_HOST, WALLE_RELAY_PATH);
    snprintf(relay->authorization_header,
             sizeof(relay->authorization_header),
             "Authorization: Bearer %s\r\n", WALLE_DEVICE_TOKEN);
    relay->events = xEventGroupCreate();
    relay->stream_mutex = xSemaphoreCreateMutex();
    relay->stream_queue_storage = heap_caps_malloc(
        STREAM_QUEUE_LENGTH * sizeof(stream_item_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    relay->audio_tx_frame = heap_caps_malloc(
        AUDIO_HEADER_BYTES + NETWORK_FRAME_SAMPLES * sizeof(int16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const bool batcher_ready = relay->audio_tx_frame != NULL
        && pcm_batcher_init(
            &relay->pcm_batcher,
            (int16_t *)&relay->audio_tx_frame[AUDIO_HEADER_BYTES],
            NETWORK_FRAME_SAMPLES);
    if (relay->events == NULL || relay->stream_mutex == NULL
            || relay->stream_queue_storage == NULL
            || relay->audio_tx_frame == NULL || !batcher_ready) {
        free(relay->audio_tx_frame);
        free(relay->stream_queue_storage);
        if (relay->stream_mutex != NULL) {
            vSemaphoreDelete(relay->stream_mutex);
        }
        if (relay->events != NULL) {
            vEventGroupDelete(relay->events);
        }
        free(relay);
        return ESP_ERR_NO_MEM;
    }
    relay->stream_queue = xQueueCreateStatic(
        STREAM_QUEUE_LENGTH, sizeof(stream_item_t),
        relay->stream_queue_storage, &relay->stream_queue_control);
    if (relay->stream_queue == NULL) {
        free(relay->audio_tx_frame);
        free(relay->stream_queue_storage);
        vSemaphoreDelete(relay->stream_mutex);
        vEventGroupDelete(relay->events);
        free(relay);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        return error;
    }
    relay->station_netif = esp_netif_create_default_wifi_sta();
    if (relay->station_netif == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    if ((error = esp_wifi_init(&wifi_init)) != ESP_OK
            || (error = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK
            || (error = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK
            || (error = esp_event_handler_instance_register(
                    WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, relay,
                    &relay->wifi_handler)) != ESP_OK
            || (error = esp_event_handler_instance_register(
                    IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, relay,
                    &relay->ip_handler)) != ESP_OK
            || (error = esp_wifi_start()) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed: %s",
                 esp_err_to_name(error));
        return error;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        manager_task, "network_relay", MANAGER_STACK_BYTES, relay, 3,
        &relay->task, 0);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    *out_relay = relay;
    ESP_LOGI(TAG,
             "Dual-network Wi-Fi/WSS manager started capture=%u network=%u samples",
             CAPTURE_CHUNK_SAMPLES, NETWORK_FRAME_SAMPLES);
    return ESP_OK;
#endif
}

bool network_relay_capture(network_relay_t *relay,
                           network_relay_capture_event_t event,
                           uint32_t turn_token,
                           const int16_t *samples, size_t sample_count)
{
    if (relay == NULL || relay->stream_queue == NULL
            || event < NETWORK_RELAY_CAPTURE_START
            || event > NETWORK_RELAY_CAPTURE_CANCEL
            || turn_token == 0) {
        return false;
    }
    if (event == NETWORK_RELAY_CAPTURE_AUDIO
            && (samples == NULL || sample_count == 0
                || sample_count > CAPTURE_CHUNK_SAMPLES)) {
        return false;
    }
    if ((xEventGroupGetBits(relay->events) & SOCKET_READY_BIT) == 0) {
        if (event == NETWORK_RELAY_CAPTURE_START) {
            increment_counter(relay,
                              &relay->snapshot.capture_start_unready);
            atomic_store(&relay->telemetry_dirty, true);
        } else if (event == NETWORK_RELAY_CAPTURE_AUDIO) {
            increment_counter(relay,
                              &relay->snapshot.audio_frames_dropped);
            increment_counter(relay,
                              &relay->snapshot.audio_drop_unready);
            atomic_store(&relay->telemetry_dirty, true);
        }
        return false;
    }
    if (xSemaphoreTake(relay->stream_mutex, 0) != pdTRUE) {
        if (event == NETWORK_RELAY_CAPTURE_START) {
            increment_counter(
                relay, &relay->snapshot.capture_start_mutex_busy);
            atomic_store(&relay->telemetry_dirty, true);
        } else if (event == NETWORK_RELAY_CAPTURE_AUDIO) {
            increment_counter(relay,
                              &relay->snapshot.audio_frames_dropped);
            increment_counter(relay,
                              &relay->snapshot.audio_drop_mutex_busy);
            atomic_store(&relay->telemetry_dirty, true);
        }
        return false;
    }

    bool allowed = false;
    switch (event) {
    case NETWORK_RELAY_CAPTURE_START:
        allowed = !relay->capture_accepting;
        if (!allowed) {
            increment_counter(
                relay, &relay->snapshot.capture_start_already_active);
            atomic_store(&relay->telemetry_dirty, true);
        }
        break;
    case NETWORK_RELAY_CAPTURE_AUDIO: {
        const bool accepting = atomic_load(&relay->capture_accepting);
        const bool has_space = uxQueueSpacesAvailable(relay->stream_queue)
            > STREAM_CONTROL_RESERVE;
        allowed = accepting && has_space;
        if (!accepting) {
            increment_counter(
                relay, &relay->snapshot.audio_drop_not_accepting);
            atomic_store(&relay->telemetry_dirty, true);
        } else if (!has_space) {
            increment_counter(
                relay, &relay->snapshot.audio_drop_backpressure);
            atomic_store(&relay->telemetry_dirty, true);
        }
        break;
    }
    case NETWORK_RELAY_CAPTURE_COMMIT:
    case NETWORK_RELAY_CAPTURE_CANCEL:
        allowed = relay->capture_accepting;
        break;
    }

    const uint32_t epoch = atomic_load(&relay->active_epoch);
    stream_item_t item = {
        .event = (stream_event_t)event,
        .sample_count = (uint16_t)sample_count,
        .epoch = epoch,
        .turn_token = turn_token,
    };
    if (allowed && event == NETWORK_RELAY_CAPTURE_AUDIO) {
        memcpy(item.samples, samples, sample_count * sizeof(*samples));
    }
    const bool queued = allowed && epoch != 0
        && xQueueSend(relay->stream_queue, &item, 0) == pdTRUE;
    if (allowed && epoch != 0 && !queued
            && event == NETWORK_RELAY_CAPTURE_AUDIO) {
        increment_counter(relay,
                          &relay->snapshot.audio_drop_queue_full);
        atomic_store(&relay->telemetry_dirty, true);
    }
    if (queued && event == NETWORK_RELAY_CAPTURE_START) {
        relay->capture_accepting = true;
        increment_counter(relay,
                          &relay->snapshot.capture_starts_queued);
        atomic_store(&relay->telemetry_dirty, true);
    } else if (queued && (event == NETWORK_RELAY_CAPTURE_COMMIT
                          || event == NETWORK_RELAY_CAPTURE_CANCEL)) {
        relay->capture_accepting = false;
    }

    const UBaseType_t depth = uxQueueMessagesWaiting(relay->stream_queue);
    xSemaphoreGive(relay->stream_mutex);

    if (event == NETWORK_RELAY_CAPTURE_AUDIO) {
        increment_counter(
            relay, queued ? &relay->snapshot.audio_frames_queued
                          : &relay->snapshot.audio_frames_dropped);
    }
    portENTER_CRITICAL(&relay->snapshot_lock);
    if (depth > relay->snapshot.stream_queue_high_water) {
        relay->snapshot.stream_queue_high_water = depth;
    }
    portEXIT_CRITICAL(&relay->snapshot_lock);
    return queued;
}

esp_err_t network_relay_set_output_sink(network_relay_t *relay,
                                        network_relay_output_sink_t sink,
                                        void *context)
{
    if (relay == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&relay->stream_lock);
    relay->output_sink = sink;
    relay->output_context = context;
    portEXIT_CRITICAL(&relay->stream_lock);
    return ESP_OK;
}

bool network_relay_cancel_response(network_relay_t *relay,
                                   uint32_t turn_token)
{
    if (relay == NULL || relay->stream_queue == NULL || turn_token == 0
            || (xEventGroupGetBits(relay->events) & SOCKET_READY_BIT) == 0
            || xSemaphoreTake(relay->stream_mutex, 0) != pdTRUE) {
        return false;
    }
    const uint32_t epoch = atomic_load(&relay->active_epoch);
    const stream_item_t item = {
        .event = STREAM_EVENT_RESPONSE_CANCEL,
        .epoch = epoch,
        .turn_token = turn_token,
    };
    const bool queued = epoch != 0
        && xQueueSend(relay->stream_queue, &item, 0) == pdTRUE;
    xSemaphoreGive(relay->stream_mutex);
    return queued;
}

bool network_relay_report_playback(network_relay_t *relay,
                                   uint32_t turn_token,
                                   bool remote,
                                   size_t sample_count,
                                   uint32_t first_codec_write_ms)
{
    if (relay == NULL || relay->stream_queue == NULL || turn_token == 0
            || sample_count == 0
            || sample_count > MAX_OUTPUT_TURN_SAMPLES
            || first_codec_write_ms > MAX_PLAYBACK_START_LATENCY_MS
            || (xEventGroupGetBits(relay->events) & SOCKET_READY_BIT) == 0
            || xSemaphoreTake(relay->stream_mutex, 0) != pdTRUE) {
        return false;
    }
    const uint32_t epoch = atomic_load(&relay->active_epoch);
    const stream_item_t item = {
        .event = remote ? STREAM_EVENT_REMOTE_PLAYED
                        : STREAM_EVENT_LOCAL_FALLBACK,
        .epoch = epoch,
        .turn_token = turn_token,
        .value_count = (uint32_t)sample_count,
        .first_codec_write_ms = first_codec_write_ms,
    };
    const bool queued = epoch != 0
        && xQueueSend(relay->stream_queue, &item, 0) == pdTRUE;
    const UBaseType_t depth = uxQueueMessagesWaiting(relay->stream_queue);
    xSemaphoreGive(relay->stream_mutex);
    portENTER_CRITICAL(&relay->snapshot_lock);
    if (depth > relay->snapshot.stream_queue_high_water) {
        relay->snapshot.stream_queue_high_water = depth;
    }
    portEXIT_CRITICAL(&relay->snapshot_lock);
    return queued;
}

void network_relay_get_snapshot(network_relay_t *relay,
                                network_relay_snapshot_t *snapshot)
{
    if (relay == NULL || snapshot == NULL) {
        return;
    }
    portENTER_CRITICAL(&relay->snapshot_lock);
    *snapshot = relay->snapshot;
    portEXIT_CRITICAL(&relay->snapshot_lock);
    snapshot->free_internal_bytes = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL);
    snapshot->minimum_internal_bytes =
        (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
}
