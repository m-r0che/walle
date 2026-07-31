# Generated-speech protocol and buffered playback

**Branch:** `feature/openai-realtime`

**Status:** buffered-streaming candidate built and host-tested; not yet flashed or deployed

## Independent input and output accounting

Generated speech does not have the same duration as user input. The protocol therefore keeps two independent final checks:

1. Relay-reported input samples must equal authoritative local capture.
2. Total received output samples must equal relay-reported output.

Input and output may differ. Each is independently bounded to 30 seconds by device and cloud resource policy. Echo mode retains the stricter input sequence/count/hash equality.

```json
{
  "v": 1,
  "type": "turn.done",
  "turnId": "...",
  "frames": 50,
  "inputSamples": 47104,
  "outputSamples": 32000
}
```

Firmware remains backward-compatible with the echo-era `samples` field, interpreting it as both counts.

## Buffered-streaming source contract

The relay explicitly negotiates `"playback":"buffered"` only for the OpenAI provider. The device still validates epoch, token, contiguous sequence, frame size, and bounded storage before publication to the audio owner.

The audio task starts remote playback after 500 ms of contiguous PCM is buffered. Source ownership then has a one-way boundary:

- **Before the first remote codec write:** incomplete, invalid, cancelled, late, or absent output leaves authoritative local fallback available.
- **After remote playback commits:** local echo is never mixed in. A gap, cancellation, late count mismatch, or timeout stops playback and exposes an error state because already-spoken audio cannot be undone.

Final `turn.done` accounting remains mandatory and successful uninterrupted playback is reported only after all declared output is consumed. Touch interruption cancels pending generation, clears buffered audio, and starts the replacement capture.

The device retains a 10-second initial response deadline. Valid in-order progress advances a five-second stall deadline, bounded by a 45-second absolute wait. PCM and credentials are never persisted.

## Resource changes

The candidate keeps all proven timing invariants:

- 256-sample codec cadence;
- 960-sample network frames;
- 35 ms minimum display submission interval;
- no display initialization changes.

PSRAM-owned bounds are intentionally independent:

- 30-second local capture;
- 30-second generated response;
- 640-entry uplink queue;
- 1,024-entry downlink event queue;
- 500 ms playback staging.

The deeper queues replace prototype scarcity rather than changing task ownership. WebSocket callbacks remain nonblocking and allocation-free; only the audio task writes the codec.

## OpenAI adapter

The Agent-owned adapter uses the official server-to-server `gpt-realtime-2.1` WebSocket protocol. It authenticates from a Worker secret, sends an installation-derived SHA-256 safety identifier, configures 24 kHz PCM with VAD disabled, clears input before push-to-talk turns, manually commits, and requests audio-only output using `marin`.

Output deltas are identity-checked, decoded, bounded to 30 seconds, and split into 960-sample frames. The initial candidate retains 20 ms downlink pacing; buffered playback begins before `turn.done`, so full response duration no longer determines time-to-first-sound. After physical validation with deep queues, unpaced, 5 ms, 10 ms, and 20 ms delivery can be compared independently.

OpenAI error, close, identity mismatch, malformed Base64, odd PCM, output overflow, and incomplete status fail closed. Unexpected upstream loss forces a device reconnect so provider availability is explicitly renegotiated.

## Evidence

ASan/UBSan host tests cover:

- differing input/output durations;
- exact complete-turn readout;
- starting only after a contiguous jitter threshold;
- reading while output remains in progress;
- appending after playback starts;
- successful final count validation after partial readout;
- late mismatch invalidation;
- overflow, cancellation, stale tokens, timeout selection, and queue stress.

Cloud Vitest covers session configuration, manual input flow, bounded output re-chunking, response identity/status failure, and cancellation. Aggregate diagnostics expose provider state, generation timing/failure reason, capture/drop reasons, queue pressure, and protocol counts without PCM, credentials, SSIDs, or user content.
