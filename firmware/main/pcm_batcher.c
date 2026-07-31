#include "pcm_batcher.h"

#include <string.h>

bool pcm_batcher_init(pcm_batcher_t *batcher, int16_t *storage,
                      size_t capacity)
{
    if (batcher == NULL || storage == NULL || capacity == 0) {
        return false;
    }
    *batcher = (pcm_batcher_t) {
        .storage = storage,
        .capacity = capacity,
        .buffered = 0,
    };
    return true;
}

bool pcm_batcher_write(pcm_batcher_t *batcher, const int16_t *samples,
                       size_t sample_count, pcm_batcher_emit_fn emit,
                       void *context)
{
    if (batcher == NULL || batcher->storage == NULL
            || batcher->capacity == 0 || emit == NULL
            || (sample_count > 0 && samples == NULL)) {
        return false;
    }

    size_t consumed = 0;
    while (consumed < sample_count) {
        const size_t available = batcher->capacity - batcher->buffered;
        const size_t remaining = sample_count - consumed;
        const size_t copied = remaining < available ? remaining : available;
        memcpy(&batcher->storage[batcher->buffered], &samples[consumed],
               copied * sizeof(*samples));
        batcher->buffered += copied;
        consumed += copied;

        if (batcher->buffered == batcher->capacity) {
            const bool emitted = emit(context, batcher->storage,
                                      batcher->buffered);
            batcher->buffered = 0;
            if (!emitted) {
                return false;
            }
        }
    }
    return true;
}

bool pcm_batcher_finish(pcm_batcher_t *batcher, pcm_batcher_emit_fn emit,
                        void *context)
{
    if (batcher == NULL || batcher->storage == NULL
            || batcher->capacity == 0 || emit == NULL) {
        return false;
    }
    if (batcher->buffered == 0) {
        return true;
    }
    const size_t sample_count = batcher->buffered;
    const bool emitted = emit(context, batcher->storage, sample_count);
    batcher->buffered = 0;
    return emitted;
}

void pcm_batcher_abort(pcm_batcher_t *batcher)
{
    if (batcher != NULL) {
        batcher->buffered = 0;
    }
}

size_t pcm_batcher_buffered(const pcm_batcher_t *batcher)
{
    return batcher == NULL ? 0 : batcher->buffered;
}
