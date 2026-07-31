#include "remote_response.h"

#include <string.h>

static bool configured(const remote_response_t *response)
{
    return response != NULL && response->storage != NULL
        && response->capacity > 0;
}

static void clear_turn(remote_response_t *response,
                       remote_response_status_t status)
{
    response->sample_count = 0;
    response->read_offset = 0;
    response->write_offset = 0;
    response->received_count = 0;
    response->read_count = 0;
    response->streaming_started = false;
    if (status == REMOTE_RESPONSE_EMPTY) {
        response->turn_token = 0;
    }
    response->status = status;
}

bool remote_response_init(remote_response_t *response, int16_t *storage,
                          size_t capacity)
{
    if (response == NULL || storage == NULL || capacity == 0) {
        return false;
    }
    *response = (remote_response_t) {
        .storage = storage,
        .capacity = capacity,
        .status = REMOTE_RESPONSE_EMPTY,
    };
    return true;
}

bool remote_response_begin(remote_response_t *response, uint32_t turn_token)
{
    if (!configured(response) || turn_token == 0) {
        return false;
    }
    response->turn_token = turn_token;
    response->sample_count = 0;
    response->read_offset = 0;
    response->write_offset = 0;
    response->received_count = 0;
    response->read_count = 0;
    response->status = REMOTE_RESPONSE_RECEIVING;
    response->streaming_started = false;
    return true;
}

bool remote_response_append(remote_response_t *response, uint32_t turn_token,
                            const int16_t *samples, size_t sample_count)
{
    if (!configured(response) || turn_token == 0
            || response->turn_token != turn_token
            || response->status != REMOTE_RESPONSE_RECEIVING) {
        return false;
    }
    if (samples == NULL || sample_count == 0) {
        clear_turn(response, REMOTE_RESPONSE_INVALID);
        return false;
    }
    if (sample_count > response->capacity - response->sample_count) {
        clear_turn(response, REMOTE_RESPONSE_INVALID);
        return false;
    }
    size_t first = response->capacity - response->write_offset;
    if (first > sample_count) {
        first = sample_count;
    }
    memcpy(&response->storage[response->write_offset], samples,
           first * sizeof(*samples));
    const size_t second = sample_count - first;
    if (second > 0) {
        memcpy(response->storage, &samples[first],
               second * sizeof(*samples));
    }
    response->write_offset =
        (response->write_offset + sample_count) % response->capacity;
    response->sample_count += sample_count;
    response->received_count += sample_count;
    return true;
}

bool remote_response_complete(remote_response_t *response,
                              uint32_t turn_token,
                              size_t relay_output_samples,
                              size_t relay_input_samples,
                              size_t local_capture_samples)
{
    if (!configured(response) || turn_token == 0
            || response->turn_token != turn_token
            || response->status != REMOTE_RESPONSE_RECEIVING) {
        return false;
    }
    if (response->received_count == 0
            || relay_input_samples == 0
            || response->received_count != relay_output_samples
            || relay_input_samples != local_capture_samples) {
        clear_turn(response, REMOTE_RESPONSE_INVALID);
        return false;
    }
    if (!response->streaming_started) {
        response->read_offset = 0;
        response->read_count = 0;
    }
    response->status = REMOTE_RESPONSE_READY;
    return true;
}

bool remote_response_invalidate(remote_response_t *response,
                                uint32_t turn_token)
{
    if (!configured(response) || turn_token == 0
            || response->turn_token != turn_token
            || response->status == REMOTE_RESPONSE_EMPTY) {
        return false;
    }
    clear_turn(response, REMOTE_RESPONSE_INVALID);
    return true;
}

bool remote_response_cancel(remote_response_t *response,
                            uint32_t turn_token)
{
    if (!configured(response) || turn_token == 0
            || response->turn_token != turn_token
            || response->status == REMOTE_RESPONSE_EMPTY) {
        return false;
    }
    clear_turn(response, REMOTE_RESPONSE_EMPTY);
    return true;
}

bool remote_response_start_streaming(remote_response_t *response,
                                     uint32_t turn_token,
                                     size_t minimum_buffered_samples)
{
    if (!configured(response) || turn_token == 0
            || minimum_buffered_samples == 0
            || response->turn_token != turn_token
            || response->status != REMOTE_RESPONSE_RECEIVING
            || response->streaming_started
            || response->sample_count < minimum_buffered_samples) {
        return false;
    }
    response->streaming_started = true;
    return true;
}

bool remote_response_streaming(const remote_response_t *response,
                               uint32_t turn_token)
{
    return configured(response) && turn_token != 0
        && response->turn_token == turn_token
        && response->streaming_started;
}

size_t remote_response_unread_samples(const remote_response_t *response,
                                      uint32_t turn_token)
{
    return remote_response_status(response, turn_token)
            == REMOTE_RESPONSE_EMPTY
        ? 0 : response->sample_count;
}

bool remote_response_stream_finished(const remote_response_t *response,
                                     uint32_t turn_token)
{
    return remote_response_streaming(response, turn_token)
        && remote_response_status(response, turn_token)
            == REMOTE_RESPONSE_READY
        && response->read_count == response->received_count;
}

size_t remote_response_read(remote_response_t *response, uint32_t turn_token,
                            int16_t *samples, size_t capacity)
{
    if (!configured(response) || turn_token == 0 || samples == NULL
            || capacity == 0 || response->turn_token != turn_token
            || (response->status != REMOTE_RESPONSE_READY
                && !(response->status == REMOTE_RESPONSE_RECEIVING
                     && response->streaming_started))) {
        return 0;
    }
    const size_t count = response->sample_count < capacity
        ? response->sample_count : capacity;
    size_t first = response->capacity - response->read_offset;
    if (first > count) {
        first = count;
    }
    memcpy(samples, &response->storage[response->read_offset],
           first * sizeof(*samples));
    const size_t second = count - first;
    if (second > 0) {
        memcpy(&samples[first], response->storage,
               second * sizeof(*samples));
    }
    response->read_offset =
        (response->read_offset + count) % response->capacity;
    response->sample_count -= count;
    response->read_count += count;
    if (response->sample_count == 0
            && response->status == REMOTE_RESPONSE_READY
            && !response->streaming_started) {
        clear_turn(response, REMOTE_RESPONSE_EMPTY);
    }
    return count;
}

remote_response_decision_t remote_response_select(
    remote_response_t *response, uint32_t turn_token,
    bool remote_attempted, int64_t now_ms,
    int64_t earliest_playback_ms, int64_t response_deadline_ms)
{
    if (now_ms < earliest_playback_ms) {
        return REMOTE_RESPONSE_WAIT;
    }
    if (!remote_attempted || !configured(response) || turn_token == 0
            || response_deadline_ms < earliest_playback_ms
            || response->turn_token != turn_token) {
        return REMOTE_RESPONSE_USE_LOCAL;
    }
    if (response->status == REMOTE_RESPONSE_READY) {
        return REMOTE_RESPONSE_USE_REMOTE;
    }
    if (response->status != REMOTE_RESPONSE_RECEIVING) {
        return REMOTE_RESPONSE_USE_LOCAL;
    }
    if (now_ms < response_deadline_ms) {
        return REMOTE_RESPONSE_WAIT;
    }
    clear_turn(response, REMOTE_RESPONSE_INVALID);
    return REMOTE_RESPONSE_USE_LOCAL;
}

remote_response_status_t remote_response_status(
    const remote_response_t *response, uint32_t turn_token)
{
    if (!configured(response) || turn_token == 0
            || response->turn_token != turn_token) {
        return REMOTE_RESPONSE_EMPTY;
    }
    return response->status;
}

size_t remote_response_sample_count(const remote_response_t *response,
                                    uint32_t turn_token)
{
    return remote_response_status(response, turn_token)
            == REMOTE_RESPONSE_EMPTY
        ? 0 : response->received_count;
}
