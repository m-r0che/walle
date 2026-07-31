# Final plan: Walle voice robot

**Date:** 2026-07-30

**Status:** implementation in progress; core hardware characterization passed

**Inputs:** [primary research](research/direction-of-travel.md) plus review of the `claude-walle-research` pane

**Confirmed after planning:** the target is a paper printer on the same home Wi-Fi; OpenAI API and Cloudflare accounts are available but credentials/access will be provided only when the cloud phase starts. The private factory-flash inspection strongly identifies the attached board as the CO5300-based V2; a physical-marking check remains desirable.

## 1. Final decisions

### Firmware

- Build on **ESP-IDF, Waveshare's revision-aware BSP, and LVGL 9**.
- Use Waveshare's board-check, I²S, and LVGL examples as the canonical starting point.
- Mine `chat-stick`, StackChan, and XiaoZhi for patterns; do not fork any of them as the product base.
- Keep rendering, audio I/O, buttons/touch, mute, buffering, and immediate interaction state local. The face must remain responsive offline.

XiaoZhi is worth adding to the reference set because it has separate targets for both revisions of this exact board and captures practical initialization details: TCA9554-controlled panel power/reset, AXP2101 setup, command `0x51` brightness, FT5x06 versus CST816-family touch, and 24 kHz input/output configuration. This is evidence that the pieces have been integrated, not proof of our required frame rate, full-duplex/AEC quality, or latency. ([V1 board source](https://github.com/78/xiaozhi-esp32/blob/dd99da00dc4c89ed4ab07fcec038c03f13f4de50/main/boards/waveshare/esp32-s3-touch-amoled-1.8/esp32-s3-touch-amoled-1.8.cc), [V1 config](https://github.com/78/xiaozhi-esp32/blob/dd99da00dc4c89ed4ab07fcec038c03f13f4de50/main/boards/waveshare/esp32-s3-touch-amoled-1.8/config.h), [V2 board source](https://github.com/78/xiaozhi-esp32/blob/dd99da00dc4c89ed4ab07fcec038c03f13f4de50/main/boards/waveshare/esp32-s3-touch-amoled-1.8-v2/esp32-s3-touch-amoled-1.8-v2.cc))

### Cloud coordinator

Use a **pinned Cloudflare Agents SDK `Agent` instance per robot/household installation**, rather than a bare Durable Object, but keep its media-relay code deliberately shallow and provider-neutral.

This resolves the earlier design disagreement:

- An Agent is the Durable Object-backed coordinator; it does not introduce another application hop.
- It provides the WebSocket lifecycle, embedded SQLite, scheduling, state synchronization, and later web-client support that the roadmap is likely to need.
- Raw audio handling should use simple binary WebSocket messages and a plain outbound provider socket—not `AIChatAgent`, an STT→LLM→TTS abstraction, or experimental voice orchestration in the hot path.
- Pin the SDK version and isolate it behind our own `SessionCoordinator` and `VoiceProvider` interfaces.
- Do not expose default caller-selected `/agents/{class}/{instance}` routing to devices. Authenticate at a custom Worker route, derive the installation ID server-side, then call `getAgentByName()`.
- Disable Agent identity and built-in protocol messages for the ESP32 connection so the device sees only our versioned wire protocol.
- Choose and benchmark the Agent `locationHint` before creating the durable production installation; placement affects the device → Agent → OpenAI path.

Cloudflare documents binary Agent WebSocket messages, pre-connect authentication/custom routing, automatic hibernation for idle inbound connections, and embedded SQLite via `this.sql`. The active outbound OpenAI socket prevents useful hibernation during a conversation, so it should be opened early when a turn/session begins and closed when the conversation is idle. ([Agents API](https://developers.cloudflare.com/agents/api-reference/agents-api/), [Agent routing](https://developers.cloudflare.com/agents/api-reference/routing/), [Agent WebSockets](https://developers.cloudflare.com/agents/api-reference/websockets/), [Durable Object WebSockets](https://developers.cloudflare.com/durable-objects/best-practices/websockets/))

### State and delivery

- Put the prototype's own shopping list, preferences, persona version, action audit, idempotency records, and print-job state in the Agent's embedded SQLite.
- If the chosen shopping-list system is external, it remains the system of record; SQLite then stores preferences, operation IDs, and audit state around an adapter.
- Do **not** add D1 initially. Add it when cross-agent/global queries, external SQL access, import/export, or independent administration justify it.
- Do **not** add Cloudflare Queues for the first single-household printer bridge. Model a durable job queue in SQLite with leases, idempotency, explicit acknowledgement, and recovery states. Move to Queues when there are multiple consumers or operational scale warrants it.
- The local print bridge keeps an outbound WebSocket only as a wake-up hint. It must pull/claim jobs through an authenticated endpoint and acknowledge them; a dropped WebSocket message must not lose or duplicate a job.

This combines the stronger local simplicity from Claude's plan with the original plan's safer at-least-once delivery semantics.

### Voice model and transport

- Start with **`gpt-realtime-2.1`** so model quality is not a confounding variable while debugging the new pipeline.
- Make the model a server-side per-session setting and A/B test `gpt-realtime-2.1-mini` once the loop is stable. Mini is a likely daily-driver candidate, not the bring-up default.
- Begin with **push-to-talk and 24 kHz, mono, little-endian PCM16**. OpenAI's current schema expresses this as `audio/pcm` at rate `24000`; pin a contract test against the schema when implementation begins.
- Disable VAD for the first version. On release, send `input_audio_buffer.commit` and then `response.create`; clear the input buffer before the next turn.
- Test 20, 40, 60, and 100 ms device chunks. Add Opus only if measured bandwidth, packet-loss, or cost pressure justifies the device CPU and relay transcoding complexity.
- Keep direct WebRTC as a measured fallback, using Espressif's maintained [`esp-webrtc-solution` OpenAI demo](https://github.com/espressif/esp-webrtc-solution/tree/main/solutions/openai_demo) as the reference if the relay cannot meet the latency target.
- Do not promise a frame rate, sub-second response, full duplex, AEC quality, or monthly cost before measuring it on this board and account.

## 2. Target architecture

```text
Waveshare ESP32-S3
  local face + buttons/touch + mic/speaker + audio queues
           |
           | WSS
           | binary sequenced PCM + small versioned JSON control messages
           v
Cloudflare Worker ingress
  authentication, coarse limits, route to installation
           |
           v
InstallationAgent (Agents SDK / Durable Object)
  live session coordinator
  OpenAI protocol adapter
  character constitution + preferences
  tool policy and confirmation state
  embedded SQLite: list, jobs, audit, idempotency
       |                         |
       | outbound WebSocket     | authenticated pull/ack
       v                         v
OpenAI Realtime            Local print bridge
                           local durable receipt ledger
                                  |
                                  v
                           allowlisted CUPS/IPP printer
```

AI Gateway remains an optional benchmark/observability layer. It is not required in the first hot path, and payload logging must be disabled if it is introduced.

## 3. Device design

### Runtime responsibilities

Use bounded queues with single owners:

```text
I²S RX/DMA -> capture ring -> WSS sender
WSS -> playback jitter ring -> I²S TX/DMA
                            -> envelope follower -> mouth target

buttons/touch -> interaction state
model affect -> bounded affect target
blink + gaze + breath + state + mouth -> LVGL renderer
```

Priorities:

1. Audio capture/playback and underrun prevention.
2. Network receive/send and cancellation.
3. Interaction controls and local mute.
4. Face updates, targeting roughly 30 Hz initially.
5. Persistence, telemetry, and background work.

### Bring-up traps to test explicitly

- Determine V1 SH8601/FT3168 versus V2 CO5300/CST820 before selecting drivers.
- Initialize the shared I²C bus and TCA9554-controlled panel power/reset before display/touch.
- Set AMOLED brightness with command `0x51`; do not assume a backlight GPIO.
- Enable the speaker amplifier and required AXP2101 rails.
- Use OPI PSRAM configuration and measure internal DMA-capable heap separately from PSRAM.
- Test the V2 display offset through the selected BSP rather than copying a hard-coded driver.
- Test concurrent I²S RX and TX. `chat-stick` switches one audio path between 16 kHz capture and 24 kHz playback, so it is a half-duplex reference and must not be copied for future barge-in/AEC.
- Treat AEC as unproven. Push-to-talk does not depend on it.

### Face model

Use one small parametric state:

```text
interaction: idle | listening | thinking | speaking | confirm | success | error | offline | sleeping
affect:      neutral | warm | delighted | doubtful | concerned | sleepy
left/right eye: openness, curve, gaze x/y
mouth: openness, width, curvature
body: breath phase, vertical offset, tilt
theme: glow colour, intensity
```

Render the base geometry and apply composable, bounded modifiers:

- saccades;
- asymmetric eased blinks;
- subtle breathing drift;
- listening focus;
- thinking glance;
- playback-RMS mouth motion;
- temporary emotion masks and decorators.

Use Claude's concrete StackChan values only as initial tuning seeds—saccades around 0.5–2.5 seconds, blink intervals around 2.5–4.5 seconds, and a 2–3 px breath—not requirements. Interaction truth overrides affect: the model cannot display success before a tool succeeds or hide confirmation/error states.

The visual direction is black AMOLED, sparse neon curves, cyan/teal as the base, warm amber accents, slight asymmetry, and no copied WALL-E/EVE assets. Add small gaze movement, pixel wandering, adaptive dimming, and screen sleep to reduce static OLED exposure.

## 4. Voice/session behavior

### First-turn path

1. User presses and holds the talk control.
2. Device immediately shows `listening`, starts capture, and sends `turn.start`.
3. Agent begins opening the OpenAI socket immediately while accepting audio into a bounded, drop-oldest pre-connect queue.
4. Device streams sequenced PCM frames; the Agent translates to provider events.
5. On release, device sends `turn.stop`; very short or near-silent clips are rejected without invoking a tool.
6. For an accepted turn, the OpenAI adapter sends `input_audio_buffer.commit` followed by `response.create`. Before a new turn it sends `input_audio_buffer.clear` as required by the VAD-disabled push-to-talk flow.
7. Agent streams `response.output_audio.delta` bytes as they arrive; device starts playback after a configurable jitter threshold.
8. Playback samples drive mouth animation locally.
9. A new press stops local playback immediately and reports how many milliseconds were actually played. The Agent cancels the response and truncates the conversation item to the heard audio—not merely the bytes received—then sends `drop_audio` so every device buffer is flushed.

Consolidate these useful `chat-stick` patterns:

- bounded audio queue during lazy upstream connection;
- 150 ms accidental-tap threshold with a pre-commit ring;
- server-side short/silent-turn rejection;
- `turn_complete` accepted only after output audio has arrived;
- explicit `drop_audio`/buffer flush on interruption;
- transcript-delta deduplication;
- graceful provider reconnect and session resumption where supported.

Do not blindly adopt its 750 ms playback prebuffer because that directly adds latency. Make the threshold configurable and test approximately 100, 250, and 750 ms plus a completed-short-reply escape hatch.

### Instrumentation

Correlate every turn and record monotonic timestamps for:

- press/release and last microphone sample;
- device queue/send;
- Agent receive and provider send;
- first provider audio delta;
- first device byte;
- first DAC sample;
- playback stop and cancellation.

Track p50/p95, reconnects, audio under/overruns, free internal heap, largest contiguous block, PSRAM high-water mark, frame time, queue depth, and tool duplicates. Optimize against first **audible** response, not model token timing.

## 5. Personality and authority

Keep three versioned domains:

1. **Character constitution:** name, identity, values, humour, cadence, curiosity, relationship stance, uncertainty behavior, and privacy boundaries.
2. **Embodiment rules:** local state-to-animation mapping and affect limits.
3. **Authority policy:** typed tools, authenticated scope, confirmation, limits, idempotency, and audit.

The character constitution may shape how the robot asks and replies; it cannot add tools, weaken confirmation, alter printer destinations, claim unverified success, or rewrite durable memory policy.

Memory is opt-in and inspectable:

- transient turn context;
- explicit preferences;
- rare user-approved durable memories with provenance;
- privacy-conscious operational audit.

## 6. Initial tools

### Shopping list

Initial interface:

- `shopping_list.read`
- `shopping_list.add_items`
- `shopping_list.undo`

All mutations use authenticated owner resolution and a unique idempotency key. The server returns canonical item IDs and the actual applied result. Ordinary, unambiguous additions can execute and then confirm; ambiguous list, item, or quantity requests require clarification.

### Printing

The target is confirmed as a **paper printer on the same home Wi-Fi**. The printer model/protocol and the always-on bridge host still need to be identified.

Initial paper-print scope:

- one allowlisted printer;
- plain text or a tightly controlled generated PDF;
- one page and one copy by default;
- no arbitrary URL, host, queue, driver, shell argument, or file path from the model.

Flow:

1. `print.prepare` validates and renders immutable content/options, then returns title, digest, page count, printer, expiry, and preview/summary.
2. The robot asks for explicit confirmation of the physical effect.
3. `print.commit` binds the confirmation to the prepared digest and creates a pending job.
4. The bridge claims a leased job, durably records the job/idempotency key locally, submits it to CUPS/IPP, records the local printer job ID, and acknowledges the cloud state.
5. Ambiguous crash windows become `needs_review`; they are not automatically resubmitted.

States: `prepared -> confirmed -> pending -> claimed -> submitted -> completed | failed | cancelled | needs_review`.

## 7. Security and privacy baseline

- Standard OpenAI keys remain in Worker secrets or AI Gateway BYOK—never firmware.
- Send a stable, privacy-preserving `OpenAI-Safety-Identifier` from the server for the installation/user.
- Each device gets a distinct revocable credential after local-presence enrollment.
- Send device credentials in an authorization header, not a query string.
- Store device/Wi-Fi credentials in encrypted NVS when hardening begins.
- Do not persist raw audio by default; redact audio/transcript/tool content from routine logs.
- Rate-limit sessions, audio volume, model spend, list mutations, print pages/copies, and retries.
- Treat model output and printable content as untrusted data.
- Delay production secure-boot/flash-encryption eFuses until signed OTA, rollback, recovery, and key custody have been rehearsed on a disposable unit.

## 8. Delivery phases and gates

Cross-cutting test strategy:

- ESP-IDF Unity tests for pure firmware logic plus on-device hardware-in-loop smoke/soak tests.
- Protocol golden fixtures shared between C/C++ and TypeScript so binary framing, sequencing, cancellation, and version errors cannot drift.
- Worker/Agent integration tests in the Workers runtime, including hibernation/reconstruction and fake OpenAI sockets.
- Bridge integration tests against a fake CUPS/IPP endpoint before any real paper is consumed.
- Every phase adds failure-injection tests, not only happy-path demos.

### Phase 0 — resolve facts and establish the toolchain

**Progress:** toolchain, repository, private verified flash backup, security inventory, and strong V2 identification are complete. Physical revision corroboration, shopping-list selection, printer details, and provisioning choice remain open.

- Corroborate the flash-derived V2/CO5300 identification from PCB markings or the purchase record.
- Record the paper printer's make/model, protocol, and always-on bridge host.
- Choose the shopping-list system of record.
- OpenAI API and Cloudflare accounts are confirmed; defer credentials and account access until Phase 3/4.
- Install `esptool` and a pinned ESP-IDF toolchain; no ESP-IDF/PlatformIO toolchain is currently detected on this laptop.
- Before the first write, record chip/flash/security information, read the full existing flash with `esptool read-flash 0 ALL`, hash it, and store the private backup plus restore notes outside source control. This preserves the factory firmware/partition data and may help identify the revision.
- Inspect the existing partition table and serial boot log without erasing anything. Do not use `erase-flash`, `--force`, or any eFuse-writing command.
- Choose a Wi-Fi provisioning path. Bench credentials may be written to local NVS but must never be committed or compiled into a distributable image.
- Create the repository skeleton and capture version decisions.

**Gate:** board revision, restorable flash backup, list target, printer path, account access, provisioning path, and privacy assumptions are explicit.

### Phase 1 — hardware characterization

**Progress:** board-check, I²C inventory, display/touch, speaker, microphone, 24 kHz duplex I²S, PSRAM, render telemetry, and a 30-second combined display/audio soak have passed. See [`bringup-log.md`](bringup-log.md). A 30-minute thermal/power soak plus owner visual confirmation remain.

- Build/flash the vendor board-check. Note that its current source prints generic display/touch capabilities, not the controller model, so revision identification also needs PCB/purchase evidence and BSP/I²C logs or a tiny read-only probe.
- Verify display, touch, brightness, PMIC/battery, microphone, speaker, buttons, IMU, flash, and PSRAM.
- Run separate capture/playback tests and then a concurrent I²S RX/TX test.
- Record heap, DMA memory, frame/flush time, under/overruns, power, and temperature during a 30-minute soak.
- Compare initialization behavior with the pinned XiaoZhi board files where vendor examples are unclear.

**Gate:** stable display/audio operation with known revision and documented memory/performance baseline.

### Phase 2 — offline face

**Progress:** the parametric neon face now uses the selected eyebrow-free thick-line design, circular open eyes, independently moving pupils, blink/gaze/breathing, and eleven selectable authored expressions alongside truthful interaction state. Push-to-talk uses bounded 220 ms pre-commit, six-second offline capture, and 500 ms playback rings; valid turns retain their first 180 ms, short taps are discarded before commit, and a new press interrupts playback locally. Runtime 10–100% output controls persist in NVS, and playback RMS drives the mouth. Display reliability combines completion-owned QSPI, a single authoritative cold-start CO5300 initialization, and an experimentally bounded submission rate. The former offline 220-row complete-frame buffer ran near 29 FPS, but Wi-Fi/TLS made that 140.8 KB contiguous internal allocation impossible. A 110-row network candidate with paired writes still blanked physically despite successful callbacks. Production now spaces every 110-row submission by at least 35 ms, yielding approximately 14.3 FPS / 28.6 transactions per second. It remained physically visible beyond 4,400 transfers with ~79 KB free internal RAM (~74 KB minimum), authenticated WSS, and passing replay/short-tap/interruption checks. Runtime dimming remains disabled. A hardware-grade microphone mute remains. Committed prebuffer/live PCM is now copied concurrently into a bounded non-blocking relay queue while local capture/replay stays authoritative.

- Implement the parametric state and local modifier system.
- Add all interaction states, touch/button controls, mute, brightness, burn-in mitigation, and local test-wave lip sync.
- Stress it with concurrent I²S and synthetic network work.

**Gate:** the face stays smooth and truthful under audio load and remains useful offline.

### Phase 3 — Cloudflare and device protocol

**Progress:** a pinned Cloudflare Agents SDK Worker is deployed at `walle-relay.matt-ce8.workers.dev`. It exposes only the custom authenticated `/v1/device` WebSocket route, derives the installation name server-side, disables Agent protocol frames, bounds PCM/control messages, and echoes binary PCM without turn accumulation. `DEVICE_TOKEN` is a Worker secret whose matching random value is retained outside the repository with owner-only permissions. Generated Wrangler binding types, strict TypeScript, unit tests, a dry-run bundle, remote health and unauthorized-socket checks, and a remote `hello → turn.start → PCM echo → turn.commit` sequence pass. Device firmware now scans/selects either of two local Wi-Fi networks, verifies the cross-signed Workers.dev certificate chain with the ESP-IDF bundle, authenticates, and passes `hello → ready → ping → pong` while preserving local audio and the paced display. A cold-boot-visible physical turn also delivered 287 bounded frames / 73,472 samples / 146,944 bytes to the Agent while local replay continued. Echoed frames are sequence/sample/hash checked but not yet played. That initial Worker-deployment failure is fixed with manager-owned client recreation, wire-visible session epochs, connect/ready/pong deadlines, capped jittered retry, and a bounded prototype session lifetime. Forced close, active deployment, and suppressed-pong recovery passed without reboot, and bounded PCM turns succeeded afterward. A physical access-point outage/rescan test and response-scoped cancellation/downlink playback remain before OpenAI upstream transport. A first downlink experiment was rejected: 256/480-sample messages exposed a sustained per-message throughput limit and a 960-sample codec-task cadence physically blanked the display after cold boot. Network-owned batching now retains the proven 256-sample codec cadence while emitting bounded 960-sample WebSocket frames. Exact short/long/post-reconnect turn accounting, forced recovery, a two-minute visible soak, and two true cold boots passed; the promoted app is `b926040d…`. This isolates the display regression away from network frame size and leaves bounded downlink playback as the next audio gate.

- Create the Worker and pinned Agents SDK installation Agent.
- Authenticate the custom device route before Agent lookup; derive the installation name server-side and disable identity/default Agent protocol messages on the device connection.
- Define versioned binary-audio and JSON-control protocol, authentication, reconnect, cancellation, and trace IDs.
- Implement an echo/test-audio provider before OpenAI.
- Add embedded SQLite schema/version bookkeeping, unique idempotency constraints, export/restore tooling for owner data, and redacted telemetry.
- Benchmark candidate Agent location hints before creating the long-lived production installation.

**Gate:** authenticated bidirectional streaming survives disconnect/reconnect without unbounded memory or duplicate control effects.

### Phase 4 — OpenAI voice, no mutating tools

- Add the provider adapter and `gpt-realtime-2.1` configuration using 24 kHz PCM16 and the exact VAD-disabled commit/create/clear event flow.
- Store and pin the versioned character prompt server-side; send direct session fields only for explicit runtime overrides. Select the voice before the first audio output because it cannot be changed afterward in the same session.
- Implement push-to-talk, pre-connect queueing, playback-position-aware cancellation/truncation, jitter-buffer experiments, the documented 60-minute session expiry/reconnect, and the first character constitution.
- Benchmark relay, optional AI Gateway, and only if needed direct WebRTC.
- A/B flagship versus mini after the path is stable.

**Gate:** measured p50/p95 meets an agreed target on the intended Wi-Fi; no sustained audio errors; mute/interruption is reliable; logs contain no credentials or audio payloads.

Provisional target: warm release-to-first-audible p50 below 1.0 s and p95 below 1.5 s. This is a target, not a promised capability.

### Phase 5 — shopping list

- Implement typed read/add/undo tools against the chosen system of record.
- Add clarification, visual/verbal result confirmation, idempotency, audit, retry, and cross-device isolation tests.

**Gate:** retries cannot duplicate mutations and every spoken success corresponds to a canonical applied result.

### Phase 6 — printer bridge

- Implement prepare/confirm/commit and the SQLite job state machine.
- Build a bridge simulator, then authenticated claim/ack and a durable local receipt ledger.
- Test dropped notifications, duplicate pulls, lease expiry, bridge outage, malformed/excessive jobs, cancellation races, and crash ambiguity.
- Enable one allowlisted real printer only after simulator tests pass.

**Gate:** no unconfirmed or unauthorized job prints; no ambiguous job is automatically resubmitted; every physical submission is traceable to a confirmed digest and local printer job ID.

### Phase 7 — hardening and ownership controls

- Signed OTA, rollback/recovery, device credential rotation/revocation, factory reset, data deletion, memory inspection/reset, rate limits, alerting, and long-duration soak tests.
- Rehearse secure boot, flash encryption, and NVS encryption before enabling irreversible settings.

**Gate:** documented recovery from bad OTA, lost credentials, Wi-Fi/cloud/provider outages, and factory reset.

### Phase 8 — optional hands-free mode

- Evaluate ESP-SR WakeNet, AFE, software AEC, full-duplex I²S, false wake rates, echo, barge-in, CPU/memory, and battery with the final enclosure.
- Retain physical mute, visible listening state, and push-to-talk fallback.

**Gate:** hands-free mode meets explicit acoustic/privacy/resource targets. Otherwise keep push-to-talk.

## 9. Planned repository shape

```text
firmware/
  main/
    board/          # revision/BSP adaptation only
    audio/          # I²S, rings, levels, diagnostics
    face/           # state, modifiers, LVGL renderer
    protocol/       # provider-neutral device protocol
    app/            # interaction state machine
  components/
  sdkconfig.defaults

server/
  src/
    agent/          # installation Agent and schemas
    voice/          # OpenAI adapter and session coordinator
    tools/          # typed list/print policy
    protocol/       # shared wire contract
    observability/
  wrangler.jsonc

bridge/
  src/              # print claim, durable receipt ledger, CUPS/IPP adapter

docs/
  research/
  final-plan.md
  architecture/
```

Keep board drivers, voice provider code, character content, and tool authority in separate modules so each can change without rewriting the others.

## 10. Immediate next move

Continue **Phase 2**: separate interaction state from affect, add listening/thinking/speaking/confirm/success/error/offline/sleeping visuals, define touch push-to-talk and mute behavior, and drive mouth openness from a local audio envelope. Keep all behavior offline first. In parallel, run a longer display/audio thermal soak. Begin Phase 3 only after the device state and bounded audio queues are stable; obtain Cloudflare/OpenAI access at that point.
