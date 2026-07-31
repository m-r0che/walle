#include "pcm_ring.h"

#include <string.h>

void pcm_ring_init(pcm_ring_t *ring, int16_t *storage, size_t capacity)
{
    if (ring == NULL) {
        return;
    }
    *ring = (pcm_ring_t) {
        .storage = storage,
        .capacity = capacity,
    };
}

void pcm_ring_reset(pcm_ring_t *ring)
{
    if (ring == NULL) {
        return;
    }
    ring->read_index = 0;
    ring->write_index = 0;
    ring->count = 0;
}

size_t pcm_ring_count(const pcm_ring_t *ring)
{
    return ring == NULL ? 0 : ring->count;
}

size_t pcm_ring_free(const pcm_ring_t *ring)
{
    return ring == NULL ? 0 : ring->capacity - ring->count;
}

size_t pcm_ring_write(pcm_ring_t *ring, const int16_t *samples, size_t count)
{
    if (ring == NULL || ring->storage == NULL || samples == NULL
            || ring->capacity == 0) {
        return 0;
    }

    const size_t writable = count < pcm_ring_free(ring)
        ? count : pcm_ring_free(ring);
    const size_t to_end = ring->capacity - ring->write_index;
    const size_t first = writable < to_end ? writable : to_end;
    memcpy(&ring->storage[ring->write_index], samples,
           first * sizeof(*samples));
    const size_t second = writable - first;
    if (second > 0) {
        memcpy(ring->storage, &samples[first], second * sizeof(*samples));
    }
    ring->write_index = (ring->write_index + writable) % ring->capacity;
    ring->count += writable;
    return writable;
}

size_t pcm_ring_read(pcm_ring_t *ring, int16_t *samples, size_t count)
{
    if (ring == NULL || ring->storage == NULL || samples == NULL
            || ring->capacity == 0) {
        return 0;
    }

    const size_t readable = count < ring->count ? count : ring->count;
    const size_t to_end = ring->capacity - ring->read_index;
    const size_t first = readable < to_end ? readable : to_end;
    memcpy(samples, &ring->storage[ring->read_index],
           first * sizeof(*samples));
    const size_t second = readable - first;
    if (second > 0) {
        memcpy(&samples[first], ring->storage, second * sizeof(*samples));
    }
    ring->read_index = (ring->read_index + readable) % ring->capacity;
    ring->count -= readable;
    return readable;
}
