#include "remote_event_queue.h"
#include "remote_response.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

typedef struct {
    remote_response_t response;
    remote_event_queue_t queue;
    uint32_t active_token;
    size_t local_samples;
} pipeline_t;

static void drain(pipeline_t *pipeline)
{
    const remote_event_t *event;
    while ((event = remote_event_queue_peek(&pipeline->queue)) != NULL) {
        if (event->turn_token == pipeline->active_token) {
            switch (event->type) {
            case REMOTE_EVENT_AUDIO:
                remote_response_append(
                    &pipeline->response, event->turn_token,
                    event->samples, event->sample_count);
                break;
            case REMOTE_EVENT_DONE:
                remote_response_complete(
                    &pipeline->response, event->turn_token,
                    event->value_count, event->input_count,
                    pipeline->local_samples);
                break;
            case REMOTE_EVENT_CANCELLED:
                remote_response_cancel(
                    &pipeline->response, event->turn_token);
                break;
            case REMOTE_EVENT_INVALID:
                remote_response_invalidate(
                    &pipeline->response, event->turn_token);
                break;
            }
        }
        assert(remote_event_queue_consume(&pipeline->queue));
    }
}

static void fill(int16_t *samples, size_t count, size_t offset)
{
    for (size_t index = 0; index < count; index++) {
        samples[index] = (int16_t)(offset + index);
    }
}

static void test_exact_complete_turn_becomes_remote_playback(void)
{
    int16_t response_storage[2500];
    remote_event_t event_storage[4];
    pipeline_t pipeline = {
        .active_token = 1,
        .local_samples = ARRAY_SIZE(response_storage),
    };
    assert(remote_response_init(
        &pipeline.response, response_storage,
        ARRAY_SIZE(response_storage)));
    assert(remote_event_queue_init(
        &pipeline.queue, event_storage, ARRAY_SIZE(event_storage)));
    assert(remote_response_begin(&pipeline.response, pipeline.active_token));

    int16_t frame[REMOTE_EVENT_MAX_SAMPLES];
    size_t sent = 0;
    while (sent < pipeline.local_samples) {
        const size_t count = pipeline.local_samples - sent
                < ARRAY_SIZE(frame)
            ? pipeline.local_samples - sent : ARRAY_SIZE(frame);
        fill(frame, count, sent);
        assert(remote_event_queue_try_push(
            &pipeline.queue, REMOTE_EVENT_AUDIO,
            pipeline.active_token, frame, count, 0, 0));
        drain(&pipeline);
        sent += count;
    }
    assert(remote_event_queue_try_push(
        &pipeline.queue, REMOTE_EVENT_DONE, pipeline.active_token,
        NULL, 0, (uint32_t)pipeline.local_samples,
        (uint32_t)pipeline.local_samples));
    drain(&pipeline);

    assert(remote_response_select(
        &pipeline.response, pipeline.active_token, true,
        250, 220, 500) == REMOTE_RESPONSE_USE_REMOTE);
    int16_t output[256];
    size_t played = 0;
    while (true) {
        const size_t count = remote_response_read(
            &pipeline.response, pipeline.active_token,
            output, ARRAY_SIZE(output));
        if (count == 0) break;
        for (size_t index = 0; index < count; index++) {
            assert(output[index] == (int16_t)(played + index));
        }
        played += count;
    }
    assert(played == pipeline.local_samples);
}

static void test_generated_output_duration_may_differ_from_input(void)
{
    int16_t response_storage[960];
    remote_event_t event_storage[3];
    pipeline_t pipeline = {
        .active_token = 4,
        .local_samples = 1920,
    };
    int16_t frame[960] = {0};
    assert(remote_response_init(
        &pipeline.response, response_storage,
        ARRAY_SIZE(response_storage)));
    assert(remote_event_queue_init(
        &pipeline.queue, event_storage, ARRAY_SIZE(event_storage)));
    assert(remote_response_begin(&pipeline.response, pipeline.active_token));
    assert(remote_event_queue_try_push(
        &pipeline.queue, REMOTE_EVENT_AUDIO, 4,
        frame, ARRAY_SIZE(frame), 0, 0));
    assert(remote_event_queue_try_push(
        &pipeline.queue, REMOTE_EVENT_DONE, 4, NULL, 0,
        ARRAY_SIZE(frame), (uint32_t)pipeline.local_samples));
    drain(&pipeline);
    assert(remote_response_status(&pipeline.response, 4)
           == REMOTE_RESPONSE_READY);
}

static void test_missing_frame_falls_back_without_partial_remote(void)
{
    int16_t response_storage[1920];
    remote_event_t event_storage[4];
    pipeline_t pipeline = {
        .active_token = 2,
        .local_samples = ARRAY_SIZE(response_storage),
    };
    int16_t frame[960] = {0};
    assert(remote_response_init(
        &pipeline.response, response_storage,
        ARRAY_SIZE(response_storage)));
    assert(remote_event_queue_init(
        &pipeline.queue, event_storage, ARRAY_SIZE(event_storage)));
    assert(remote_response_begin(&pipeline.response, pipeline.active_token));

    assert(remote_event_queue_try_push(
        &pipeline.queue, REMOTE_EVENT_AUDIO, 2,
        frame, ARRAY_SIZE(frame), 0, 0));
    assert(remote_event_queue_try_push(
        &pipeline.queue, REMOTE_EVENT_DONE, 2,
        NULL, 0, (uint32_t)pipeline.local_samples,
        (uint32_t)pipeline.local_samples));
    drain(&pipeline);
    assert(remote_response_status(&pipeline.response, 2)
           == REMOTE_RESPONSE_INVALID);
    assert(remote_response_select(
        &pipeline.response, 2, true, 250, 220, 500)
           == REMOTE_RESPONSE_USE_LOCAL);
    assert(remote_response_read(
        &pipeline.response, 2, frame, ARRAY_SIZE(frame)) == 0);
}

static void test_queue_overflow_times_out_to_local(void)
{
    int16_t response_storage[3840];
    remote_event_t event_storage[3];
    pipeline_t pipeline = {
        .active_token = 3,
        .local_samples = ARRAY_SIZE(response_storage),
    };
    int16_t frame[960] = {0};
    assert(remote_response_init(
        &pipeline.response, response_storage,
        ARRAY_SIZE(response_storage)));
    assert(remote_event_queue_init(
        &pipeline.queue, event_storage, ARRAY_SIZE(event_storage)));
    assert(remote_response_begin(&pipeline.response, pipeline.active_token));

    assert(remote_event_queue_try_push(
        &pipeline.queue, REMOTE_EVENT_AUDIO, 3,
        frame, ARRAY_SIZE(frame), 0, 0));
    assert(remote_event_queue_try_push(
        &pipeline.queue, REMOTE_EVENT_AUDIO, 3,
        frame, ARRAY_SIZE(frame), 0, 0));
    assert(!remote_event_queue_try_push(
        &pipeline.queue, REMOTE_EVENT_AUDIO, 3,
        frame, ARRAY_SIZE(frame), 0, 0));
    drain(&pipeline);

    assert(remote_response_select(
        &pipeline.response, 3, true, 499, 220, 500)
           == REMOTE_RESPONSE_WAIT);
    assert(remote_response_select(
        &pipeline.response, 3, true, 500, 220, 500)
           == REMOTE_RESPONSE_USE_LOCAL);
    assert(remote_response_status(&pipeline.response, 3)
           == REMOTE_RESPONSE_INVALID);
}

int main(void)
{
    test_exact_complete_turn_becomes_remote_playback();
    test_generated_output_duration_may_differ_from_input();
    test_missing_frame_falls_back_without_partial_remote();
    test_queue_overflow_times_out_to_local();
    puts("remote_pipeline_test: PASS");
    return 0;
}
