#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REMOTE_EVENT_MAX_SAMPLES 960

typedef enum {
    REMOTE_EVENT_AUDIO = 0,
    REMOTE_EVENT_DONE,
    REMOTE_EVENT_CANCELLED,
    REMOTE_EVENT_INVALID,
} remote_event_type_t;

typedef struct {
    remote_event_type_t type;
    uint32_t turn_token;
    uint32_t value_count;
    uint32_t input_count;
    uint16_t sample_count;
    int16_t samples[REMOTE_EVENT_MAX_SAMPLES];
} remote_event_t;

typedef struct {
    remote_event_t *storage;
    size_t storage_count;
    atomic_size_t head;
    atomic_size_t tail;
} remote_event_queue_t;

/*
 * Initializes a caller-owned SPSC queue. Usable capacity is storage_count - 1;
 * one WebSocket event task is the producer and one audio task is the consumer.
 */
bool remote_event_queue_init(remote_event_queue_t *queue,
                             remote_event_t *storage,
                             size_t storage_count);

/* Copies and publishes one event without allocation, waiting, or locks. */
bool remote_event_queue_try_push(remote_event_queue_t *queue,
                                 remote_event_type_t type,
                                 uint32_t turn_token,
                                 const int16_t *samples,
                                 size_t sample_count,
                                 uint32_t value_count,
                                 uint32_t input_count);

/*
 * Returns the next immutable event in place. It remains valid until consume;
 * this avoids a second 1,920-byte copy and a large audio-task stack object.
 */
const remote_event_t *remote_event_queue_peek(
    const remote_event_queue_t *queue);
bool remote_event_queue_consume(remote_event_queue_t *queue);

size_t remote_event_queue_count(const remote_event_queue_t *queue);

/* May be called only while producer and consumer are quiescent. */
void remote_event_queue_reset(remote_event_queue_t *queue);
