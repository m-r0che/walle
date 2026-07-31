#include "remote_response.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

static void fill_sequence(int16_t *samples, size_t count, int start)
{
    for (size_t index = 0; index < count; index++) {
        samples[index] = (int16_t)(start + (int)index);
    }
}

static void test_complete_response_requires_three_matching_counts(void)
{
    int16_t storage[2048];
    int16_t first[960];
    int16_t second[1040];
    remote_response_t response;
    fill_sequence(first, ARRAY_SIZE(first), 0);
    fill_sequence(second, ARRAY_SIZE(second), 960);
    assert(remote_response_init(&response, storage, ARRAY_SIZE(storage)));
    assert(remote_response_begin(&response, 7));
    assert(remote_response_append(&response, 7, first, ARRAY_SIZE(first)));
    assert(remote_response_append(&response, 7, second, ARRAY_SIZE(second)));
    assert(remote_response_sample_count(&response, 7) == 2000);
    assert(remote_response_complete(&response, 7, 2000, 2000));
    assert(remote_response_status(&response, 7) == REMOTE_RESPONSE_READY);

    int16_t output[333];
    size_t total = 0;
    while (true) {
        const size_t count = remote_response_read(
            &response, 7, output, ARRAY_SIZE(output));
        if (count == 0) {
            break;
        }
        for (size_t index = 0; index < count; index++) {
            assert(output[index] == (int16_t)(total + index));
        }
        total += count;
    }
    assert(total == 2000);
    assert(remote_response_status(&response, 7) == REMOTE_RESPONSE_EMPTY);
}

static void test_count_mismatch_invalidates_response(void)
{
    int16_t storage[16];
    int16_t samples[8] = {0};
    remote_response_t response;
    assert(remote_response_init(&response, storage, ARRAY_SIZE(storage)));

    assert(remote_response_begin(&response, 10));
    assert(remote_response_append(&response, 10, samples, 8));
    assert(!remote_response_complete(&response, 10, 7, 8));
    assert(remote_response_status(&response, 10)
           == REMOTE_RESPONSE_INVALID);
    assert(remote_response_read(&response, 10, samples, 8) == 0);

    assert(remote_response_begin(&response, 11));
    assert(remote_response_append(&response, 11, samples, 8));
    assert(!remote_response_complete(&response, 11, 8, 9));
    assert(remote_response_status(&response, 11)
           == REMOTE_RESPONSE_INVALID);
}

static void test_overflow_invalidates_without_partial_playback(void)
{
    int16_t storage[8];
    int16_t samples[6] = {0};
    remote_response_t response;
    assert(remote_response_init(&response, storage, ARRAY_SIZE(storage)));
    assert(remote_response_begin(&response, 12));
    assert(remote_response_append(&response, 12, samples, 6));
    assert(!remote_response_append(&response, 12, samples, 3));
    assert(remote_response_status(&response, 12)
           == REMOTE_RESPONSE_INVALID);
    assert(remote_response_sample_count(&response, 12) == 0);
    assert(remote_response_read(&response, 12, samples, 6) == 0);
}

static void test_stale_events_do_not_damage_current_turn(void)
{
    int16_t storage[16];
    int16_t samples[4] = {1, 2, 3, 4};
    remote_response_t response;
    assert(remote_response_init(&response, storage, ARRAY_SIZE(storage)));
    assert(remote_response_begin(&response, 20));

    assert(!remote_response_append(&response, 19, samples, 4));
    assert(!remote_response_complete(&response, 19, 0, 0));
    assert(!remote_response_invalidate(&response, 19));
    assert(!remote_response_cancel(&response, 19));
    assert(remote_response_status(&response, 20)
           == REMOTE_RESPONSE_RECEIVING);

    assert(remote_response_append(&response, 20, samples, 4));
    assert(remote_response_complete(&response, 20, 4, 4));
    assert(remote_response_status(&response, 20) == REMOTE_RESPONSE_READY);
}

static void test_timeout_and_cancel_never_make_audio_readable(void)
{
    int16_t storage[16];
    int16_t samples[4] = {1, 2, 3, 4};
    remote_response_t response;
    assert(remote_response_init(&response, storage, ARRAY_SIZE(storage)));

    assert(remote_response_begin(&response, 30));
    assert(remote_response_append(&response, 30, samples, 4));
    assert(remote_response_invalidate(&response, 30));
    assert(remote_response_status(&response, 30)
           == REMOTE_RESPONSE_INVALID);
    assert(remote_response_read(&response, 30, samples, 4) == 0);

    assert(remote_response_begin(&response, 31));
    assert(remote_response_append(&response, 31, samples, 4));
    assert(remote_response_cancel(&response, 31));
    assert(remote_response_status(&response, 31)
           == REMOTE_RESPONSE_EMPTY);
    assert(remote_response_read(&response, 31, samples, 4) == 0);
}

static void test_invalid_configuration_and_new_turn_reset(void)
{
    int16_t storage[8];
    int16_t samples[2] = {1, 2};
    remote_response_t response;
    assert(!remote_response_init(NULL, storage, ARRAY_SIZE(storage)));
    assert(!remote_response_init(&response, NULL, ARRAY_SIZE(storage)));
    assert(!remote_response_init(&response, storage, 0));
    assert(remote_response_init(&response, storage, ARRAY_SIZE(storage)));
    assert(!remote_response_begin(&response, 0));

    assert(remote_response_begin(&response, 40));
    assert(remote_response_append(&response, 40, samples, 2));
    assert(remote_response_begin(&response, 41));
    assert(remote_response_sample_count(&response, 41) == 0);
    assert(remote_response_status(&response, 41)
           == REMOTE_RESPONSE_RECEIVING);
}

int main(void)
{
    test_complete_response_requires_three_matching_counts();
    test_count_mismatch_invalidates_response();
    test_overflow_invalidates_without_partial_playback();
    test_stale_events_do_not_damage_current_turn();
    test_timeout_and_cancel_never_make_audio_readable();
    test_invalid_configuration_and_new_turn_reset();
    puts("remote_response_test: PASS");
    return 0;
}
