#include "pcm_batcher.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))

typedef struct {
    int16_t samples[4096];
    size_t sample_count;
    size_t frame_sizes[16];
    size_t frame_count;
    size_t fail_on_frame;
} collector_t;

static bool collect(void *context, const int16_t *samples,
                    size_t sample_count)
{
    collector_t *collector = context;
    collector->frame_count++;
    if (collector->fail_on_frame == collector->frame_count) {
        return false;
    }
    assert(collector->frame_count <= ARRAY_SIZE(collector->frame_sizes));
    assert(collector->sample_count + sample_count
           <= ARRAY_SIZE(collector->samples));
    collector->frame_sizes[collector->frame_count - 1] = sample_count;
    memcpy(&collector->samples[collector->sample_count], samples,
           sample_count * sizeof(*samples));
    collector->sample_count += sample_count;
    return true;
}

static void fill_sequence(int16_t *samples, size_t count, int start)
{
    for (size_t index = 0; index < count; index++) {
        samples[index] = (int16_t)(start + (int)index);
    }
}

static void assert_sequence(const int16_t *samples, size_t count, int start)
{
    for (size_t index = 0; index < count; index++) {
        assert(samples[index] == (int16_t)(start + (int)index));
    }
}

static void test_four_codec_chunks_cross_one_network_frame(void)
{
    int16_t storage[960];
    int16_t chunk[256];
    pcm_batcher_t batcher;
    collector_t collector = {0};
    assert(pcm_batcher_init(&batcher, storage, ARRAY_SIZE(storage)));

    for (int chunk_index = 0; chunk_index < 4; chunk_index++) {
        fill_sequence(chunk, ARRAY_SIZE(chunk), chunk_index * 256);
        assert(pcm_batcher_write(&batcher, chunk, ARRAY_SIZE(chunk),
                                 collect, &collector));
    }

    assert(collector.frame_count == 1);
    assert(collector.frame_sizes[0] == 960);
    assert(pcm_batcher_buffered(&batcher) == 64);
    assert(pcm_batcher_finish(&batcher, collect, &collector));
    assert(collector.frame_count == 2);
    assert(collector.frame_sizes[1] == 64);
    assert(collector.sample_count == 1024);
    assert_sequence(collector.samples, collector.sample_count, 0);
    assert(pcm_batcher_buffered(&batcher) == 0);
}

static void test_arbitrary_input_preserves_order(void)
{
    int16_t storage[7];
    int16_t input[23];
    pcm_batcher_t batcher;
    collector_t collector = {0};
    fill_sequence(input, ARRAY_SIZE(input), -11);
    assert(pcm_batcher_init(&batcher, storage, ARRAY_SIZE(storage)));

    assert(pcm_batcher_write(&batcher, input, 3, collect, &collector));
    assert(pcm_batcher_write(&batcher, &input[3], 17, collect, &collector));
    assert(pcm_batcher_write(&batcher, &input[20], 3, collect, &collector));
    assert(pcm_batcher_finish(&batcher, collect, &collector));

    assert(collector.frame_count == 4);
    assert(collector.frame_sizes[0] == 7);
    assert(collector.frame_sizes[1] == 7);
    assert(collector.frame_sizes[2] == 7);
    assert(collector.frame_sizes[3] == 2);
    assert(collector.sample_count == ARRAY_SIZE(input));
    assert_sequence(collector.samples, collector.sample_count, -11);
}

static void test_emit_failure_discards_buffered_turn(void)
{
    int16_t storage[8];
    int16_t input[12];
    pcm_batcher_t batcher;
    collector_t failed = {.fail_on_frame = 1};
    collector_t retry = {0};
    fill_sequence(input, ARRAY_SIZE(input), 100);
    assert(pcm_batcher_init(&batcher, storage, ARRAY_SIZE(storage)));

    assert(!pcm_batcher_write(&batcher, input, ARRAY_SIZE(input),
                              collect, &failed));
    assert(pcm_batcher_buffered(&batcher) == 0);

    assert(pcm_batcher_write(&batcher, input, 4, collect, &retry));
    assert(pcm_batcher_finish(&batcher, collect, &retry));
    assert(retry.frame_count == 1);
    assert(retry.sample_count == 4);
    assert_sequence(retry.samples, retry.sample_count, 100);
}

static void test_abort_and_empty_finish(void)
{
    int16_t storage[8];
    const int16_t input[] = {1, 2, 3};
    pcm_batcher_t batcher;
    collector_t collector = {0};
    assert(!pcm_batcher_init(NULL, storage, ARRAY_SIZE(storage)));
    assert(!pcm_batcher_init(&batcher, NULL, ARRAY_SIZE(storage)));
    assert(!pcm_batcher_init(&batcher, storage, 0));
    assert(pcm_batcher_init(&batcher, storage, ARRAY_SIZE(storage)));

    assert(pcm_batcher_write(&batcher, input, ARRAY_SIZE(input),
                             collect, &collector));
    pcm_batcher_abort(&batcher);
    assert(pcm_batcher_buffered(&batcher) == 0);
    assert(pcm_batcher_finish(&batcher, collect, &collector));
    assert(collector.frame_count == 0);
}

int main(void)
{
    test_four_codec_chunks_cross_one_network_frame();
    test_arbitrary_input_preserves_order();
    test_emit_failure_discards_buffered_turn();
    test_abort_and_empty_finish();
    puts("pcm_batcher_test: PASS");
    return 0;
}
