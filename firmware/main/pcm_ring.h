#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * Bounded single-owner PCM16 ring.
 *
 * Storage is supplied by the caller so the owner controls whether it lives in
 * internal RAM or PSRAM. Writes never overwrite unread samples; callers must
 * treat a short write as backpressure/overflow. The audio task is currently
 * the sole owner, avoiding locks in the I2S path.
 */
typedef struct {
    int16_t *storage;
    size_t capacity;
    size_t read_index;
    size_t write_index;
    size_t count;
} pcm_ring_t;

void pcm_ring_init(pcm_ring_t *ring, int16_t *storage, size_t capacity);
void pcm_ring_reset(pcm_ring_t *ring);
size_t pcm_ring_count(const pcm_ring_t *ring);
size_t pcm_ring_free(const pcm_ring_t *ring);
size_t pcm_ring_write(pcm_ring_t *ring, const int16_t *samples, size_t count);
size_t pcm_ring_read(pcm_ring_t *ring, int16_t *samples, size_t count);
