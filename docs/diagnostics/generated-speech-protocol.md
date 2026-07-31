# Generated-speech protocol evolution

**Branch:** `feature/openai-realtime`

**Status:** OpenAI adapter and firmware protocol built/tested off-device; not flashed or deployed

## Why the echo invariant must evolve

The validated echo provider returns exactly the user's PCM, so its input and output sample counts are identical. Generated speech is independently authored and will normally have a different duration. Requiring generated output length to equal user input length would reject every otherwise-valid OpenAI response.

The evolved complete-turn gate keeps two independent equalities:

1. Relay-reported input samples must equal the authoritative local capture count.
2. Buffered output samples must equal relay-reported output samples.

Input and output may differ from each other. Both remain nonzero and bounded to six seconds. Epoch, turn token, sequence, frame size, hash, queue capacity, completion deadline, cancellation, and source-ownership checks remain unchanged.

The relay completion message is now:

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

Firmware remains backward-compatible with the echo-era `samples` field, interpreting it as both input and output counts.

## Off-device evidence

ASan/UBSan host tests now include a complete response whose 960 output samples correspond to 1,920 authoritative input samples. Exact output becomes readable. Mismatched output counts and mismatched relay-input/local-input counts remain invalid and expose no partial PCM. The integrated queue → response → selector test also passes this differing-duration case.

The firmware build passes with unchanged 256-sample codec cadence, 960-sample network frame maximum, 35 ms display pacing, and no display-source changes. In `openai` mode, output uses a separate contiguous sequence beginning at zero; echo mode retains its stricter input sequence/count/hash equality. The response deadline is provisionally extended from the echo-only 500 ms to a bounded six seconds for concise generated speech; expiry publishes a scoped `response.cancel` before local fallback. This requires physical latency and fallback validation before promotion.

## OpenAI adapter

The Agent-owned adapter uses the official server-to-server `gpt-realtime-2.1` WebSocket protocol. It authenticates from the Worker secret, sends a SHA-256 installation safety identifier, configures 24 kHz PCM with VAD disabled, clears input before each push-to-talk turn, appends Base64 PCM, manually commits, and requests audio-only output using the `marin` voice.

Output deltas are decoded, bounded to six seconds, split into 960-sample device frames, and scoped to response metadata/ID. Only a completed response emits `turn.done`; OpenAI error, close, identity mismatch, malformed Base64, odd PCM, output overflow, and incomplete status fail closed. Unexpected upstream loss forces a device reconnect so a fresh provider session or echo fallback is explicitly negotiated in `ready`.

Vitest covers session configuration, manual input flow, bounded output re-chunking, response identity/status failure, and cancellation. Agent debug status adds provider and bounded generation timing metadata (`firstAudioMs`, `totalMs`, input/output samples); PCM remains unpersisted.
