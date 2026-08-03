#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct offline_echo offline_echo_t;

typedef enum {
    OFFLINE_ECHO_STREAM_START = 0,
    OFFLINE_ECHO_STREAM_AUDIO,
    OFFLINE_ECHO_STREAM_COMMIT,
    OFFLINE_ECHO_STREAM_CANCEL,
    OFFLINE_ECHO_STREAM_REMOTE_PLAYED,
    OFFLINE_ECHO_STREAM_LOCAL_FALLBACK,
    OFFLINE_ECHO_STREAM_RESPONSE_CANCEL,
} offline_echo_stream_event_t;

typedef bool (*offline_echo_stream_sink_t)(
    void *context,
    offline_echo_stream_event_t event,
    uint32_t turn_token,
    const int16_t *samples,
    size_t sample_count);

typedef enum {
    OFFLINE_ECHO_REMOTE_AUDIO = 0,
    OFFLINE_ECHO_REMOTE_DONE,
    OFFLINE_ECHO_REMOTE_CANCELLED,
    OFFLINE_ECHO_REMOTE_INVALID,
} offline_echo_remote_event_t;

typedef enum {
    OFFLINE_ECHO_IDLE = 0,
    OFFLINE_ECHO_RECORDING,
    OFFLINE_ECHO_PREPARING,
    OFFLINE_ECHO_PLAYING,
    OFFLINE_ECHO_ERROR,
} offline_echo_state_t;

typedef struct {
    offline_echo_state_t state;
    bool muted;
    uint8_t output_volume_percent;
    float playback_level;
    bool recording_committed;
    uint32_t recorded_ms;
    uint32_t precommit_buffered_ms;
    uint32_t capture_queued_ms;
    uint32_t playback_queued_ms;
    uint32_t capture_overruns;
    uint32_t playback_underruns;
    uint32_t read_errors;
    uint32_t write_errors;
    uint32_t stream_audio_frames;
    uint32_t stream_drops;
    uint32_t remote_audio_frames;
    uint32_t remote_event_drops;
    uint32_t remote_playbacks;
    uint32_t local_fallbacks;
    uint32_t remote_timeouts;
    bool first_codec_write_latency_valid;
    uint32_t release_to_first_codec_write_ms;
    esp_err_t last_error;
} offline_echo_snapshot_t;

/**
 * Initialize the ES8311 duplex path and start the offline echo worker.
 * The module owns its codec, I2S channels, task, command queue, and bounded
 * PSRAM pre-commit, capture, and playback rings.
 */
esp_err_t offline_echo_create(offline_echo_t **out_echo);

/**
 * Install a non-blocking observer for committed capture. Audio remains owned by
 * the local echo path; the sink receives duplicate frames and must never block.
 */
esp_err_t offline_echo_set_stream_sink(offline_echo_t *echo,
                                       offline_echo_stream_sink_t sink,
                                       void *context);

/**
 * Non-blocking SPSC producer seam for already-validated remote output. The
 * WebSocket event task may call this; only the audio task consumes events and
 * owns response validation, source selection, and codec writes.
 */
bool offline_echo_receive_remote(offline_echo_t *echo,
                                 offline_echo_remote_event_t event,
                                 uint32_t turn_token,
                                 const int16_t *samples,
                                 size_t sample_count,
                                 uint32_t value_count,
                                 uint32_t input_count);

/** Start capturing a new phrase. Returns ESP_ERR_INVALID_STATE when muted/busy. */
esp_err_t offline_echo_record_start(offline_echo_t *echo);

/** Stop capture and schedule playback. Uncommitted taps under 180 ms are discarded. */
esp_err_t offline_echo_record_stop(offline_echo_t *echo);

/**
 * Apply logical mute. Muting immediately cancels capture/playback and prevents
 * samples from being retained; the current codec implementation still clocks
 * and discards microphone frames while idle.
 */
esp_err_t offline_echo_set_muted(offline_echo_t *echo, bool muted);

/** Queue a runtime output-volume change, clamped by the caller to 10–100%. */
esp_err_t offline_echo_set_output_volume(offline_echo_t *echo,
                                         uint8_t volume_percent);

/** Read a coherent, non-blocking state snapshot. */
void offline_echo_get_snapshot(offline_echo_t *echo,
                               offline_echo_snapshot_t *snapshot);
