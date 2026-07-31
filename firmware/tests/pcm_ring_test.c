#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "pcm_ring.h"

static void expect_samples(const int16_t *actual, const int16_t *expected,
                           size_t count)
{
    for (size_t index = 0; index < count; index++) {
        assert(actual[index] == expected[index]);
    }
}

int main(void)
{
    int16_t storage[5] = {0};
    pcm_ring_t ring;
    pcm_ring_init(&ring, storage, 5);
    assert(pcm_ring_count(&ring) == 0);
    assert(pcm_ring_free(&ring) == 5);

    const int16_t first[] = {1, 2, 3, 4};
    assert(pcm_ring_write(&ring, first, 4) == 4);
    int16_t output[8] = {0};
    assert(pcm_ring_read(&ring, output, 3) == 3);
    expect_samples(output, first, 3);

    const int16_t wrapped[] = {5, 6, 7, 8, 9};
    assert(pcm_ring_write(&ring, wrapped, 5) == 4);
    assert(pcm_ring_count(&ring) == 5);
    assert(pcm_ring_free(&ring) == 0);

    const int16_t expected_wrapped[] = {4, 5, 6, 7, 8};
    assert(pcm_ring_read(&ring, output, 8) == 5);
    expect_samples(output, expected_wrapped, 5);
    assert(pcm_ring_count(&ring) == 0);

    assert(pcm_ring_write(&ring, wrapped, 2) == 2);
    pcm_ring_reset(&ring);
    assert(pcm_ring_count(&ring) == 0);
    assert(pcm_ring_free(&ring) == 5);

    pcm_ring_init(&ring, NULL, 0);
    assert(pcm_ring_write(&ring, first, 1) == 0);
    assert(pcm_ring_read(&ring, output, 1) == 0);

    puts("pcm_ring_test: PASS");
    return 0;
}
