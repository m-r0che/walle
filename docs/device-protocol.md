# Device relay protocol v1

The ESP32 connects to `GET /v1/device` over `wss://` with an `Authorization: Bearer …` header. The Worker authenticates before Agent lookup, removes the credential, derives the installation ID from server configuration, and forwards to one `WalleAgent` Durable Object. Default Agent identity/state/MCP frames are disabled.

## Audio

Audio is 24 kHz, mono, little-endian PCM16. Each WebSocket binary message is one complete bounded audio frame.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 | Magic bytes `0x57 0x41` (`WA`) |
| 2 | 1 | Protocol version (`1`) |
| 3 | 1 | Kind: `1` input PCM, `2` output PCM |
| 4 | 4 | Sequence number, unsigned little-endian |
| 8 | 2 | Sample count, unsigned little-endian |
| 10 | 2 | Flags, unsigned little-endian; currently zero |
| 12 | `samples × 2` | PCM16LE payload |

Frames are limited to 960 samples (40 ms). Input sequence numbers must be contiguous within a turn; the first input frame may use any unsigned 32-bit value. In `echo` mode, kind-2 frames must exactly match expected input sequence/count/hash records. In `openai` mode, generated kind-2 frames use their own contiguous sequence starting at zero; TLS-authenticated relay ownership plus turn/epoch/count bounds replace the echo-only hash equality.

## Control messages

Control messages are UTF-8 JSON text frames, limited to 4,096 encoded bytes. Every message includes `"v": 1`.

### Device → relay

```json
{"v":1,"type":"hello","firmware":"build-id","sampleRate":24000,"channels":1,"sampleFormat":"pcm16le","sessionEpoch":6}
{"v":1,"type":"turn.start","turnId":"device-generated-id"}
{"v":1,"type":"turn.commit","turnId":"device-generated-id"}
{"v":1,"type":"turn.cancel","turnId":"device-generated-id"}
{"v":1,"type":"response.cancel","turnId":"device-generated-id"}
{"v":1,"type":"playback.report","turnId":"device-generated-id","source":"local","samples":48000}
{"v":1,"type":"telemetry.report","epoch":6,"state":6,"captureAccepting":false,"queueDepth":0,"startsQueued":3,"startUnready":1,"startMutexBusy":0,"startAlreadyActive":0,"turnsStarted":3,"turnsCommitted":3,"audioDropped":0,"protocolErrors":0,"socketRestarts":1}
{"v":1,"type":"ping","nonce":"bounded-value"}
```

A successful `hello` is required before turns; binary input requires an active turn. `turn.cancel` aborts uncommitted input; `response.cancel` aborts generated output after input commit, such as when the device's bounded response deadline expires. Turn IDs and nonces are limited to 64 characters. `playback.report` is optional aggregate telemetry emitted after uninterrupted playback. It retains only the selected source and sample count; validated firmware reports `remote` only after complete response validation and reports `local` when a completed relay turn fails that gate. `telemetry.report` contains only bounded aggregate connectivity/capture counters—never PCM, credentials, SSIDs, or user content—and is sent periodically or after a capture-start decision.

### Relay → device

```json
{"v":1,"type":"ready","mode":"openai","sampleRate":24000,"channels":1,"sampleFormat":"pcm16le","sessionEpoch":6}
{"v":1,"type":"turn.started","turnId":"..."}
{"v":1,"type":"turn.done","turnId":"...","frames":287,"inputSamples":73472,"outputSamples":48000}
{"v":1,"type":"turn.cancelled","turnId":"..."}
{"v":1,"type":"pong","nonce":"..."}
```

`inputSamples` must match the complete committed device input; `outputSamples` must match the complete binary output stream and may differ in duration. Echo-era firmware also accepts the legacy single `samples` field as both counts. Remote PCM remains unreadable unless relay input matches authoritative local capture and buffered output matches relay output.

Malformed or out-of-order messages close the socket with application code `4002`. A newer connection to the same installation replaces the old socket with code `4009`. `sessionEpoch` is positive, increases for every device-side client recreation, and must match in `hello`/`ready`; queued items from older epochs are discarded. The device sends nonce-checked application heartbeats and recreates the client after a missing pong or bounded session lifetime. During bring-up, authenticated `GET /v1/debug/last-turn` returns connection state and aggregate counts for the latest turn; PCM is never retained or returned.

## Next protocol increments

- Add explicit server playback cancellation/drop acknowledgement before OpenAI.
- Add explicit response IDs and scope future playback/cancellation queues to both response ID and the existing connection epoch.
- Add bounded queue-depth/backpressure messages only if measurements show they are needed.
- Keep shopping/printing tool events as typed JSON; never mix them into binary audio payloads.
