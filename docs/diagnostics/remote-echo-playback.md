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

The intended production adapter will allocate six seconds of PCM storage in PSRAM. WebSocket callbacks will copy already-validated frames into a bounded zero-wait event queue; only the audio task will mutate `remote_response`, choose remote versus local source, and write the codec.

## Off-device evidence

Run:

```bash
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Ifirmware/main \
  firmware/main/remote_response.c firmware/tests/remote_response_test.c \
  -o /tmp/remote_response_test
/tmp/remote_response_test
```

Covered behavior:

- exact ordered readout after matching buffered/relay/local sample counts;
- relay-count mismatch and local-count mismatch;
- capacity overflow with no partial readout;
- stale append/complete/invalidate/cancel isolation;
- timeout invalidation and explicit cancellation;
- new-turn replacement and invalid configuration;
- ASan/UBSan execution.

## Next controlled step

Integrate the module on this branch without changing the validated 256-sample codec cadence, 960-sample network batching, display path, or local rings. Before any flash, add host tests for the bounded callback-to-audio queue and response deadline/source selector. The first hardware candidate will retain echo mode and must pass local-fallback cases before any OpenAI connection is introduced.
