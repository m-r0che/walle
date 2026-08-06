# Flue framework assessment (2026-08-06)

Researched for the shopping-list and printer-bridge phases. Sources: flueframework.com docs, github.com/withastro/flue, npm.

## What it is

Flue is the Astro team's open-source TypeScript AI-agent framework (launched May 2026, `@flue/runtime` 2.0.3 as of Aug 5 2026, Apache-2.0, ~7.7k stars). Agents are functions marked `'use agent'` composing hooks (`useModel`, `useTool`, `usePersistentState`, `useSubagent`, `useSandbox`). Its pitch is durable, headless agent runs: survives restarts, provider timeouts, and disconnects via durable streams and session resumption. HTTP-addressable via Hono; triggered by API/webhook/cron.

## Cloudflare relationship

On the Cloudflare target Flue is built **on top of the Cloudflare Agents SDK** (`agents` package — the same one this repo pins). The Vite plugin generates one Durable Object class per agent with SQLite storage and stores the conversation as an append-only stream in DO SQLite. Client updates are HTTP streaming/SSE only; the docs do not expose the Agents SDK's WebSocket server, alarms, or scheduling to Flue code.

## Relevant APIs

- `usePersistentState<T>(name, default)` — a JSON whole-value cell per agent instance; equivalent to Agents SDK `this.setState`, strictly weaker than `this.sql`.
- `defineTool({ name, description, input, output, run })` with **Valibot** schemas; optional `durable: true` gives `step.do(name, fn)` exactly-once steps (Workflows-style).
- `dispatch(agent, request)` fire-and-forget inter-agent delivery; SDK client with a 202-accept `send()` then `read()`/`wait()` settlement pattern (`FlueExecutionError` on failure).

## Verdict for this project

- **Shopping list:** the Agents SDK alone is simpler and stronger. `this.sql` in the existing `WalleAgent` DO already gives transactional, queryable storage; Flue adds a Vite build pipeline and generated DO classes while offering only a JSON state cell.
- **Printer bridge:** poor fit for the critical leg. The bridge needs a persistent WebSocket from the DO to the Mac daemon with claim/ack/lease-expiry — exactly what the Agents SDK exposes (hibernatable WS server, `this.schedule()`/alarms) and exactly what Flue's Cloudflare target does not (SSE only, no alarm/cron hooks).
- **Where Flue would earn its keep later:** long-running autonomous LLM runs that must survive crashes mid-tool-call, multi-agent delegation, sandboxed execution — e.g. a future "household chores" agent that composes list + printing on a schedule.

Recommendation: implement both features directly on the Agents SDK; borrow Flue's patterns — Valibot-validated tool definitions and the 202-accept/settle job-admission shape — rather than its runtime.
