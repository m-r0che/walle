# Walle Cloud relay

Cloudflare Agents SDK relay for one Walle installation. The current provider is OpenAI `gpt-realtime-2.1` speech-to-speech with authenticated bounded PCM transport, rolling buffered device playback, exact sample validation, local fallback, versioned personality/voice direction, and turn-scoped semantic affect.

The Agent also owns the household tools (`walle-tools-v1`): a shopping list in Durable Object SQLite (read/add/undo with per-call idempotency), a prepare→confirm→commit print pipeline with a durable job state machine ([`docs/bridge-protocol.md`](../docs/bridge-protocol.md)), Code Mode document composition (model-written JavaScript in a network-isolated Dynamic Worker sandbox via `@cloudflare/codemode`), and SVG/text → PDF rendering through the Browser Run binding. Physical printing requires a spoken confirmation bound to a content digest; the Mac bridge in [`bridge/`](../bridge/) is the only component that touches the printer.

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

## Deployment

The relay is deployed to the Workers.dev prototype URL above. `DEVICE_TOKEN` and `BRIDGE_TOKEN` are installed as Worker secrets, and the matching random credentials are held outside this repository with owner-only filesystem permissions. The bridge routes are `GET /v1/bridge` (WebSocket wake-up hints), `POST /v1/bridge/claim`, and `POST /v1/bridge/ack`; authenticated `GET /v1/debug/shopping-list`, `GET /v1/debug/print-jobs`, and `POST /v1/debug/print-test` are bounded bring-up seams. Remote checks pass for health, unauthorized WebSocket rejection, authentication, Agent routing, control messages, and binary PCM echo.

For future releases:

```bash
cd cloud
npm run check
npm run deploy:dry
npx wrangler deploy
```

Do not place device or OpenAI credentials in `wrangler.jsonc`, source, shell history, or Git. `INSTALLATION_ID` is a non-secret routing label and may remain in config.

The device now sends a positive `sessionEpoch` in `hello`; `ready` echoes it and firmware rejects mismatched readiness. Manager-owned reconnect passed a forced clean close (~3 seconds), active Worker deployments, and an intentionally suppressed application pong (~20 seconds) without reboot. Authenticated `POST /v1/debug/disconnect-device`, `POST /v1/debug/suppress-pong`, `POST /v1/debug/drop-next-output-frame`, and `POST /v1/debug/mismatch-next-done-samples` exist only as bounded bring-up fault-injection seams and never expose credentials or PCM. The latter two are one-shot faults used to prove exact local fallback.
