# Hardware bring-up log

## 2026-07-30 — pre-write inventory and backup

### Host and connection

- Host: Apple Silicon macOS 26.1
- Device port: `/dev/cu.usbmodem2101`
- USB interface: Espressif native USB Serial/JTAG
- Tool: `esptool` 5.3.1, run ephemerally with `uvx`

The full device identifier and raw logs are retained with the private backup, not in this repository.

### Read-only inventory

- Chip: ESP32-S3 QFN56 revision v0.2
- CPU/radio: dual-core 240 MHz, Wi-Fi, BLE 5
- Embedded PSRAM: 8 MB
- External flash: 16 MB, quad, 3.3 V
- Crystal: 40 MHz
- Secure Boot: disabled
- Flash Encryption: disabled
- SPI boot crypt count: zero

No flash erase/write or eFuse-writing command was run.

### Factory flash backup

- Read range: `0x00000000` through the auto-detected end of 16 MB flash
- Backup size: 16,777,216 bytes
- Successful read baud: 460,800
- SHA-256: `e80abf20ae8c95415129c4bac80fec12e3b656262d64303023b2d1d90949c0d1`
- Verification: `esptool verify-flash` passed against the attached device
- Storage: private, mode-restricted directory under `~/Library/Application Support/Walle/backups/`; intentionally outside source control

A first read at 921,600 baud stopped at approximately 5.8% with serial corruption. Its partial output was discarded. The complete 460,800-baud read was subsequently verified.

### Existing partition layout

| Label | Offset | Size | Purpose |
|---|---:|---:|---|
| `nvsfactory` | `0x009000` | `0x032000` | Factory NVS data |
| `nvs` | `0x03b000` | `0x0d2000` | Runtime NVS |
| `otadata` | `0x10d000` | `0x002000` | OTA selection data |
| `phy_init` | `0x10f000` | `0x001000` | PHY initialization |
| `factory` | `0x110000` | `0x580000` | Factory app |
| `ota_0` | `0x690000` | `0x300000` | OTA app slot |
| `assets` | `0x990000` | `0x300000` | Assets data |
| `storage` | `0xc90000` | `0x370000` | Storage data |

The OTA data sector appeared erased in the backup, so the factory app is the expected default boot selection.

### Existing firmware and board-revision evidence

The factory app identifies as:

- project `esp-brookesia`, app version `1`
- compiled 2026-05-27
- linked with the `esp_lcd_co5300` driver

The OTA slot identifies as:

- project `xiaozhi`, app version `2.2.6`
- compiled 2026-05-26
- Waveshare ESP32-S3 Touch AMOLED 1.8 board target
- linked with the `esp_lcd_co5300` driver

Both installed applications contain CO5300 support and no SH8601 identity was found in their application partitions. Combined with the current Waveshare revision map, this is strong evidence that the attached device is the **V2 CO5300/CST820-family revision**. Corroborate it from PCB or purchase markings before treating it as a manufacturing identity.

### Toolchain pin candidate

The pinned Waveshare source at commit `ba32b5cbca96f0e04b0736d04959b6e832268d3f` declares:

- ESP-IDF `>=5.5`
- Waveshare BSP `^2.0.3`
- CI builds against ESP-IDF 5.5.5 and 6.0.2

The initial implementation candidate is ESP-IDF **6.0.2** with Waveshare BSP **2.0.3**, subject to a clean local board-check build before the first project firmware write.

### Confirmed product facts

- Printer: paper printer on the same home Wi-Fi
- OpenAI API account/key: available later; not supplied yet
- Cloudflare account: available later; not supplied yet

Still needed before their respective phases:

- shopping-list system of record
- printer make/model and protocol
- always-on host for the local printer bridge
- physical/purchase confirmation of the V2 board revision

## 2026-07-30 — toolchain and first hardware checks

### Toolchain

Installed outside the repository:

- ESP-IDF 6.0.2 at `~/.espressif/frameworks/esp-idf-v6.0.2`
- ESP-IDF target tools for ESP32-S3
- CMake 4.4.1 and Ninja 1.13.2 through Homebrew

The repository firmware pins Waveshare BSP 2.0.3 and has a generated `dependencies.lock` for transitive component versions. The vendor-derived board-check built successfully under ESP-IDF 6.0.2.

### First project firmware write: board-check

After the verified private backup, the vendor-derived board-check was flashed at 460,800 baud. Flash verification passed and the application booted cleanly.

Observed at runtime:

- ESP-IDF 6.0.2 bootloader/application
- QIO flash at 80 MHz, 16 MB
- 8 MB OPI PSRAM at 80 MHz; PSRAM memory test passed
- 2 CPU cores, silicon v0.2
- display capability: 368 × 448
- touch, speaker, microphone, and SD capabilities present
- board I²C: SDA 15, SCL 14
- SD: CMD 1, CLK 2, D0 3
- initial free internal heap: 391,023 bytes
- initial free PSRAM: 8,386,156 bytes
- periodic heap values remained stable over the short observation window

### I²C scan

The pinned vendor I²C-tools example built, flashed, and scanned the onboard bus successfully at 400 kHz.

Detected addresses:

| Address | Expected device |
|---:|---|
| `0x15` | CST816S-compatible V2 touch controller |
| `0x18` | ES8311 audio codec control |
| `0x20` | TCA9554 I/O expander |
| `0x34` | AXP2101 PMIC |
| `0x51` | PCF85063 RTC |
| `0x6b` | QMI8658 IMU |
| `0x7e` | Additional board device/address; identify only if needed |

The `0x15` response independently confirms the V2 detection method used by the current BSP.

### BSP display/touch quick-start

The pinned vendor BSP quick-start built and flashed successfully. Serial logs confirmed:

- CO5300 driver 2.1.0 initialized over QSPI
- CST816S-compatible touch found at `0x15`
- touch IC ID `183`
- LVGL started without an initialization error
- missing SD card reported as `ESP_ERR_TIMEOUT`, which is non-fatal and expected without a card

The dashboard was subsequently replaced by the diagnostics and local face prototype described below. Final visual/aesthetic confirmation by the owner remains useful, but serial touch events and the display workload are now validated in firmware.

## 2026-07-30 — speaker and microphone validation

### Finite speaker test

A vendor-derived ES8311 test was reduced to a finite and safer profile:

- 24 kHz mono PCM16
- 440 Hz sine wave
- 20% output volume
- two seconds of playback, followed by automatic mute

The codec opened successfully, the write completed, and the output was muted at the end. The codec component logs an `i2s_channel_disable` error while opening channels that are not yet enabled; this is reproducible initialization noise from the component and did not prevent operation.

### Duplex audio diagnostic

A repeatable diagnostic now lives at [`diagnostics/audio_duplex`](../diagnostics/audio_duplex/). The final run simultaneously captured the microphone and played the finite tone after discarding startup frames.

Final measurements:

| Phase | Samples | RMS | Peak |
|---|---:|---:|---:|
| quiet baseline | 15,872 | 29.0 | 118 |
| 440 Hz tone | 46,848 | 524.4 | 1,694 |
| post-tone tail | 33,280 | 54.2 | 2,299 |

- read errors: 0
- write errors: 0
- result: `AUDIO_DUPLEX_RESULT=PASS`

The large and repeatable increase during the tone confirms that speaker output couples into the onboard microphone and that concurrent RX/TX works at the intended OpenAI Realtime audio format.

## 2026-07-30 — offline face prototype

The board is now running a local LVGL 9 face prototype at 45% display brightness. It provides:

- neon cyan eye outlines, rounded irises/pupils, expressive brows, and a curved mouth on black
- autonomous blinking and breathing
- independently moving iris gaze/saccades while the eye sockets stay anchored
- neutral, happy, doubtful, and sleepy expressions
- tap-to-cycle emotion interaction
- render/FPS and heap telemetry

The first implementation used LVGL layered line drawing over a full-screen RGB565 canvas and reached only about 12.8 FPS with 44 ms rendering. The renderer was replaced with a purpose-built RGB565 neon rasterizer, then the PSRAM canvas was bounded to the 320 × 220 region occupied by the face.

The first optimized sparse face reached 44.4 FPS. Adding independently moving round irises, dark pupils, catchlights, soft emotion-dependent eyebrows, and a more curved mouth produced the current detailed face.

Current short-run telemetry:

- frame rate: approximately 40.0 FPS
- average render time: approximately 15.6 ms
- maximum observed render time: approximately 15.8 ms
- free internal heap: 224,847 bytes
- free PSRAM: 7,551,392 bytes
- heap values stable across reports
- touch events successfully cycled all four emotions
- no watchdog, allocation, display, or reset faults

The cropped canvas reduces each QSPI update from approximately 330 KB to 141 KB while retaining the full 368 × 448 black screen background.

## 2026-07-30 — combined display/audio soak

A separate repeatable diagnostic at [`diagnostics/display_audio_soak`](../diagnostics/display_audio_soak/) ran the production face renderer while continuously reading and writing 24 kHz mono PCM16 for 30 seconds. TX contained silence and speaker output was explicitly muted.

Initial sparse-face baseline results:

- face rate during duplex audio: approximately 44.4 FPS
- microphone samples: 719,872
- microphone RMS / peak: 19.5 / 93
- read errors: 0
- write errors: 0
- maximum read call: 19,991 µs
- maximum write call: 20,038 µs
- RX elapsed: 29,990 ms
- TX elapsed: 29,941 ms
- minimum free internal heap: 209,023 bytes
- minimum free PSRAM: 7,472,936 bytes
- heap remained flat for the full run
- result: `DISPLAY_AUDIO_SOAK_RESULT=PASS`

The soak was repeated after adding the richer eyes and eyebrows, while touch-cycling all four emotions:

- face rate: approximately 40–41 FPS
- read errors: 0
- write errors: 0
- RX elapsed: 30,010 ms
- TX elapsed: 29,942 ms
- heap remained flat
- result: `DISPLAY_AUDIO_SOAK_RESULT=PASS`

The touch callback originally forced an additional immediate render; during repeated taps this produced one 53 ms microphone read call despite no data error. The redundant render was removed so touch changes are picked up by the regular animation timer instead. A focused touch-under-audio latency rerun remains appropriate when push-to-talk input is implemented.

The production detailed-face firmware was restored after the diagnostic and booted cleanly at approximately 40 FPS. Hardware characterization now supports proceeding with the offline interaction/state layer and push-to-talk audio architecture. A longer thermal/display soak and owner confirmation of the revised visual appearance remain useful before enclosure or manufacturing decisions.

## 2026-07-30 — offline push-to-talk echo prototype

The production firmware now separates interaction state from affect and includes a deep `offline_echo` module that owns codec initialization, I²S, commands, state snapshots, and a bounded recording buffer. The local interaction is:

1. press and hold the main face to record;
2. release to stop;
3. show a brief thinking state;
4. replay the phrase at 20% volume (audible but quiet in the first physical test);
5. animate mouth opening from playback RMS;
6. return to idle.

Safety/resource bounds:

- 24 kHz mono PCM16
- maximum recording: six seconds
- PSRAM recording buffer: 288,000 bytes
- clips shorter than 180 ms discarded
- output muted except during playback
- logical mute cancels capture/playback and prevents sample retention
- codec input is still clocked and discarded while logically muted; a future hardware-grade privacy mute must stop or power down the input path

Initial boot validation:

- codec and face initialized successfully
- idle audio state stable with zero read/write errors
- approximately 35.4 FPS with continuous microphone draining
- average render time approximately 16.5 ms
- free internal heap: 210,983 bytes
- free PSRAM: 7,177,620 bytes
- no resets, watchdogs, or heap drift during the first 50-second observation

Physical hold/release recording, audible replay, mouth-envelope response, and mute-zone behavior still require owner confirmation.

### Display blanking regression — transfer ownership fix

A physical observation found that the direct-raster face could appear after reset and then leave the AMOLED black even though serial telemetry continued reporting healthy rendering. The earlier high-FPS conclusion was based on CPU/render telemetry and short observations; it did not prove sustained physical panel output.

The regression was minimized with owner-observed probes:

- known-good vendor dashboard remained visible, ruling out panel/power hardware;
- current face without audio still blanked, ruling out codec initialization;
- the same face at 4 FPS remained visible;
- increasing the update rate reproduced the blanking;
- a separate LVGL cyan object confirmed display initialization itself was healthy.

An initial hypothesis blamed PSRAM cache coherency because adding `lv_draw_buf_flush_cache()` correlated with a visible run. Later source inspection disproved that explanation: the canvas uses LVGL's default software draw-buffer handlers, whose flush callback is null, so that call is a no-op. The software renderer CPU-reads the PSRAM canvas into a separate LVGL display buffer.

A later one-byte audio-volume change produced another black run while the exact preceding binary remained visible when restored. This timing sensitivity, together with 4 FPS stability, points instead toward display transfer scheduling/ownership. In Waveshare BSP 2.0.3, the QSPI CO5300 is registered through `lvgl_port_add_disp_rgb()` with a single partial draw buffer; Espressif's RGB path marks partial flushes ready immediately even though CO5300 QSPI color transfers are asynchronous. The CO5300 driver's own tests wait for `on_color_trans_done`. This is a strong code-level candidate, but not yet a confirmed fix: a first custom generic-QSPI LVGL port produced no physical pixels despite healthy render telemetry and was reverted.

This failure is a reminder that render/FPS telemetry is not a substitute for sustained physical display observation. A minimal transfer-completion diagnostic was therefore developed before changing production again; its results and the final fix are recorded below.

### Adaptive cadence and AMOLED protection

Following a current-source comparison with `m5stack/StackChan` and `steveruizok/chat-stick`, production rendering now treats display work as elastic:

- idle frame period: 25 ms;
- listening/speaking and short result states: 33 ms;
- thinking: 50 ms;
- offline: 100 ms;
- display-sleep interaction: 200 ms.

Actual measured idle throughput is approximately 28.7–28.8 FPS because the approximately 16.5 ms CPU raster pass is part of the LVGL scheduling interval. A finite serial check showed stable internal heap of 210,983 bytes and PSRAM of 7,177,476 bytes, with no reported boot, audio, watchdog, or allocation error.

Current AMOLED mitigation includes fixed 45% brightness and deterministic canvas drift bounded to ±3 pixels, changing once per minute. The earlier two-stage runtime dimming experiment is disabled pending a fresh test on the corrected display path. This does not stop codec input or place the ESP32 in light/deep sleep.

The dimming build's application binary had SHA-256 `428a2d430402a02bc680034f5487b333bba8b97d0d1d8715ee5c7e2302d25f19`.

#### Runtime-dimming experiment and recovery

The physical display first became invisible near the two-minute runtime brightness transition. Removing runtime brightness updates restored visibility past that boundary, so dimming remains disabled. A subsequent blank run caused by an unrelated one-byte audio-volume build showed that dimming was not the sole display problem.

The exact fixed-brightness, 20%-playback binary with SHA-256 `6865318cf1b480b49bd79a747c8336c8b2c708637a09ea992835dca5a6f1312c` repeatedly restored the face during diagnosis. Position drift, push-to-talk capture/replay, and playback-driven mouth motion were physically confirmed. The first replay was audible but quiet; a 30% build coincided with another black display run and was reverted without attributing causality to audio volume.

#### Confirmed QSPI ownership fix

The finite [`diagnostics/display_transfer`](../diagnostics/display_transfer/) application bypassed LVGL, registered `on_color_trans_done`, used one 20-row internal DMA buffer, and waited before every reuse. It submitted and completed all 226 transfers without timeout or error, then held a physically confirmed cyan-bordered pattern.

Production now owns the LVGL flush driver instead of using the BSP's RGB wrapper. It:

- software-renders the PSRAM canvas into one 20-row internal DMA buffer;
- byte-swaps and submits each QSPI region;
- marks the buffer available only from `on_color_trans_done`;
- records submissions, completions, submit failures, and premature-overlap attempts.

A two-minute interactive production soak remained physically visible while crossing position-drift boundaries and exercising multiple recordings, playback, short-tap rejection, and mute toggles. It exceeded 125,000 submitted transfers with zero overlap/submit errors, zero audio read/write errors, and stable internal/PSRAM heap. Owner confirmation covered visibility, replay, RMS mouth motion, and mute behavior. The validated binary SHA-256 is `28f0e7e7344fb4fffe808a3c383b5d9656eb92d10d5f687349431abf202ec437`.

#### Startup panel-state recovery and runtime volume

Adding even tiny face pixels or a separate LVGL volume overlay reproducibly brought back a physically black panel while exact recovery binaries remained visible. Moving NVS after display startup, removing the custom volume raster path, reducing it to one fixed pixel, and serializing the initial brightness command before LVGL creation narrowed the problem but did not independently resolve it. Touch-only changes remained visible, and the failed build still reported completed QSPI transfers, pointing to a bad CO5300 startup state rather than codec volume, draw-buffer reuse, or touch handling.

The successful startup sequence now:

1. creates the display with explicit transfer-completion ownership;
2. creates the face and bottom LVGL controls;
3. pauses LVGL and waits until no color transfer is in flight;
4. software-resets and reinitializes the CO5300;
5. turns the display on, applies fixed 45% brightness, invalidates the screen, and resumes LVGL.

This post-UI reinitialization restored the face and separate `− / VOL / +` overlay. Enlarging each button to 72×52 pixels, adding a 14-pixel extended hit area, and acting on `LV_EVENT_PRESSED` made volume adjustment usable. Codec changes are queued through the audio task, persisted in NVS, and selectable from 10–100% in 10% steps. The owner physically confirmed the face, controls, persistence path, and working playback up to 100%; current microphone replay remains relatively quiet even at maximum, so future cloud speech must be tested separately for level and distortion.

The exact initiating panel-state corruption is not yet isolated, so the post-UI reinitialization remains an explicit startup invariant rather than being described as a proven silicon/driver root cause. The current confirmed binary SHA-256 is `ed4b84582efb4d08f9dc90781ef23fc9287a74fb8c45e7f97e7332010c001c2d`.

#### Sustained-transfer rate limit and coalesced production frames

After a later power cycle, exact previously visible production recovery binaries began showing the face briefly and then turning physically black. The original 226-transfer direct diagnostic still held a visible final pattern, allowing a bounded hardware loop to isolate the discrepancy:

- 500 moving 20-row updates with a 5 ms inter-transfer gap remained visible;
- 1,000 updates at the same 5 ms gap turned the panel black;
- all 1,046 submitted transfers still reported completion with no timeout or submit error;
- 1,000 updates paced at 20 ms remained physically visible.

This proves an additional CO5300 **sustained transfer-rate** limit that software completion telemetry cannot detect. It also explains why exact binaries could differ across runs: the old 20-row production buffer generated bursts of eleven transactions per face frame and sat near a hardware/state-dependent boundary. The earlier long soak remains valid evidence for that run, but not a sufficient regression guard for panel visibility.

Production now allocates one 220-row internal DMA buffer, tall enough for the complete 320×220 face invalidation. Normal animation therefore submits one panel transfer per frame rather than eleven. A post-fix serial sample over two five-second windows measured 28.0–28.1 submitted transfers per second, approximately 29 FPS, zero overlap/submit errors, 112,967 bytes free internal RAM, and 7,290,388 bytes free PSRAM. The owner physically confirmed sustained visibility. The larger internal allocation is deliberate but leaves a tighter TLS/network budget that must be measured in Phase 3.

#### Authored expression deck and bounded audio rings

The installed face now matches the selected off-device study: no eyebrows, true circular open eyes, thicker cyan cores, larger proportions, and distinct `OPEN`, `PEEK`, `WHAT?!`, `DOZY`, `WORRY`, `HAPPY`, `CURIOUS`, `DEVIOUS`, `ANGRY`, `FURY`, and `SAD` states. A center LVGL button cycles the deck; audio interaction states override demo affect truthfully. The owner approved all expressions on-device.

The former linear recording array is now wrapped in explicit single-owner PCM ring seams:

- 220 ms pre-commit ring;
- six-second capture ring for offline echo compatibility;
- 500 ms playback staging ring.

The first 180 ms are buffered and transferred into capture on commit, so valid turns retain their beginning while accidental taps never replay. Playback consumes bounded chunks with RMS mouth drive, and a new PTT press cancels playback locally before beginning another recording. Ring depth, commit state, overrun, underrun, and I/O errors are exposed in snapshots. A host wrap/backpressure test passes, and the owner physically confirmed normal replay, short-tap rejection, and playback interruption. Total ring storage is 322,560 PSRAM bytes. The cloud sender will later drain committed capture concurrently instead of accumulating six-second turns.

The current physically confirmed production binary SHA-256 is `2ecc7023ed07742e2fd5fc759978cad28612d15d93f8cb6555970b1d5a56672f`.

## 2026-07-31 — authenticated Cloudflare relay

A pinned Cloudflare Agents SDK Worker was deployed to `walle-relay.matt-ce8.workers.dev` under the authenticated project account. Its custom `/v1/device` WebSocket route authenticates a bearer credential before Agent lookup, derives the `home-prototype` installation identity server-side, strips authorization before forwarding, and disables default Agent identity/state/MCP protocol messages. The random device credential is installed as a Worker secret and retained only in an owner-readable file outside the repository.

The initial provider deliberately echoes bounded 24 kHz mono PCM16 frames rather than contacting OpenAI. Local validation passed generated binding types, strict TypeScript, six protocol/authentication tests, and a Wrangler deployment dry run. Remote validation passed:

- public health response;
- unauthenticated WebSocket rejection with HTTP 401;
- authenticated WebSocket upgrade;
- installation Agent connection;
- `hello → ready → turn.start → binary PCM echo → turn.commit → turn.done`.

No Wi-Fi credential, device bearer token, or cloud API key is committed.

### Dual-network device connection and paced display

The local credential helper generated an owner-readable, Git-ignored header with two WPA-Personal network entries and the existing device token. Firmware scans visible access points, selects the strongest configured entry, reconnects by rescanning, verifies the Workers.dev certificate chain, and authenticates the WSS upgrade. The attached device selected configured network 2 at approximately −61 dBm and completed relay `hello → ready → ping → pong` with zero protocol errors. OpenAI and audio streaming are not yet enabled; the validated local echo behavior remains authoritative.

Adding Wi-Fi/TLS first prevented the former 140.8 KB, 220-row display DMA allocation because the linked network stack reduced contiguous internal RAM. A 110-row candidate restored allocation and reached the relay, but its two back-to-back transfers per face frame eventually turned the panel physically black after telemetry had crossed thousands of successful callbacks. An app-only recovery image also flashed briefly then disappeared, even after a full power cycle. The finite direct diagnostic—1,000 evenly paced 20 ms updates followed by a static pattern—remained visible and reconditioned the panel; the exact recovery app then remained visible again. Binary comparison showed the diagnostic and network bootloaders differed only in generated image metadata/hash, falsifying the initial bootloader-mismatch hypothesis.

Production retains the 110-row allocation but now enforces a minimum 35 ms interval before **every** panel submission. This removes paired bursts and limits the panel to about 28.6 transactions per second, matching the previously visible complete-frame transaction envelope while reducing the face to approximately 14.3 FPS. A 150-second serial/physical soak passed more than 4,400 submissions with zero submit/overlap/protocol errors, a continuously visible panel, stable ~79 KB free internal RAM (~74 KB minimum), authenticated WSS, and working replay, short-tap rejection, and interruption.

That warm-run candidate was then rejected: after a true 15-second power-off, the face appeared for approximately one second and disappeared. Removing only the second post-UI `display_port_reinitialize()` made the same network/pacing path remain visible after an equivalent cold boot. This confirms that the first `display_port_start()` initialization is valid and the later reset/init—not the bootloader—caused the cold-start blanking. The hazardous reinitialization API and call have been removed. The cold-boot-validated credential-bearing app SHA-256 is `9da91deb0a5bc6962468d533c162d8a96ca523e1637d2c69f9cf351191b257eb`; its bootloader, production partition table, app, hashes, and restore offsets are stored in an owner-only recovery directory outside the repository. The earlier warm-only set is retained only under `failed-candidates` with an explicit rejection marker.

### Committed microphone PCM uplink

The offline audio owner now exposes a non-blocking committed-stream observer. On the 180 ms recording commit it emits `START`, copies the retained prebuffer and subsequent live PCM in chunks of at most 256 samples, and emits `COMMIT` or `CANCEL`. The network relay uses a 32-item PSRAM-backed queue with two slots reserved for control, so network backpressure can only drop the cloud copy; the six-second capture ring and local replay remain authoritative. Binary frames carry the v1 header, per-turn sequence, sample count, and PCM16 payload. Returned echo frames are checked against bounded expected sequence/sample/hash records but are not yet played.

After a true 15-second power-off, the streaming candidate remained physically visible. A normal local recording then committed **287 frames / 73,472 samples / 146,944 bytes**, equal to **3.061 seconds at 24 kHz**, to the deployed installation Agent while local replay continued normally. The Agent retained only aggregate frame/sample counts and a timestamp; PCM was never stored. The exact validated app SHA-256 is `a5a41b0826a95572673b7fca691d655396aa1abe001f40a6e471513e222999af`, with its actual validated bootloader and partition table in an owner-only recovery directory outside the repository.

A Worker deployment terminated the existing device WebSocket, and the current client did not re-establish a live Agent connection without a cold reboot. This established a measured reconnect/liveness defect. Opening native USB serial also perturbed panel state for this run, so remote authenticated aggregate metrics were used for validation instead of claiming serial telemetry as harmless.

### Manager-owned reconnect and heartbeat recovery

A pinned `chat-stick` source review reinforced four changes: callbacks must not perform blocking reset work; old callbacks/audio need connection generations; socket-open and protocol-ready require separate deadlines; and local cancellation must remain authoritative. Firmware now gives each explicit client instance a positive session epoch, tags queued capture/expected echoes with it, and ignores stale callbacks/items. The WebSocket event task performs only bounded parsing/event publication and never waits on the stream mutex. The manager disables library auto-reconnect, owns stop/destroy/reset/recreate, applies connect/ready/pong deadlines, uses capped exponential retry with jitter, and refreshes the prototype session after two minutes so an old deployment cannot retain the device indefinitely. `hello`/`ready` exchange and verify the epoch.

Remote fault injection passed without native serial or reboot:

- forced clean socket close: epoch 1 → 2 and ready in approximately 3 seconds;
- a PCM turn after recovery: 212 frames / 54,272 samples / 108,544 bytes;
- active Worker deployments: migration to a newer ready epoch, observed in 2–7 seconds;
- suppressed application pong on an otherwise open socket: epoch 5 → 6 and ready in approximately 20 seconds;
- a PCM turn after watchdog recovery: 187 frames / 47,872 samples / 95,744 bytes (1.995 seconds).

The face remained visible and local replay passed. The exact cold-boot-visible reconnect candidate app SHA-256 is `5c19c3cd2ba826b739ae9d8df0b82aeaaeb22974e4ccbf28183b267f28864496`; its actual validated bootloader, partition table, app, and evidence are stored in an owner-only recovery directory outside the repository. A physical access-point outage/rescan test remains for the broader soak; socket/deployment/protocol liveness no longer requires a power cycle.

### Rejected remote-playback experiment

A bounded remote-echo playback experiment was attempted only after saving the reconnect recovery set. It added a device-side full-turn response ring, a 32-item inbound queue, source/fallback selection, and aggregate `playback.report` telemetry. The safety behavior worked: every incomplete response used the existing local capture, and no partial remote audio reached the speaker.

Measured results identified transport message rate—not PCM bandwidth—as the immediate bottleneck:

- At 256 samples per WebSocket message (~94 messages/s), the 32-item uplink queue reached 31 and rejected one frame per turn. Each turn was cancelled and replayed locally.
- Increasing only the queue delayed rather than removed pressure.
- At 480 samples (~50 messages/s), one 4.22-second turn committed 211 frames with zero uplink drops, 211/211 matching echoes, zero protocol/hash errors, and zero inbound-queue drops. However, a 75-frame backlog drained for roughly three seconds after release; only 188 frames had arrived by the 500 ms source-selection deadline, so the device correctly reported local fallback.
- Changing codec/audio-task cadence to 960 samples (40 ms) reduced message rate below measured transport capacity, but the face physically disappeared 15–20 seconds after cold boot. That candidate was rejected immediately rather than interpreting healthy transfer callbacks as display success.

The exact `5c19c3cd…` reconnect image was restored and physically confirmed visible again. Production source was then rolled back and rebuilt to the identical SHA-256. Remote playback will resume only in a separate diagnostic that keeps the proven 256-sample codec cadence and aggregates capture chunks into bounded 960-sample network frames inside the network owner. No rejected remote-playback firmware remains on the device or production source.

### Validated network-owned PCM batching

An allocation-free `pcm_batcher` module now sits between the existing 256-sample capture queue and WebSocket sender. It preserves codec/I²S timing, carries spill across chunk boundaries, emits full 960-sample frames, flushes the final partial frame before commit, and discards buffered state on cancellation or sender failure. Its PSRAM storage is the PCM portion of the eventual `WA` frame, avoiding both a second 1,920-byte copy and a larger manager-task stack frame. ASan/UBSan host tests cover ordering, arbitrary boundaries, partial completion, cancellation, and fail-closed emission failure.

A deterministic cadence model reproduced the unsustainable 256-sample strategy and predicted that 960-sample framing would preserve the 32-item queue through a six-second turn for synchronous send times up to 40 ms. A one-variable hardware candidate then changed only network batching—no downlink playback, codec cadence, display timing, or local replay behavior.

Physical and authenticated remote validation passed:

- initial 15-second power-off and 60-second continuously visible cold boot;
- 75,008 samples in exactly 79 frames;
- 121,344 samples (5.06 seconds) in exactly 127 frames;
- two further minutes of continuous visibility and 13/13 ready remote checks;
- normal planned session refresh;
- injected disconnect recovery in roughly three seconds, epoch 3 → 4;
- 59,904 post-reconnect samples in exactly 63 frames;
- final 15-second power-off and 60-second continuously visible cold boot.

Every frame count equals `ceil(samples / 960)`, and the Agent accepts a commit only after contiguous frame sequences. Local replay remained normal and authoritative. This isolates the rejected display shutdown away from the 960-sample network message size: changing codec/audio-task cadence or another removed downlink change was responsible.

The promoted source commit is `6956c2f`; the exact flashed app SHA-256 is `b926040df0468a289da20fd6576a2e9dd0fdb6c040919a3f36878391926cdb02`. Its bootloader, partition table, app, hashes, and evidence are stored owner-only at `~/Library/Application Support/Walle/recovery/walle-network-batching-coldboot-visible-b926040df0468a28/`.

### Validated complete-turn remote PCM playback

Two allocation-free modules now protect the downlink. `remote_event_queue` is a 32-event usable-capacity SPSC queue from the WebSocket event task to the audio task; callbacks copy validated frames directly into PSRAM slots and never wait, allocate, write the codec, or own teardown. `remote_response` buffers at most six seconds and makes PCM readable only after token-scoped buffered, relay-reported, and local-capture counts match. Stale tokens, overflow, mismatch, timeout, cancellation, reconnect, and supersession cannot expose partial remote audio. The audio task alone observes the 220 ms minimum thinking interval, applies the 500 ms response deadline, chooses remote versus untouched local capture, and owns codec writes.

Host evidence passed under ASan/UBSan: exact frame ordering/readout, three-way count validation, overflow, stale events, cancellation, timeout, source selection, queue wrap/full/reset, a 100,000-event concurrent SPSC stress, and integrated exact/missing-frame/queue-overflow pipelines. The firmware retained 256-sample codec reads, 960-sample network frames, 35 ms display submission pacing, and the validated DIRAM footprint.

Physical and authenticated echo validation passed:

- initial 15-second power-off and 60-second continuously visible cold boot;
- 59,392-sample exact remote playback over 62 frames;
- 126,208-sample (5.26-second) exact remote playback over 132 frames;
- forced reconnect epoch 1 → 2 in roughly three seconds;
- 53,760-sample exact post-reconnect remote playback over 56 frames;
- one intentionally omitted output frame produced exact 36,608-sample **local** fallback;
- an intentionally wrong completion count produced exact 52,736-sample **local** fallback;
- playback interruption immediately began a replacement capture, whose 36,864 samples then played remotely;
- two-minute continuous visibility, 13/13 ready checks, and planned epoch 4 → 5 refresh;
- final 15-second power-off and 60-second continuously visible cold boot;
- final fresh-boot 47,104-sample exact remote playback over 50 frames.

The face and audio remained normal. The promoted firmware source commit is `a9b37f8`; exact app SHA-256 is `6310d81102ef42c3b272dc1a83564292a6c225bfc736a3b7c906550c7dfab9c8`. Fault validation used deployed Worker version `e128a3b2-e7de-4cab-a409-6184bf5a0558`. The owner-only recovery set is `~/Library/Application Support/Walle/recovery/walle-remote-echo-coldboot-visible-6310d81102ef42c3/`.

### OpenAI Realtime candidate bring-up

The first `gpt-realtime-2.1` deployment failed closed to negotiated echo because the GA PCM session schema requires an explicit 24,000 Hz rate on both input and output formats. Bounded provider diagnostics exposed the exact missing output field without exposing the API key; adding it established the outbound session successfully.

The first speech turn generated 57,600 samples from 32,768 captured samples in 1,074 ms, but the device correctly used local fallback. Diagnosis showed that a single upstream audio delta had been synchronously expanded into roughly 60 device frames, exceeding the intentionally small 33-event device queue. The Agent now queues at most the already-bounded six seconds of output and pumps 960-sample frames without blocking the OpenAI callback. Physical trials reduced pacing from 40 ms to 20 ms and then 10 ms: the latter retained exact synthesized playback across repeated turns and felt materially faster, while the face remained visible.

Further turns exposed a telemetry-only duration assumption: successful generated playback compared played output samples against captured input samples, preventing `playback.report` even though complete generated audio reached the codec. Playback completion now uses the independently validated remote output count. Exact independent accounting subsequently passed with 57,344 input / 102,000 output samples and 31,488 input / 73,200 output samples at 20 ms, followed by 42,496 input / 75,600 output samples at 10 ms. The model's first audio arrived in 648 ms and generation completed in 1,106 ms for the latter turn.

Intermittent local echoes during longer manual testing correlated with epoch changes, not failed OpenAI generations: the prototype still forced a device WebSocket refresh every two minutes. The production candidate now refreshes proactively at 55 minutes, ahead of the documented 60-minute Realtime limit, while heartbeat and injected reconnect paths remain available. This candidate still requires fault fallback, reconnect, long visible soak, and cold boot before promotion.
