# Walle Cloud relay

Cloudflare Agents SDK relay for one Walle installation. The current provider is an authenticated PCM echo transport with validated complete-turn device playback and local fallback. OpenAI and tools are the next provider phase.

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
- Authenticated `GET /v1/debug/last-turn` exposes bounded connection state plus latest aggregate turn/playback-source metrics for bring-up; it never returns PCM or credentials.

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
