#include "remote_event_queue.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

static void test_audio_and_control_validation(void)
{
    remote_event_t storage[3];
    remote_event_queue_t queue;
    int16_t samples[REMOTE_EVENT_MAX_SAMPLES];
    for (size_t index = 0; index < ARRAY_SIZE(samples); index++) {
        samples[index] = (int16_t)index;
    }

    assert(!remote_event_queue_init(NULL, storage, ARRAY_SIZE(storage)));
    assert(!remote_event_queue_init(&queue, NULL, ARRAY_SIZE(storage)));
    assert(!remote_event_queue_init(&queue, storage, 1));
    assert(remote_event_queue_init(&queue, storage, ARRAY_SIZE(storage)));
    assert(!remote_event_queue_try_push(
        &queue, REMOTE_EVENT_AUDIO, 0, samples, 1, 0));
    assert(!remote_event_queue_try_push(
        &queue, REMOTE_EVENT_AUDIO, 1, NULL, 1, 0));
    assert(!remote_event_queue_try_push(
        &queue, REMOTE_EVENT_DONE, 1, samples, 1, 1));

    assert(remote_event_queue_try_push(
        &queue, REMOTE_EVENT_AUDIO, 5, samples, ARRAY_SIZE(samples), 0));
    const remote_event_t *event = remote_event_queue_peek(&queue);
    assert(event != NULL);
    assert(event->type == REMOTE_EVENT_AUDIO);
    assert(event->turn_token == 5);
    assert(event->sample_count == ARRAY_SIZE(samples));
    for (size_t index = 0; index < ARRAY_SIZE(samples); index++) {
        assert(event->samples[index] == (int16_t)index);
    }
    assert(remote_event_queue_consume(&queue));

    assert(remote_event_queue_try_push(
        &queue, REMOTE_EVENT_DONE, 5, NULL, 0, 960));
    event = remote_event_queue_peek(&queue);
    assert(event != NULL && event->type == REMOTE_EVENT_DONE);
    assert(event->value_count == 960);
    assert(remote_event_queue_consume(&queue));
    assert(remote_event_queue_peek(&queue) == NULL);
    assert(!remote_event_queue_consume(&queue));
}

static void test_capacity_wrap_and_fifo_order(void)
{
    remote_event_t storage[5];
    remote_event_queue_t queue;
    assert(remote_event_queue_init(&queue, storage, ARRAY_SIZE(storage)));

    for (uint32_t token = 1; token <= 4; token++) {
        assert(remote_event_queue_try_push(
            &queue, REMOTE_EVENT_DONE, token, NULL, 0, token * 10));
    }
    assert(remote_event_queue_count(&queue) == 4);
    assert(!remote_event_queue_try_push(
        &queue, REMOTE_EVENT_DONE, 5, NULL, 0, 50));

    for (uint32_t token = 1; token <= 2; token++) {
        const remote_event_t *event = remote_event_queue_peek(&queue);
        assert(event != NULL && event->turn_token == token);
        assert(remote_event_queue_consume(&queue));
    }
    for (uint32_t token = 5; token <= 6; token++) {
        assert(remote_event_queue_try_push(
            &queue, REMOTE_EVENT_DONE, token, NULL, 0, token * 10));
    }
    for (uint32_t token = 3; token <= 6; token++) {
        const remote_event_t *event = remote_event_queue_peek(&queue);
        assert(event != NULL && event->turn_token == token);
        assert(event->value_count == token * 10);
        assert(remote_event_queue_consume(&queue));
    }
    assert(remote_event_queue_count(&queue) == 0);
}

static void test_reset_discards_queued_events(void)
{
    remote_event_t storage[3];
    remote_event_queue_t queue;
    assert(remote_event_queue_init(&queue, storage, ARRAY_SIZE(storage)));
    assert(remote_event_queue_try_push(
        &queue, REMOTE_EVENT_INVALID, 9, NULL, 0, 0));
    remote_event_queue_reset(&queue);
    assert(remote_event_queue_count(&queue) == 0);
    assert(remote_event_queue_peek(&queue) == NULL);
}

#define STRESS_EVENTS 100000

typedef struct {
    remote_event_queue_t *queue;
    atomic_bool start;
} stress_context_t;

static void *produce_events(void *argument)
{
    stress_context_t *context = argument;
    while (!atomic_load_explicit(&context->start, memory_order_acquire)) {
        sched_yield();
    }
    for (uint32_t sequence = 1; sequence <= STRESS_EVENTS; sequence++) {
        const int16_t sample = (int16_t)sequence;
        while (!remote_event_queue_try_push(
                    context->queue, REMOTE_EVENT_AUDIO, sequence,
                    &sample, 1, sequence)) {
            sched_yield();
        }
    }
    return NULL;
}

static void test_concurrent_spsc_stress(void)
{
    remote_event_t storage[17];
    remote_event_queue_t queue;
    stress_context_t context = {.queue = &queue};
    pthread_t producer;
    assert(remote_event_queue_init(&queue, storage, ARRAY_SIZE(storage)));
    atomic_init(&context.start, false);
    assert(pthread_create(&producer, NULL, produce_events, &context) == 0);
    atomic_store_explicit(&context.start, true, memory_order_release);

    for (uint32_t expected = 1; expected <= STRESS_EVENTS;) {
        const remote_event_t *event = remote_event_queue_peek(&queue);
        if (event == NULL) {
            sched_yield();
            continue;
        }
        assert(event->type == REMOTE_EVENT_AUDIO);
        assert(event->turn_token == expected);
        assert(event->value_count == expected);
        assert(event->sample_count == 1);
        assert(event->samples[0] == (int16_t)expected);
        assert(remote_event_queue_consume(&queue));
        expected++;
    }
    assert(pthread_join(producer, NULL) == 0);
    assert(remote_event_queue_count(&queue) == 0);
}

int main(void)
{
    test_audio_and_control_validation();
    test_capacity_wrap_and_fifo_order();
    test_reset_discards_queued_events();
    test_concurrent_spsc_stress();
    puts("remote_event_queue_test: PASS");
    return 0;
}
