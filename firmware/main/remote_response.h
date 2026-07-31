#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    REMOTE_RESPONSE_EMPTY = 0,
    REMOTE_RESPONSE_RECEIVING,
    REMOTE_RESPONSE_READY,
    REMOTE_RESPONSE_INVALID,
} remote_response_status_t;

typedef enum {
    REMOTE_RESPONSE_WAIT = 0,
    REMOTE_RESPONSE_USE_LOCAL,
    REMOTE_RESPONSE_USE_REMOTE,
} remote_response_decision_t;

typedef struct {
    int16_t *storage;
    size_t capacity;
    size_t sample_count;
    size_t read_offset;
    size_t write_offset;
    size_t received_count;
    size_t read_count;
    uint32_t turn_token;
    remote_response_status_t status;
    bool streaming_started;
} remote_response_t;

/* Initializes an allocation-free response buffer over caller-owned storage. */
bool remote_response_init(remote_response_t *response, int16_t *storage,
                          size_t capacity);

/* Starts a new response and discards all state from any older turn. */
bool remote_response_begin(remote_response_t *response, uint32_t turn_token);

/*
 * Appends validated, in-order PCM for the active token. Overflow invalidates
 * the response. Events for stale tokens are rejected without touching the
 * current turn.
 */
bool remote_response_append(remote_response_t *response, uint32_t turn_token,
                            const int16_t *samples, size_t sample_count);

/*
 * Makes the response readable only when buffered output equals relay-reported
 * output and relay-reported input equals authoritative local capture. Input and
 * output durations may differ. Any mismatch invalidates the response.
 */
bool remote_response_complete(remote_response_t *response,
                              uint32_t turn_token,
                              size_t relay_output_samples,
                              size_t relay_input_samples,
                              size_t local_capture_samples);

/* Invalidates the matching turn after timeout, queue loss, or protocol error. */
bool remote_response_invalidate(remote_response_t *response,
                                uint32_t turn_token);

/* Cancels the matching turn and returns to empty state. */
bool remote_response_cancel(remote_response_t *response,
                            uint32_t turn_token);

/*
 * Commits a receiving response to buffered streaming after the requested
 * contiguous jitter threshold is present. Once true, local echo must not be
 * selected for this turn.
 */
bool remote_response_start_streaming(remote_response_t *response,
                                     uint32_t turn_token,
                                     size_t minimum_buffered_samples);
bool remote_response_streaming(const remote_response_t *response,
                               uint32_t turn_token);
size_t remote_response_unread_samples(const remote_response_t *response,
                                      uint32_t turn_token);
bool remote_response_stream_finished(const remote_response_t *response,
                                     uint32_t turn_token);

/*
 * Reads a complete response, or a response explicitly committed to buffered
 * streaming. Complete non-streaming responses reset after the final sample;
 * streaming responses retain totals until the owner reports or cancels them.
 */
size_t remote_response_read(remote_response_t *response, uint32_t turn_token,
                            int16_t *samples, size_t capacity);

/*
 * Selects a complete remote response after the authored minimum thinking time,
 * waits only until the bounded deadline, and otherwise chooses local fallback.
 * Timing values are monotonic milliseconds for the same turn.
 */
remote_response_decision_t remote_response_select(
    remote_response_t *response, uint32_t turn_token,
    bool remote_attempted, int64_t now_ms,
    int64_t earliest_playback_ms, int64_t response_deadline_ms);

remote_response_status_t remote_response_status(
    const remote_response_t *response, uint32_t turn_token);
size_t remote_response_sample_count(const remote_response_t *response,
                                    uint32_t turn_token);
