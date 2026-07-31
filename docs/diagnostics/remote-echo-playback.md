# Complete-turn remote echo playback diagnostic

**Branch:** `diagnostic/remote-echo-playback`

**Status:** physically validated and promoted as the current recovery baseline

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

The production adapter allocates six seconds of PCM storage in PSRAM. `remote_event_queue` is a bounded allocation-free single-producer/single-consumer seam between the WebSocket event task and audio task. The producer copies already-validated frames directly into caller-owned slots and publishes them with release/acquire ordering; the consumer peeks in place, avoiding a second 1,920-byte copy and a large stack object. Only the audio task will mutate `remote_response`, choose remote versus local source, and write the codec.

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

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Ifirmware/main \
  firmware/main/remote_event_queue.c firmware/main/remote_response.c \
  firmware/tests/remote_pipeline_test.c \
  -o /tmp/remote_pipeline_test
/tmp/remote_pipeline_test
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
- end-to-end queue → complete-response → source-selection → exact readout;
- missing-frame and queue-overflow local-fallback paths with no partial readout;
- ASan/UBSan execution.

## Integrated candidate

The branch now wires the tested modules into the existing owners:

- The audio task assigns positive local turn tokens, preserves the local capture ring, drains remote events, validates the three sample counts, and exclusively selects/writes the playback source.
- The WebSocket event task forwards only epoch/sequence/count/hash-validated output through the zero-wait SPSC queue.
- The network manager retains 256 → 960 uplink batching, associates output with the local token, parses turn completion, and queues aggregate playback reports.
- Remote completion waits no longer than 500 ms after release and never bypasses the existing 220 ms authored thinking interval.
- Reconnect, event loss, mismatch, overflow, timeout, mute, interruption, and stale tokens leave local capture authoritative.

No display files changed. `FRAME_SAMPLES` remains 256, `NETWORK_FRAME_SAMPLES` remains 960, and the CO5300 pacing path remains unchanged.

## Physical validation

The exact firmware candidate is source commit `a9b37f8` with app SHA-256 `6310d81102ef42c3b272dc1a83564292a6c225bfc736a3b7c906550c7dfab9c8`. Cloud fault validation used deployed Worker version `e128a3b2-e7de-4cab-a409-6184bf5a0558`.

All promotion gates passed without native USB serial during visibility checks:

- Initial 15-second power-off and 60-second continuously visible cold boot.
- Exact remote playback: 59,392 samples / 62 frames.
- Exact long remote playback: 126,208 samples / 132 frames (5.26 seconds).
- Forced reconnect in roughly three seconds, epoch 1 → 2.
- Exact post-reconnect remote playback: 53,760 samples / 56 frames.
- One intentionally omitted output frame: exact 36,608-sample local fallback after 39 input frames.
- Intentionally wrong completion count: exact 52,736-sample local fallback after 55 input frames.
- Immediate playback interruption/new recording, followed by exact 36,864-sample remote playback.
- Two-minute continuously visible soak with 13/13 ready checks and planned epoch 4 → 5 refresh.
- Final 15-second power-off and 60-second continuously visible cold boot.
- Final fresh-boot exact remote playback: 47,104 samples / 50 frames.

The face and audio remained normal throughout. Every successful input frame count equals `ceil(samples / 960)`, and every playback report exactly matches its authoritative local capture count. The fault cases explicitly reported `source: local`; normal cases reported `source: remote`.

The owner-only recovery set is stored outside Git at:

`~/Library/Application Support/Walle/recovery/walle-remote-echo-coldboot-visible-6310d81102ef42c3/`

## Next controlled step

The bounded PCM path is now ready for a Cloudflare-side OpenAI Realtime adapter. Echo mode remains the fallback provider until credentials are supplied and the same completion, cancellation, reconnect, fallback, visibility, and cold-boot gates pass with generated speech.
