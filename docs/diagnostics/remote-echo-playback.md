# Complete-turn remote echo playback diagnostic

**Branch:** `diagnostic/remote-echo-playback`

**Status:** response-buffer module tested off-device; not integrated or flashed

## Safety contract

Remote PCM may become speaker-readable only when all of these are true for the same positive local turn token:

1. Every binary output frame passed the relay's epoch, sequence, sample-count, and hash validation.
2. The bounded response buffer accepted every frame without overflow.
3. The relay sent a completion event for that turn.
4. Buffered samples, relay-reported samples, and authoritative local-capture samples are identical and nonzero.
5. Completion arrived before the audio owner's response deadline.
6. The turn was not muted, cancelled, interrupted, disconnected, or superseded.

Any failure leaves the existing local capture available for playback. Partial remote audio is never sent to the codec.

## Module seam

`remote_response` is an allocation-free complete-turn buffer over caller-owned storage. Its interface hides token isolation, bounded append, three-way sample-count validation, overflow/mismatch invalidation, cancellation, and readout gating. Stale-token events are rejected without damaging the current response. A new turn atomically discards older state.

The intended production adapter will allocate six seconds of PCM storage in PSRAM. `remote_event_queue` is a bounded allocation-free single-producer/single-consumer seam between the WebSocket event task and audio task. The producer copies already-validated frames directly into caller-owned slots and publishes them with release/acquire ordering; the consumer peeks in place, avoiding a second 1,920-byte copy and a large stack object. Only the audio task will mutate `remote_response`, choose remote versus local source, and write the codec.

`remote_response_select` centralizes timing policy: it preserves the authored minimum thinking interval, chooses only a complete response, waits no longer than the response deadline, invalidates an expired partial response, and otherwise selects local fallback.

## Off-device evidence

Run:

```bash
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Ifirmware/main \
  firmware/main/remote_response.c firmware/tests/remote_response_test.c \
  -o /tmp/remote_response_test
/tmp/remote_response_test

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -pthread -Ifirmware/main \
  firmware/main/remote_event_queue.c \
  firmware/tests/remote_event_queue_test.c \
  -o /tmp/remote_event_queue_test
/tmp/remote_event_queue_test
```

Covered behavior:

- exact ordered readout after matching buffered/relay/local sample counts;
- relay-count mismatch and local-count mismatch;
- capacity overflow with no partial readout;
- stale append/complete/invalidate/cancel isolation;
- timeout invalidation and explicit cancellation;
- new-turn replacement and invalid configuration;
- earliest-playback, completion, and exact deadline source selection;
- event validation, bounded-full rejection, wraparound, FIFO order, and reset;
- 100,000-event concurrent producer/consumer stress;
- ASan/UBSan execution.

## Next controlled step

Integrate both modules on this branch without changing the validated 256-sample codec cadence, 960-sample network batching, display path, or local rings. The first hardware candidate will retain echo mode and must pass exact remote playback plus timeout, mismatch, overflow, cancellation, reconnect, and local-fallback cases before any OpenAI connection is introduced.
