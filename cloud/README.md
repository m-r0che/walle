# Walle Cloud relay

Cloudflare Agents SDK relay for one Walle installation. The current provider is OpenAI `gpt-live-1` over the Live API (`wss://api.openai.com/v1/live/sessions`), with authenticated bounded PCM transport, rolling buffered device playback, exact sample validation, local fallback, versioned personality/voice direction, and turn-scoped semantic affect.

Deployed prototype: `wss://walle-relay.matt-ce8.workers.dev/v1/device`

Health check: `https://walle-relay.matt-ce8.workers.dev/health`

## Security and routing

- Device endpoint: `GET /v1/device` with `Upgrade: websocket`.
- Authentication: `Authorization: Bearer …`; tokens are SHA-256 hashed and compared with `node:crypto`'s `timingSafeEqual`.
- The client cannot choose an Agent instance. `INSTALLATION_ID` is read server-side and routed with `getAgentByName()`.
- The public Worker does not expose default `/agents/...` routing.
- Agent identity/state/MCP protocol messages are disabled for device connections.
- The authorization header is removed before forwarding to the Agent.
- Audio frames are bounded to 40 ms of 24 kHz mono PCM16 and are never stored in SQLite. The last 100 aggregate turn records retain only frame/sample counts and timestamps.
- Authenticated `GET /v1/debug/last-turn` exposes bounded connection state plus latest aggregate turn/playback-source, latency, personality, voice-profile, and affect metrics for bring-up; it never returns PCM, transcripts, or credentials.

## Local development

```bash
cd cloud
npm install
cp .dev.vars.example .dev.vars
# Replace DEVICE_TOKEN in .dev.vars with a local random value.
npm run check
npm run dev
```

Health check: `GET http://localhost:8787/health`.

The binary frame format is defined in [`src/protocol.ts`](src/protocol.ts). A device must send `hello`, then `turn.start`, then input PCM frames. In echo mode each input frame is returned immediately as an output PCM frame. Frame sequences must be contiguous within a turn, but the first sequence may be any unsigned 32-bit value. `turn.commit` finishes the turn; `turn.cancel` drops it.

## Live turn model

The Live API has no input commit and no end-of-reply event, so [`src/live-turn.ts`](src/live-turn.ts) supplies the turn machine as a pure reducer over `(policy, state, input, now)`, and [`src/openai-live.ts`](src/openai-live.ts) owns the socket, the JSON, and one timer.

- `turn.start` opens a hold. The Agent stores the input frames and sends nothing upstream.
- `turn.commit` sends the whole utterance as one burst: `session.input_audio.unmute`, one `session.input_audio.append` per 960 samples, then `session.input_audio.mute`. If an earlier reply is still draining, the commit waits and bursts when the stream is quiet.
- A reply ends 1200 ms after its last output audio or transcript delta. `turn.done` then reports exactly the samples forwarded, including the final partial frame. Every forwarded frame before the last is 1920 bytes.
- `response.cancel` after a commit drops the rest of that reply. Live has no speech cancel, so the adapter discards the tail until 1200 ms of quiet. A new `turn.start` during a reply fails the old turn with `superseded`.
- No output within 6 s fails the turn with `no_upstream_audio`. Output past 28 s fails it with `output_overflow`. A cancelled reply still arriving 20 s later closes the session.
- A failed `session.start` falls back to echo. A socket loss after `session.started` closes the device connection with code 1012 so the firmware reconnects.

## Deployment

The relay is deployed to the Workers.dev prototype URL above. `DEVICE_TOKEN` is installed as a Worker secret, and the matching random device credential is held outside this repository with owner-only filesystem permissions. Remote checks pass for health, unauthorized WebSocket rejection, authentication, Agent routing, control messages, and binary PCM echo.

For future releases:

```bash
cd cloud
npm run check
npm run deploy:dry
npx wrangler deploy
```

Do not place device or OpenAI credentials in `wrangler.jsonc`, source, shell history, or Git. `INSTALLATION_ID` is a non-secret routing label and may remain in config.

The device now sends a positive `sessionEpoch` in `hello`; `ready` echoes it and firmware rejects mismatched readiness. Manager-owned reconnect passed a forced clean close (~3 seconds), active Worker deployments, and an intentionally suppressed application pong (~20 seconds) without reboot. Authenticated `POST /v1/debug/disconnect-device`, `POST /v1/debug/suppress-pong`, `POST /v1/debug/drop-next-output-frame`, and `POST /v1/debug/mismatch-next-done-samples` exist only as bounded bring-up fault-injection seams and never expose credentials or PCM. The latter two are one-shot faults used to prove exact local fallback.
