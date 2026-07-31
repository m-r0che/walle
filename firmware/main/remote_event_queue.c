#include "remote_event_queue.h"

#include <string.h>

static bool configured(const remote_event_queue_t *queue)
{
    return queue != NULL && queue->storage != NULL
        && queue->storage_count >= 2;
}

bool remote_event_queue_init(remote_event_queue_t *queue,
                             remote_event_t *storage,
                             size_t storage_count)
{
    if (queue == NULL || storage == NULL || storage_count < 2) {
        return false;
    }
    queue->storage = storage;
    queue->storage_count = storage_count;
    atomic_init(&queue->head, 0);
    atomic_init(&queue->tail, 0);
    return true;
}

bool remote_event_queue_try_push(remote_event_queue_t *queue,
                                 remote_event_type_t type,
                                 uint32_t turn_token,
                                 const int16_t *samples,
                                 size_t sample_count,
                                 uint32_t value_count)
{
    if (!configured(queue) || type < REMOTE_EVENT_AUDIO
            || type > REMOTE_EVENT_INVALID || turn_token == 0
            || sample_count > REMOTE_EVENT_MAX_SAMPLES
            || (type == REMOTE_EVENT_AUDIO
                && (samples == NULL || sample_count == 0))
            || (type != REMOTE_EVENT_AUDIO && sample_count != 0)) {
        return false;
    }

    const size_t head = atomic_load_explicit(
        &queue->head, memory_order_relaxed);
    const size_t next = (head + 1) % queue->storage_count;
    if (next == atomic_load_explicit(
                    &queue->tail, memory_order_acquire)) {
        return false;
    }

    remote_event_t *event = &queue->storage[head];
    event->type = type;
    event->turn_token = turn_token;
    event->value_count = value_count;
    event->sample_count = (uint16_t)sample_count;
    if (sample_count > 0) {
        memcpy(event->samples, samples, sample_count * sizeof(*samples));
    }
    atomic_store_explicit(&queue->head, next, memory_order_release);
    return true;
}

const remote_event_t *remote_event_queue_peek(
    const remote_event_queue_t *queue)
{
    if (!configured(queue)) {
        return NULL;
    }
    const size_t tail = atomic_load_explicit(
        &queue->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(
                    &queue->head, memory_order_acquire)) {
        return NULL;
    }
    return &queue->storage[tail];
}

bool remote_event_queue_consume(remote_event_queue_t *queue)
{
    if (!configured(queue)) {
        return false;
    }
    const size_t tail = atomic_load_explicit(
        &queue->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(
                    &queue->head, memory_order_acquire)) {
        return false;
    }
    atomic_store_explicit(
        &queue->tail, (tail + 1) % queue->storage_count,
        memory_order_release);
    return true;
}

size_t remote_event_queue_count(const remote_event_queue_t *queue)
{
    if (!configured(queue)) {
        return 0;
    }
    const size_t head = atomic_load_explicit(
        &queue->head, memory_order_acquire);
    const size_t tail = atomic_load_explicit(
        &queue->tail, memory_order_acquire);
    return head >= tail ? head - tail : queue->storage_count - tail + head;
}

void remote_event_queue_reset(remote_event_queue_t *queue)
{
    if (!configured(queue)) {
        return;
    }
    atomic_store_explicit(&queue->tail, 0, memory_order_relaxed);
    atomic_store_explicit(&queue->head, 0, memory_order_release);
}
