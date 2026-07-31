#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*pcm_batcher_emit_fn)(void *context,
                                    const int16_t *samples,
                                    size_t sample_count);

typedef struct {
    int16_t *storage;
    size_t capacity;
    size_t buffered;
} pcm_batcher_t;

/*
 * Initializes a caller-owned, allocation-free PCM batcher. The storage must
 * remain valid until the batcher is no longer used.
 */
bool pcm_batcher_init(pcm_batcher_t *batcher, int16_t *storage,
                      size_t capacity);

/*
 * Preserves input order and emits each full batch synchronously. On emission
 * failure, the current turn's buffered samples are discarded and false is
 * returned so the caller can fail the remote copy closed.
 */
bool pcm_batcher_write(pcm_batcher_t *batcher, const int16_t *samples,
                       size_t sample_count, pcm_batcher_emit_fn emit,
                       void *context);

/* Emits a final partial batch, if any, and always clears buffered state. */
bool pcm_batcher_finish(pcm_batcher_t *batcher, pcm_batcher_emit_fn emit,
                        void *context);

/* Discards a cancelled or invalid turn without emitting it. */
void pcm_batcher_abort(pcm_batcher_t *batcher);

size_t pcm_batcher_buffered(const pcm_batcher_t *batcher);
