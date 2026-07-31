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
    response->status = REMOTE_RESPONSE_RECEIVING;
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
    if (samples == NULL || sample_count == 0
            || sample_count > response->capacity - response->sample_count) {
        clear_turn(response, REMOTE_RESPONSE_INVALID);
        return false;
    }
    memcpy(&response->storage[response->sample_count], samples,
           sample_count * sizeof(*samples));
    response->sample_count += sample_count;
    return true;
}

bool remote_response_complete(remote_response_t *response,
                              uint32_t turn_token,
                              size_t relay_reported_samples,
                              size_t local_capture_samples)
{
    if (!configured(response) || turn_token == 0
            || response->turn_token != turn_token
            || response->status != REMOTE_RESPONSE_RECEIVING) {
        return false;
    }
    if (response->sample_count == 0
            || response->sample_count != relay_reported_samples
            || response->sample_count != local_capture_samples) {
        clear_turn(response, REMOTE_RESPONSE_INVALID);
        return false;
    }
    response->read_offset = 0;
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

size_t remote_response_read(remote_response_t *response, uint32_t turn_token,
                            int16_t *samples, size_t capacity)
{
    if (!configured(response) || turn_token == 0 || samples == NULL
            || capacity == 0 || response->turn_token != turn_token
            || response->status != REMOTE_RESPONSE_READY) {
        return 0;
    }
    const size_t remaining = response->sample_count - response->read_offset;
    const size_t count = remaining < capacity ? remaining : capacity;
    memcpy(samples, &response->storage[response->read_offset],
           count * sizeof(*samples));
    response->read_offset += count;
    if (response->read_offset == response->sample_count) {
        clear_turn(response, REMOTE_RESPONSE_EMPTY);
    }
    return count;
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
        ? 0 : response->sample_count;
}
