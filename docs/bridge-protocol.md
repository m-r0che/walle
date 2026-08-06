# Print bridge protocol v1

The Mac print bridge is the only component allowed to reach the physical
printer. It authenticates to the relay with `Authorization: Bearer` using the
`BRIDGE_TOKEN` Worker secret (distinct from the device token) and follows the
job state machine in `cloud/src/printing.ts`:

```
prepared -> confirmed -> pending -> claimed -> submitted -> completed
                 |            |          |           |
   (ttl) cancelled   (ttl) failed   (lease) needs_review / failed
                                         (stale) needs_review / failed
```

## Transport

- `GET /v1/bridge` (WebSocket): wake-up hints only. The relay pushes
  `{"v":1,"type":"job.available","pending":n}` when a job becomes claimable
  and answers `{"v":1,"type":"hello"}` with
  `{"v":1,"type":"bridge.ready","printer":…,"pending":n}`. A dropped or
  missed hint can never lose a job: the bridge also polls.
- `POST /v1/bridge/claim`: atomically claims the oldest `pending` job.
  Returns `{"job":null}` or the full job: `jobId`, `claimId`, `kind`,
  `title`, `digest`, `printer`, `pages`, `payloadType`
  (`application/pdf` or `text/plain`), `payloadBase64`, `leaseSeconds`.
- `POST /v1/bridge/ack` with `{jobId, claimId, phase, localJobId?, failure?}`
  where `phase` is `submitted`, `completed`, or `failed`. Repeating the ack
  that produced the current state is a no-op so crash-recovery retries are
  safe. Acks with a stale `claimId` are rejected.

## Bridge obligations

1. Append a receipt to the local ledger before and after every `lp`
   submission (`claimed`, `lp_attempted`, `submitted`, `completed`).
2. Never run `lp` for a job whose ledger already shows `lp_attempted`; ack
   `failed` with an explanatory reason instead so the cloud can flag the
   ambiguity rather than double-print.
3. Only print to the single allowlisted printer; refuse jobs naming any
   other destination.
4. Ack `submitted` with the CUPS request id, then `completed` only after
   `lpstat` shows the job left the queue.

## Cloud-side timers

- Prepared jobs expire after 5 minutes unconfirmed (`cancelled`).
- Pending jobs expire after 15 minutes unclaimed (`failed`).
- Claimed jobs whose 90-second lease lapses without an ack become
  `needs_review` — the bridge may have printed before crashing.
- Submitted jobs with no completion ack after 5 minutes become
  `needs_review`.

`needs_review` is terminal until a human resolves it; nothing is ever
automatically resubmitted from an ambiguous state.

## Running the bridge

```bash
# One-off, dry-run (spools files instead of printing):
WALLE_BRIDGE_DRY_RUN=1 node bridge/walle-bridge.mjs

# Real printing:
node bridge/walle-bridge.mjs

# Install as a launchd service:
cp bridge/com.walle.bridge.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/com.walle.bridge.plist
```

The bridge token lives at
`~/Library/Application Support/Walle/secrets/bridge-token.txt` (0600,
outside the repository) and must match the Worker's `BRIDGE_TOKEN` secret.
Receipts are appended to
`~/Library/Application Support/Walle/bridge/receipts.jsonl`; spooled
payloads land next to it in `bridge/spool/`.
