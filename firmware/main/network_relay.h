#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct network_relay network_relay_t;

typedef enum {
    NETWORK_RELAY_CAPTURE_START = 0,
    NETWORK_RELAY_CAPTURE_AUDIO,
    NETWORK_RELAY_CAPTURE_COMMIT,
    NETWORK_RELAY_CAPTURE_CANCEL,
} network_relay_capture_event_t;

typedef enum {
    NETWORK_RELAY_OUTPUT_AUDIO = 0,
    NETWORK_RELAY_OUTPUT_DONE,
    NETWORK_RELAY_OUTPUT_CANCELLED,
    NETWORK_RELAY_OUTPUT_INVALID,
} network_relay_output_event_t;

typedef bool (*network_relay_output_sink_t)(
    void *context,
    network_relay_output_event_t event,
    uint32_t turn_token,
    const int16_t *samples,
    size_t sample_count,
    uint32_t value_count);

typedef enum {
    NETWORK_RELAY_DISABLED = 0,
    NETWORK_RELAY_SCANNING,
    NETWORK_RELAY_WIFI_CONNECTING,
    NETWORK_RELAY_WIFI_CONNECTED,
    NETWORK_RELAY_SOCKET_CONNECTING,
    NETWORK_RELAY_SOCKET_CONNECTED,
    NETWORK_RELAY_READY,
    NETWORK_RELAY_ERROR,
} network_relay_state_t;

typedef struct {
    network_relay_state_t state;
    int8_t active_network;
    int8_t rssi;
    uint32_t wifi_connects;
    uint32_t wifi_disconnects;
    uint32_t socket_connects;
    uint32_t socket_disconnects;
    uint32_t socket_restarts;
    uint32_t connect_timeouts;
    uint32_t ready_timeouts;
    uint32_t heartbeat_timeouts;
    uint32_t heartbeat_pongs;
    uint32_t stale_socket_events;
    uint32_t connection_epoch;
    uint32_t protocol_errors;
    uint32_t turns_started;
    uint32_t turns_committed;
    uint32_t turns_cancelled;
    uint32_t audio_frames_queued;
    uint32_t audio_frames_sent;
    uint32_t audio_frames_dropped;
    uint32_t echo_frames_received;
    uint32_t echo_mismatches;
    uint32_t output_frames_forwarded;
    uint32_t output_events_dropped;
    uint32_t output_turns_done;
    uint32_t remote_playback_reports;
    uint32_t local_fallback_reports;
    uint32_t stream_queue_high_water;
    uint32_t free_internal_bytes;
    uint32_t minimum_internal_bytes;
} network_relay_snapshot_t;

/**
 * Start the dual-network Wi-Fi manager and authenticated WSS relay seam.
 * Returns ESP_ERR_NOT_SUPPORTED when the ignored credentials header is absent.
 * Offline audio remains authoritative; committed PCM is duplicated to the
 * relay. Echoed frames remain unable to reach the codec until the audio owner
 * validates a complete response and selects it over local fallback.
 */
esp_err_t network_relay_create(network_relay_t **out_relay);

/**
 * Non-blocking producer seam for committed local capture. Returns false when
 * the relay is unavailable or bounded backpressure rejects the event.
 */
bool network_relay_capture(network_relay_t *relay,
                           network_relay_capture_event_t event,
                           uint32_t turn_token,
                           const int16_t *samples, size_t sample_count);

/** Install the bounded callback-to-audio output seam. */
esp_err_t network_relay_set_output_sink(network_relay_t *relay,
                                        network_relay_output_sink_t sink,
                                        void *context);

/** Queue aggregate playback-source telemetry after uninterrupted playback. */
bool network_relay_report_playback(network_relay_t *relay,
                                   uint32_t turn_token,
                                   bool remote,
                                   size_t sample_count);

/** Read a coherent, non-blocking connectivity snapshot. */
void network_relay_get_snapshot(network_relay_t *relay,
                                network_relay_snapshot_t *snapshot);
