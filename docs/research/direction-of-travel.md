# Direction of travel: a small voice robot with a face

**Research date:** 2026-07-30

**Target hardware:** Waveshare ESP32-S3-Touch-AMOLED-1.8

**Status:** technical direction, not an implementation specification

## Executive recommendation

Build the first serious prototype as four deliberately separate pieces:

1. **ESP-IDF firmware using Waveshare's maintained board-support package and LVGL 9.** The device owns the face, buttons, touch, microphone/speaker streaming, buffering, and immediate interaction state. The face must remain alive even with no network.
2. **A raw-binary WebSocket from the device to a Cloudflare Worker, with one Durable Object per device/session.** The relay owns device authentication, the OpenAI credential, Realtime protocol translation, turn/session state, tool authorization, audit records, and reconnection. It streams rather than accumulating audio.
3. **OpenAI Realtime with `gpt-realtime-2.1` as the initial voice model.** Begin with push-to-talk. Measure the current `gpt-realtime-2.1-mini` as a latency/cost alternative if the project account can access it; do not silently substitute a text model into the speech path. OpenAI describes GPT-Realtime-2.1 as speech-to-speech with configurable reasoning and tool use, and describes 2.1 mini as its faster, lower-cost distilled variant. Its server-to-server guide uses a WebSocket endpoint with 2.1. ([model catalog](https://developers.openai.com/api/docs/models/all), [`gpt-realtime-2.1`](https://developers.openai.com/api/docs/models/gpt-realtime-2.1), [`gpt-realtime-2.1-mini`](https://developers.openai.com/api/docs/models/gpt-realtime-2.1-mini), [Realtime WebSocket guide](https://developers.openai.com/api/docs/guides/realtime-websocket))
4. **Small, typed server-side actions.** Put shopping-list state and audit records in D1. Treat printing as a two-stage `prepare` then `commit` operation, with an explicit confirmation before `commit`. Deliver committed jobs through a Cloudflare Queue to a small outbound-connected bridge beside the printer; the bridge, not the public Worker, talks to CUPS/IPP or a vendor API.

The personality should be an authored, versioned “character constitution,” but the apparent life of the robot should come primarily from deterministic local behaviour: breath, gaze, saccades, blinks, listening posture, and audio-envelope lip motion. StackChan demonstrates the value of independent face state and local motion generators, while the supplied `chat-stick` project demonstrates that this exact board can participate in a low-latency, device-to-Durable-Object voice loop. ([StackChan face state](https://github.com/stack-chan/stack-chan/blob/c25bba5273dbdb23cc0bcc29b7df9494365272c5/firmware/host/modules/ui/state/face-state.ts), [StackChan face behaviour](https://github.com/stack-chan/stack-chan/blob/c25bba5273dbdb23cc0bcc29b7df9494365272c5/firmware/host/modules/ui/components/face/behaviors/face.ts), [`chat-stick` architecture](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/README.md))

This recommendation is an **inference and design choice**, not a claim that the unbuilt system has a particular latency or reliability. The proof-of-concept gates below are intended to validate it on the actual board, network, enclosure, account, printer, and shopping-list target.

## How to read the claims

The report uses four labels:

- **Verified** — directly supported by a linked first-party source, inspected source code, schematic, or the read-only local USB observation.
- **Inference / recommendation** — engineering judgment derived from the verified facts. It still needs testing.
- **Unknown** — information not established by the permitted sources or the connected device inspection.
- **Risk** — a plausible failure, security, privacy, product, or integration hazard that should become a test or control.

Repository links are pinned to the inspected revisions:

- Waveshare examples: [`ba32b5cbca96f0e04b0736d04959b6e832268d3f`](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/tree/ba32b5cbca96f0e04b0736d04959b6e832268d3f)
- `chat-stick`: [`3321c9bfc9771ee8b3adc4815f6c72890d3db125`](https://github.com/steveruizok/chat-stick/tree/3321c9bfc9771ee8b3adc4815f6c72890d3db125)
- StackChan: [`c25bba5273dbdb23cc0bcc29b7df9494365272c5`](https://github.com/stack-chan/stack-chan/tree/c25bba5273dbdb23cc0bcc29b7df9494365272c5)

“Current” OpenAI and Cloudflare statements mean current at the research date above. Model names, availability, limits, pricing, and managed-service behaviour must be checked again when implementation starts.

## 1. What the board actually provides

### 1.1 Verified hardware

Waveshare specifies:

| Capability | Verified value | Consequence |
|---|---:|---|
| MCU | ESP32-S3R8, dual-core Xtensa LX7, up to 240 MHz | Capable embedded UI/audio/network controller, not a general-purpose Linux computer |
| On-chip memory | 512 KB SRAM, 384 KB ROM | Internal SRAM is scarce once networking, TLS, GUI, and audio coexist |
| External memory | 8 MB PSRAM and 16 MB flash | PSRAM can hold large GUI/audio buffers; flash has room for firmware/assets and an OTA partition plan |
| Radio | 2.4 GHz 802.11 b/g/n Wi-Fi and Bluetooth 5 LE | No 5 GHz Wi-Fi; real latency depends on 2.4 GHz congestion and signal quality |
| Display | 1.8-inch, 368 × 448, capacitive-touch AMOLED, 16.7 million colours, up to 350 nit | High-quality expressive face; small pixel count is tractable but static bright elements need care |
| Display/touch revisions | Original: SH8601 QSPI + FT3168 I²C; V2: CO5300 + CST820 | Firmware must identify/support the actual revision rather than hard-code the original panel |
| Audio | ES8311 audio codec, onboard microphone and speaker/power-amplifier path | The ingredients for voice I/O are present; acoustic performance and echo rejection remain unverified |
| Other devices | QMI8658 six-axis IMU, PCF85063 RTC, AXP2101 PMIC, microSD/TF slot | Motion-reactive face, timekeeping, battery telemetry/power control, and removable storage are possible |
| Physical I/O | Two buttons; battery connector; exposed GPIO, I²C, UART and USB pads | Push-to-talk and recovery can have dedicated physical controls |

Sources: [Waveshare product wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8), [Waveshare product page](https://www.waveshare.com/esp32-s3-touch-amoled-1.8.htm), [Waveshare schematic PDF](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf), and the [current Waveshare example repository README](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/ba32b5cbca96f0e04b0736d04959b6e832268d3f/README.md). The schematic identifies the ESP32-S3R8 and W25Q128 flash device. The repository README documents both panel/touch revisions and the other board peripherals.

At 368 × 448, the panel has 164,864 pixels. One RGB565 full-frame buffer is 329,728 bytes and two are 659,456 bytes. Those numbers are **calculated facts** from the verified resolution and two bytes per RGB565 pixel; they explain why external PSRAM and partial/DMA-capable transfer buffers matter. They do not prove a particular frame rate.

Waveshare's wiki says brightness is controlled with display command `0x51`, using values 0–255. It also gives approximate battery runtimes for a recommended 400 mAh cell—roughly one hour at full light, three to four hours with the screen off, and about six hours in low power—but these are vendor test figures rather than guarantees for this voice workload. ([Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8))

#### Pin assignment captured by the supplied board implementation

The inspected `chat-stick` Waveshare target records the following assignment. This is useful corroborating source for the original board and an exact starting point for scope/logic-analyser work; it is **not a substitute for asking the current Waveshare BSP for the attached revision's capabilities**.

| Function | ESP32-S3 pin(s) in inspected target |
|---|---|
| AMOLED QSPI | SDIO0 `GPIO4`, SDIO1 `GPIO5`, SDIO2 `GPIO6`, SDIO3 `GPIO7`, SCLK `GPIO11`, CS `GPIO12` |
| Board I²C | SDA `GPIO15`, SCL `GPIO14` |
| Boot/button A | `GPIO0`, active low |
| ES8311 I²S | MCLK `GPIO16`, BCLK `GPIO9`, codec-to-MCU DIN `GPIO10`, WS `GPIO45`, MCU-to-codec DOUT `GPIO8` |
| Speaker amplifier enable | `GPIO46` |
| Power button | Reported through the AXP2101 interrupt path/TCA9554 `EXIO5`, not a directly wake-capable ESP32 GPIO in this implementation |

Source: pinned [`chat-stick` Waveshare `Config.h`](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/devices/firmware/waveshare/src/Config.h). The vendor board-check obtains SD and other capabilities from BSP symbols instead of reproducing a second hard-coded map. ([Waveshare board-check source](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/ba32b5cbca96f0e04b0736d04959b6e832268d3f/examples/esp-idf/00_board_check/main/board_check_main.c))

### 1.2 Verified software support

Waveshare supports both Arduino and ESP-IDF. Its wiki states that the official Arduino examples use LVGL 8.4.0, while its ESP-IDF examples include LVGL with double buffering/DMA and describe that route as smoother and less prone to tearing than the Arduino examples. ([Waveshare wiki, examples and FAQ](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8))

The inspected ESP-IDF examples declare the managed component `waveshare/esp32_s3_touch_amoled_1_8` at `^2.0.3` and require ESP-IDF 5.5 or later. Waveshare's CI currently builds its examples against ESP-IDF 5.5.5 and 6.0.2 and its Arduino examples against Arduino-ESP32 3.3.11. These are build checks, not evidence of hardware validation for every example. ([component manifest](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/ba32b5cbca96f0e04b0736d04959b6e832268d3f/examples/esp-idf/00_board_check/main/idf_component.yml), [CI workflow](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/ba32b5cbca96f0e04b0736d04959b6e832268d3f/.github/workflows/examples.yml), [Espressif component registry](https://components.espressif.com/components/waveshare/esp32_s3_touch_amoled_1_8))

The vendor's board-check example queries and reports chip, flash, PSRAM, display, touch, audio, SD-card, capability, and pin information. The audio example configures the ES8311 path for 16-bit mono I²S and intentionally does not demonstrate microphone echo mode. The LVGL 9 example says PSRAM and a larger application partition are required. ([board-check source](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/ba32b5cbca96f0e04b0736d04959b6e832268d3f/examples/esp-idf/00_board_check/main/board_check_main.c), [ES8311/I²S example](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/ba32b5cbca96f0e04b0736d04959b6e832268d3f/examples/esp-idf/12_i2s_codec/main/i2s_es8311_example.c), [LVGL 9 example](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/tree/ba32b5cbca96f0e04b0736d04959b6e832268d3f/examples/esp-idf/14_lvgl_demo_v9))

Espressif documents two I²S peripherals on ESP32-S3, with independently allocated receive/transmit channels, DMA-backed transfers, blocking reads/writes, and asynchronous callbacks. Its ESP-SR Audio Front-End provides configurable acoustic echo cancellation, noise suppression, voice-activity detection, automatic gain control, and WakeNet integration; AEC needs a playback reference signal. These are available platform capabilities, not evidence that they will all fit or work well simultaneously in this product. ([ESP-IDF I²S documentation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/i2s.html), [ESP-SR Audio Front-End](https://docs.espressif.com/projects/esp-sr/en/latest/esp32/audio_front_end/README.html), [ESP-SR ESP32-S3 guide](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/getting_started/readme.html), [AEC documentation](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/acoustic_echo_cancellation/README.html), [WakeNet documentation](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html))

### 1.3 Read-only connected-device observation

On 2026-07-30, macOS enumerated `/dev/cu.usbmodem2101` and `/dev/tty.usbmodem2101` as:

- USB product: `USB JTAG/serial debug unit`
- USB vendor: `Espressif`
- VID:PID: `303a:1001`
- USB serial value: `1C:DB:D4:7B:76:FC`

This is a **local observation**, not a web-sourced claim. It is consistent with an ESP-family native USB Serial/JTAG interface, but it does **not** identify the Waveshare PCB/display revision or prove that the attached device is healthy. The serial port was not opened because opening it can change DTR/RTS and reset or enter the bootloader on some ESP development setups. Nothing was flashed, written, or altered.

### 1.4 Board unknowns to resolve first

- **Unknown:** original SH8601/FT3168 board or V2 CO5300/CST820 board. Establish this from the PCB marking, purchase record, and then the vendor board-check/BSP probe in the implementation phase.
- **Unknown:** actual microphone noise floor, loudspeaker level, enclosure resonance, acoustic echo path, Wi-Fi RSSI, battery capacity/quality, thermal behaviour, and charger behaviour.
- **Unknown:** how much internal DMA-capable memory remains under the chosen LVGL buffer, TLS, WebSocket, audio, ESP-SR, and logging configuration.
- **Risk:** the AMOLED face will contain persistent shapes. OLED image retention/burn-in is an engineering risk; use a mostly dark design, low adaptive brightness, pixel wandering, gaze motion, automatic dimming, and screen-off sleep. Validate the mitigation with the actual panel.
- **Risk:** Wi-Fi, flash/PSRAM access, QSPI display transfers, I²S DMA, and GUI rendering may contend in ways that are invisible in desktop reasoning. Only device telemetry and long-running tests can establish underrun-free operation.

## 2. Lessons from the two supplied projects

### 2.1 `chat-stick`: use the architecture patterns, not the firmware wholesale

**Verified:** `chat-stick` explicitly supports the Waveshare ESP32-S3-Touch-AMOLED-1.8. Its device sends audio over WebSocket to a Cloudflare Worker/Durable Object, which maintains an outbound live-model WebSocket; it captures 16 kHz PCM and plays 24 kHz PCM. It also contains D1-backed persistence, tool handling, optional R2 OTA, device metrics, and NVS settings. ([README](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/README.md))

The Waveshare configuration uses a 368 × 448 screen, 16 kHz microphone input, 24 kHz output, and 100 ms capture chunks. It records the board's LCD, I²C, I²S, amplifier, and button pins. ([Waveshare `Config.h`](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/devices/firmware/waveshare/src/Config.h))

The audio service allocates large playback storage in PSRAM, falls back to a smaller allocation, and uses a background playback task/ring-buffer pattern. ([`AudioService.h`](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/devices/firmware/waveshare/src/services/AudioService.h), [`AudioService.cpp`](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/devices/firmware/waveshare/src/services/AudioService.cpp))

Its Worker checks `DEVICE_AUTH_TOKEN` only when that secret is configured, accepts the credential in a header or query parameter, and routes by caller-supplied `device_id` to a Durable Object. ([Worker entry point](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/server/src/index.ts)) The Durable Object accepts the device WebSocket, opens an outbound provider WebSocket, wraps incoming raw binary audio in the provider's base64 JSON events, decodes provider output back to device binary, and records messages/tools in D1. ([live-session Durable Object](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/server/src/live-session.ts))

**Recommendations derived from it:**

- Retain the binary device transport, separate capture/playback tasks, PSRAM buffering, per-device session coordinator, timing metrics, and server-side tool dispatch.
- Do not retain the optional fleet-wide token model. Give each device a distinct identity and revocable credential.
- Do not put tokens in query strings; URLs are more likely than authorization headers to enter logs, histories, and diagnostics.
- Do not hard-code `Arduino_SH8601`: the current Waveshare BSP supports both documented revisions, while `chat-stick`'s inspected `Board.cpp` constructs the original SH8601 driver. ([Waveshare board implementation](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/devices/firmware/waveshare/src/hal/Board.cpp))
- Do not ship Wi-Fi and API credentials compiled from `credentials.h`; `chat-stick` itself warns that credentials and built firmware contain them in plaintext. ([README security notes](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/README.md), [example credentials](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/devices/firmware/waveshare/src/credentials.h.example))
- Treat its 100 ms chunks as one benchmark point, not a chosen optimum. A sample can wait almost a full chunk before it is sent.

### 2.2 StackChan: the face should be a local dynamical system

**Verified:** StackChan represents face state independently: mouth openness; openness and X/Y gaze for each eye; breath; emotion; and theme. Its emotion vocabulary includes neutral, angry, sad, happy, sleepy, doubtful, cold, and hot. ([face state](https://github.com/stack-chan/stack-chan/blob/c25bba5273dbdb23cc0bcc29b7df9494365272c5/firmware/host/modules/ui/state/face-state.ts))

Its face behaviour updates on a default 33 ms interval, maintains desired and current values, generates independent blink/breath/saccade motions, and avoids a redraw when the state has not changed. ([face behaviour](https://github.com/stack-chan/stack-chan/blob/c25bba5273dbdb23cc0bcc29b7df9494365272c5/firmware/host/modules/ui/components/face/behaviors/face.ts), [blink](https://github.com/stack-chan/stack-chan/blob/c25bba5273dbdb23cc0bcc29b7df9494365272c5/firmware/host/modules/ui/components/face/motions/blink.ts), [saccade](https://github.com/stack-chan/stack-chan/blob/c25bba5273dbdb23cc0bcc29b7df9494365272c5/firmware/host/modules/ui/components/face/motions/saccade.ts), [breath](https://github.com/stack-chan/stack-chan/blob/c25bba5273dbdb23cc0bcc29b7df9494365272c5/firmware/host/modules/ui/components/face/motions/breath.ts)) Its lip-sync example maps audio power to mouth openness. ([lip-sync example](https://github.com/stack-chan/stack-chan/blob/c25bba5273dbdb23cc0bcc29b7df9494365272c5/firmware/mods/examples/lip_sync/mod.js))

**Recommendation:** reproduce this *separation of concerns* in C/C++ and LVGL; do not attempt to port StackChan's Moddable/Piu UI runtime. Firmware should animate continuously from bounded state. The cloud/model may request a high-level affect such as `happy` or `doubtful`, but firmware owns timing, interpolation, gaze limits, blink cadence, and safe rendering. Mouth motion should be driven from the audio samples actually entering the playback path, so it stays synchronized through network jitter.

## 3. Firmware and face direction

### 3.1 Framework choice

**Recommendation:** use a pinned ESP-IDF release supported by the selected Waveshare BSP, the managed Waveshare board component, LVGL 9, `esp_lcd`/the BSP's LVGL port, the ESP-IDF I²S APIs, and the maintained ESP WebSocket client component. Start from the vendor board-check, I²S, and LVGL examples rather than an empty project. ([Waveshare component manifest](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/ba32b5cbca96f0e04b0736d04959b6e832268d3f/examples/esp-idf/00_board_check/main/idf_component.yml), [ESP-IDF programming guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/), [Espressif WebSocket client component](https://components.espressif.com/components/espressif/esp_websocket_client))

The reason is control, not fashion: the vendor's IDF path is the one that exposes its maintained revision-aware BSP, DMA/display examples, current LVGL example, native I²S, partitions, secure boot, flash/NVS encryption, and OTA controls. Arduino remains useful for comparison with `chat-stick`, but is not the recommended product foundation.

Pin exact versions and update deliberately. Re-run the vendor board check and a hardware soak test after every BSP, IDF, LVGL, or codec change. Waveshare's CI proves compilation of examples, not operation on the attached revision. ([Waveshare CI](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/ba32b5cbca96f0e04b0736d04959b6e832268d3f/.github/workflows/examples.yml))

### 3.2 Runtime shape

Use bounded queues/ring buffers and explicit ownership:

```text
I²S RX/DMA -> capture ring -> WebSocket sender -> WSS
WSS -> audio receive ring -> I²S TX/DMA -> speaker
                         \-> envelope follower -> mouth target

touch/buttons -> interaction state -> tool/session messages
network/model affect ------^

local blink + gaze + breath + state + mouth target -> LVGL face
```

Recommended responsibilities:

- **Audio capture/playback:** highest scheduling priority among application tasks; short DMA buffers in memory acceptable to the peripheral, larger queues in PSRAM only where supported; monotonically count overruns/underruns.
- **Network:** one owner for the live WebSocket and reconnect state; no audio JSON construction on the I²S callback.
- **Face/UI:** approximately 30 updates per second initially; render only changed regions/state; it must never wait on network or model work.
- **Control:** buttons/touch, push-to-talk state, tool confirmations, volume/brightness, and error recovery.
- **Persistence:** device identity, non-secret preferences, encrypted Wi-Fi credentials, and OTA metadata; never write on an audio callback.

These are **recommendations**. Stack sizes, priorities, core affinity, buffer placement, LVGL draw-buffer size, and clock rates must be selected from measurements of free internal heap, largest contiguous block, PSRAM use, DMA failures, frame time, and audio under/overruns.

### 3.3 The “soul” without surrendering safety

Keep three separate layers:

1. **Character constitution (versioned content):** identity, values, humour, speaking rhythm, favourite metaphors, degree of curiosity, relationship stance, forbidden pretences, and how the robot admits uncertainty. It can evoke WALL-E/EVE through warmth, economy of language, sound and motion, without copying protected character art, names, dialogue, or distinctive assets.
2. **Local embodiment (firmware):** `idle`, `listening`, `thinking`, `speaking`, `confirmation_required`, `success`, `error`, and `sleeping`, each with bounded gaze, blink, breath, colour, and motion parameters. This is the persistent “aliveness.”
3. **Authority policy (server code):** what tools exist, who may invoke them, confirmation rules, limits, and audit. Personality text cannot redefine this layer.

Recommended memory classes:

- **Turn context:** transient Realtime conversation state.
- **User preferences:** explicit, inspectable settings such as default list, voice volume, and preferred printer.
- **Durable memories:** rare, user-approved facts with provenance, edit/delete controls, and retention rules.
- **Operational audit:** action, actor/device, typed arguments or a privacy-safe digest, confirmation, result, and idempotency key.

Do not claim sentience. “Soul” here is an explicit, authored continuity of voice, values, motion, and memory policy. Give the owner a readable version, a change log, and controls to inspect/reset memories.

## 4. Voice path and latency alternatives

### 4.1 What OpenAI's current Realtime interface implies

OpenAI documents WebSockets as the server-to-server Realtime transport and WebRTC as the preferred browser/mobile transport. Standard API keys belong on a secure backend, not a client. The documented WebSocket endpoint is `wss://api.openai.com/v1/realtime?model=gpt-realtime-2.1`. ([Realtime WebSocket guide](https://developers.openai.com/api/docs/guides/realtime-websocket), [Realtime WebRTC guide](https://developers.openai.com/api/docs/guides/realtime-webrtc))

The current conversation guide shows 24 kHz PCM audio configuration, base64 audio chunks inside JSON events, streamed `response.output_audio.delta` output, semantic VAD options, and push-to-talk by disabling automatic turn detection. It says a Realtime session can last up to 60 minutes, a voice cannot be changed after the model has emitted audio in that session, and WebSocket clients must implement playback truncation when the user interrupts; WebRTC/SIP clients receive more automatic interruption handling. ([Realtime conversations guide](https://developers.openai.com/api/docs/guides/realtime-conversations))

**Important unknown:** the guide's current examples establish 24 kHz PCM, but this research did not establish from a first-party schema that arbitrary 16 kHz PCM can be sent directly to the selected model. Either capture at the session's required 24 kHz format or resample explicitly after validating the current API schema. Do not assume `chat-stick`'s 16 kHz input configuration transfers unchanged to OpenAI Realtime.

For 16-bit mono PCM, 24,000 samples/s is 48,000 bytes/s (384 kbit/s) in each active direction. Base64 expansion makes that about 64,000 bytes/s (512 kbit/s), before JSON/WebSocket/TLS overhead. At 16 kHz, raw input is 32,000 bytes/s (256 kbit/s). These are arithmetic results, not measured network loads. A relay permits raw binary between device and edge, keeping the base64/provider event format off the ESP32.

### 4.2 Alternatives

| Alternative | Advantages | Costs/risks | Direction |
|---|---|---|---|
| **A. ESP32 → OpenAI WebSocket** using a short-lived client credential | Fewest application hops; useful as a controlled latency experiment | Device still implements provider JSON/base64 and token renewal; direct tool policy/audit is harder; never embed the standard key | Benchmark-only spike, not the product architecture |
| **B. ESP32 raw binary → Worker/DO → OpenAI WebSocket** | Standard key stays at edge; small device protocol; central auth, policy, tool confirmation, metrics and audit; one coordinator per device | Extra hop and audio translation; active outbound WebSocket has cost; relay is realtime critical | **Recommended first product architecture** |
| **C. ESP32 → Cloudflare AI Gateway Realtime → OpenAI** | Gateway supports OpenAI Realtime WebSockets, BYOK, rate limits and observability | Device remains coupled to provider events and holds a Cloudflare credential; tool authorization still needs an application backend; another managed layer | Benchmark as an optional path, not the default device boundary |
| **D. ESP32 → relay → STT → Responses/tool loop → TTS** | Easier transcript inspection, structured non-voice processing, modular provider choices | More turns and buffering; weaker native prosody and barge-in; likely higher conversational latency | Use for offline/complex workflows, not the hot social loop |
| **E. WebRTC implemented on ESP32** | OpenAI's preferred realtime media transport for browser/mobile; built-in media semantics | ICE/DTLS/SRTP/codec and memory complexity on this MCU; official recommendation is not specific to ESP32 | Defer unless WebSocket measurements fail |

Cloudflare documents persistent WebSocket connections for Workers and for Agents, and AI Gateway documents an OpenAI Realtime WebSocket integration. ([Workers WebSockets](https://developers.cloudflare.com/workers/runtime-apis/websockets/), [Agents WebSockets](https://developers.cloudflare.com/agents/runtime/communication/websockets/), [AI Gateway Realtime API](https://developers.cloudflare.com/ai-gateway/usage/websockets-api/realtime-api/), [AI Gateway WebSockets](https://developers.cloudflare.com/ai-gateway/usage/websockets-api/))

### 4.3 Recommended live-session architecture

```text
button/touch
     |
ESP32-S3 face + PCM I/O
     |  WSS: small control JSON + raw PCM binary
     v
Cloudflare Worker ingress
     |  authenticate device; rate limit; choose session object
     v
Durable Object (one active coordinator per device)
     |  translate raw PCM <-> OpenAI base64 events
     |  enforce turn/tool/confirmation policy
     +--------------------------> OpenAI Realtime
     |
     +--> D1: list state, preferences, audit/job state
     |
     +--> Queue: committed print job
                   |
                   v
           LAN print bridge -> allowlisted CUPS/IPP printer
```

Use a plain Durable Object initially. Cloudflare Agents are built on Durable Objects and add persistent state, WebSocket hooks, scheduling and an agent-oriented API; those are useful if the product later needs durable autonomous workflows, but an Agents SDK abstraction is not required merely to relay audio. ([Agents API](https://developers.cloudflare.com/agents/runtime/agents-api/), [Agents WebSockets](https://developers.cloudflare.com/agents/runtime/communication/websockets/))

Cloudflare recommends the Durable Objects WebSocket Hibernation API for idle inbound connections, but an outbound WebSocket prevents the object from hibernating while it is active. A Realtime session has an outbound OpenAI connection, so hibernation does not remove the live-session cost; it can still help a parked device socket after the provider connection closes. ([Durable Objects WebSocket guidance](https://developers.cloudflare.com/durable-objects/best-practices/websockets/))

Workers requests have no fixed wall-clock duration while the client remains connected, subject to platform limits and disconnects. Stream both ways and impose explicit application session/reconnect limits rather than accumulating a recording in Worker memory. ([Workers limits](https://developers.cloudflare.com/workers/platform/limits/))

### 4.4 Latency is a measurement programme

Record monotonic timestamps and correlation IDs for:

1. push-to-talk release or last microphone sample;
2. last device audio chunk queued/sent;
3. relay receive and provider send;
4. first OpenAI audio delta received;
5. first output byte received by the device;
6. first corresponding DAC sample;
7. playback end and any interruption/truncation.

Measure p50/p95 and failures separately for cold boot, warm connected session, session reconnect, weak Wi-Fi, tool and non-tool turns, and simultaneous face rendering. Count audio under/overruns, reconnects, free heap, largest internal block, PSRAM high-water mark, frame time, and queue depth.

Test 20, 40, 60, and 100 ms capture chunks. Smaller chunks reduce waiting at the chunk boundary but increase packet/event/CPU overhead. Cloudflare's Durable Objects guidance suggests batching WebSocket messages over roughly 50–100 ms when appropriate to reduce invocation overhead; that is a cost hint, not a voice-quality result. ([Durable Objects WebSocket guidance](https://developers.cloudflare.com/durable-objects/best-practices/websockets/))

Provisional **acceptance targets, not verified capabilities**:

- warm push-to-talk release to first audible output: p50 under 1.0 s and p95 under 1.5 s on the intended home network;
- zero sustained I²S underruns/overruns during a 30-minute session;
- face updates remain perceptually smooth while streaming;
- an interruption stops audible playback promptly and sends the correct truncation/cancel event;
- no duplicate tool side effect after disconnect/retry.

Keep direct OpenAI and AI Gateway benchmark paths behind the same trace format. Select the production route from end-to-end audible latency, reliability, privacy, operational visibility, and security—not provider time-to-first-token alone.

## 5. Cloudflare component choices

| Component | Use now? | Grounded role |
|---|---|---|
| Worker | Yes | TLS/WebSocket ingress, device authentication, routing, coarse rate limits |
| Durable Object | Yes | Exactly one live coordinator per device/session; owns device and provider sockets, turn state, tool confirmation state |
| D1 | Yes, when first action lands | Shopping-list rows, device/user preferences, prepared print jobs, idempotency and privacy-conscious audit |
| Queue | Yes, for real printing | Decouple a committed cloud job from the LAN bridge; retries require consumer-side deduplication |
| AI Gateway | Optional experiment | Realtime routing, BYOK, rate controls and metadata; disable payload logging for private voice |
| Agents SDK | Later, if justified | Durable scheduled/background agent workflows and richer state/RPC, not needed for the audio relay itself |
| R2 | Later | Private print artifacts or signed OTA objects if those use cases require blobs |
| Cloudflare Tunnel | Optional | Managed private-network connectivity is possible, but an outbound polling/Queue bridge is simpler and exposes less LAN surface initially |

Cloudflare Queues provide at-least-once delivery; duplicate messages are possible. Cloudflare's HTTP pull consumer lets infrastructure outside Workers explicitly pull and acknowledge batches, which fits an outbound-connected LAN bridge. The bridge must atomically record/detect an idempotency key before submitting a job and acknowledge only after durable job-state recording. ([Queues delivery guarantees](https://developers.cloudflare.com/queues/reference/delivery-guarantees/), [HTTP pull consumers](https://developers.cloudflare.com/queues/configuration/pull-consumers/), [Queues overview](https://developers.cloudflare.com/queues/))

Cloudflare Tunnel can connect private networks, but the initial design should not assume that a public Worker can directly reach a home printer. An outbound-connected bridge can pull/consume a committed job and reach only allowlisted local printers. ([Cloudflare Tunnel private networks](https://developers.cloudflare.com/cloudflare-one/networks/connectors/cloudflare-tunnel/private-net/))

AI Gateway logs request and response payloads by default when logging is enabled. For household speech/transcripts, set `cf-aig-collect-log-payload: false` to retain metadata without content, or disable request logging entirely with `cf-aig-collect-log: false`, and verify the effective setting. Rate limiting is useful; caching is documented for matching text/image requests and is not a Realtime voice optimization. ([AI Gateway logging](https://developers.cloudflare.com/ai-gateway/observability/logging/), [rate limiting](https://developers.cloudflare.com/ai-gateway/features/rate-limiting/), [caching](https://developers.cloudflare.com/ai-gateway/features/caching/))

## 6. Authentication, security, and privacy

### 6.1 Trust boundaries

The device is not a trusted container for a fleet-wide OpenAI key. OpenAI says standard API keys must remain on a secure backend; its WebRTC client flow uses a backend to mint ephemeral credentials. Keep the standard OpenAI secret in a Worker secret or AI Gateway BYOK configuration. ([OpenAI WebSocket guide](https://developers.openai.com/api/docs/guides/realtime-websocket), [OpenAI WebRTC guide](https://developers.openai.com/api/docs/guides/realtime-webrtc), [Cloudflare secrets](https://developers.cloudflare.com/workers/configuration/secrets/), [AI Gateway Realtime API](https://developers.cloudflare.com/ai-gateway/usage/websockets-api/realtime-api/))

Recommended device enrollment:

1. A physical/local setup action starts a short-lived provisioning mode.
2. The device authenticates a one-time bootstrap code with the backend over certificate-validated TLS.
3. The backend issues a unique, scoped, revocable device credential and binds the server-side device ID; the caller cannot select another object's ID.
4. Store Wi-Fi credentials and the device credential with NVS encryption; support rotation/revocation and safe factory reset.
5. Send the credential in an authorization/device header, never a query string. Add expiry, nonce/challenge or signed request freshness where replay matters.

A unique rotating bearer token is adequate for a prototype if its storage and provisioning are controlled. Evaluate per-device asymmetric keys or TLS client certificates for productization; do not add them without measuring provisioning, rotation, recovery, and ESP32 resource cost.

### 6.2 ESP32 hardening sequence

Espressif recommends TLS, secure boot, flash encryption, and encrypted storage as parts of ESP32-S3 security. Secure Boot v2 verifies signed bootloader/application/OTA images; Espressif recommends combining flash encryption with secure boot. NVS encryption is recommended for sensitive data such as Wi-Fi credentials. ([ESP32-S3 security overview](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/security.html), [Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/secure-boot-v2.html), [flash encryption](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/flash-encryption.html))

Do not burn production eFuses during the early proof of concept. Secure-boot and flash-encryption production modes alter reflashing/debug/recovery behaviour and can be irreversible. First establish signed OTA, rollback, key custody, factory reset, failure recovery, serial-number mapping, and a disposable-board rehearsal. Then enable secure boot, flash encryption, NVS encryption, signed/anti-rollback OTA, and disable unnecessary debug/download paths according to Espressif's production guidance. ([Secure Boot v2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/secure-boot-v2.html), [flash encryption](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/security/flash-encryption.html), [security overview](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/security.html))

Use WPA2/WPA3 as appropriate, certificate validation, an up-to-date CA bundle, bounded reconnect backoff, and a provisioning access point that times out and requires local presence. ESP-IDF documents WPA3-Personal, Protected Management Frames, and OWE support, subject to network configuration. ([Wi-Fi security](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-security.html))

### 6.3 Voice privacy and failure safety

- Start with **push-to-talk**, an unmistakable on-screen listening state, and immediate local mute. Do not add an always-listening wake word until the product has an explicit privacy decision and AEC/wake false-positive measurements.
- Do not persist raw audio by default. Minimize transcript/action retention, state the retention period, and give the owner inspection/deletion controls.
- Disable AI Gateway payload collection if Gateway is used. Scrub authorization, Wi-Fi data, speech content, and tool payloads from device/Worker diagnostics.
- On authentication or policy failure, fail closed for actions but keep the local face and volume/mute controls operational.
- Treat model output and document/print content as untrusted. They never choose credentials, arbitrary URLs, printer addresses, file paths, SQL, or shell commands.
- Rate-limit sessions, audio bytes, model spend, shopping mutations, print prepares, pages, copies, and retries per device/user.

## 7. Action and tool design

Cloudflare's Agents tool guidance says tools can have side effects and may require approval. Its MCP guidance recommends fewer, goal-oriented tools with clear schemas and scoped permissions. The same design principles apply even if the first implementation uses ordinary Realtime function calls rather than MCP. ([Agents tools](https://developers.cloudflare.com/agents/concepts/tools/), [MCP tool design](https://developers.cloudflare.com/agents/model-context-protocol/), [MCP authorization](https://developers.cloudflare.com/agents/model-context-protocol/protocol/authorization/))

The model proposes typed intent; server code authenticates, authorizes, validates, executes, records, and returns a narrow result.

### 7.1 Shopping list

Prefer goal-level tools:

```json
{
  "name": "shopping_list.add_items",
  "arguments": {
    "list_id": "default",
    "items": [
      {"name": "oat milk", "quantity": 2, "unit": "cartons", "note": null}
    ],
    "idempotency_key": "device-session-turn-action"
  }
}
```

Also provide `shopping_list.read` and, if genuinely needed, `shopping_list.undo`. Server validation should:

- resolve `default` from the authenticated owner rather than a model-supplied account;
- normalize names/units but preserve the spoken original for confirmation;
- cap item count and field lengths;
- make the idempotency key unique for the authenticated principal/action;
- return canonical item IDs, the applied result, and a short-lived undo token.

Adding ordinary groceries is low-risk and reversible, so it may execute after an unambiguous utterance and then confirm verbally/visually. Ask a clarifying question when the item, quantity, or list is materially ambiguous. This is a **product-policy recommendation**, not a statement about an unspecified external shopping service.

### 7.2 Printing

Printing consumes paper/ink, can expose private content, and may execute complex parsers. Split intent from effect:

```json
{
  "name": "print.prepare",
  "arguments": {
    "printer_id": "home-default",
    "source": {"kind": "plain_text", "content": "Shopping list..."},
    "options": {"copies": 1, "duplex": false, "color": false},
    "idempotency_key": "..."
  }
}
```

`print.prepare` validates an allowlisted printer and source type, sanitizes/renders in an isolated bridge/service, and returns a `prepared_job_id`, content digest, title, page count, selected printer/options, expiry, and preview/summary. It does **not** print.

```json
{
  "name": "print.commit",
  "arguments": {
    "prepared_job_id": "...",
    "confirmation_token": "...",
    "idempotency_key": "..."
  }
}
```

`print.commit` succeeds only after a fresh, explicit user confirmation that names the important effect (“one black-and-white page on the kitchen printer”). It enqueues the immutable prepared artifact/options. Add `print.status` and `print.cancel` where the backend can truthfully support them.

Controls:

- allowlist printer IDs; never accept a model-supplied IP, hostname, URL, driver, queue name, or file path;
- initially allow only plain text and a tightly controlled generated PDF path; cap bytes, pages, copies and DPI;
- fetch no arbitrary remote URL; if later allowed, apply strict egress/SSRF controls and content scanning;
- sandbox parsers/renderers away from credentials and the LAN;
- bind prepared jobs and confirmations to the authenticated user/device, content digest and exact options;
- expire prepare/confirmation tokens and reject mutations;
- deduplicate before local CUPS/IPP submission because Queue delivery is at least once;
- record state transitions (`prepared`, `confirmed`, `queued`, `submitted`, `completed`, `failed`, `cancelled`) and expose uncertainty honestly;
- dead-letter or require manual retry after bounded failures; do not let the model retry a physical effect indefinitely.

### 7.3 Local-only controls

Face affect, gaze target, temporary brightness, and volume are device capabilities, not cloud account tools. Define a tiny bounded control schema, clamp all numbers in firmware, and keep safety-critical mute/button handling local. Model emotion is a suggestion layered under explicit interaction states: it cannot make a “success” face before the backend confirms success or hide `confirmation_required`/`error`.

## 8. Staged proof-of-concept plan

Each stage has a stop/go gate. Do not begin with hands-free voice, production eFuses, arbitrary document printing, or long-term memory.

### Stage 0 — identify dependencies and the board

- Photograph/read PCB revision markings and reconcile them with the Waveshare original/V2 documentation.
- Select the exact pinned ESP-IDF and current Waveshare BSP versions.
- Decide the actual shopping-list system of record: project-owned D1 list, Apple/Google/Alexa integration, retailer API, or something else.
- Identify printer make/model, supported IPP/CUPS/vendor path, LAN topology, and the always-on host available for the bridge.
- Define privacy/retention, household users, provisioning/reset, and whether the prototype leaves the home.

**Gate:** revision, list target, printer path, and data policy are explicit; no pin/driver guess remains.

### Stage 1 — vendor-derived hardware characterization

- Run/adapt the pinned vendor board-check and the smallest display, touch, PMIC, audio and SD examples.
- Record detected panel/touch revision, flash/PSRAM, pin map, battery/PMIC readings, internal/PSRAM heap, largest blocks, display frame/flush time, and I²S stability.
- Loop microphone-to-buffer and known test audio separately; do not attempt cloud voice yet.

**Gate:** 30-minute display/audio soak without crashes or sustained I²S errors, with a documented memory/thermal/battery baseline. If it fails, reduce display buffers/features before adding TLS.

### Stage 2 — offline embodied face

- Implement the local state model and deterministic blink, breath, saccade, gaze and interpolation.
- Add push-to-talk/listening/thinking/speaking/confirmation/success/error states.
- Drive mouth openness from a local test waveform/playback envelope.
- Add pixel wandering, brightness management, screen sleep, touch/button mute, and an obvious listening indicator.

**Gate:** responsive face and controls remain smooth during simultaneous full-duplex I²S stress; network absence never freezes the face.

### Stage 3 — instrumented transport bake-off

- Define the device protocol: authenticated control JSON, sequenced/timestamped binary PCM frames, server events, cancellation, keepalive, reconnect, and version negotiation.
- Compare direct-to-OpenAI with a short-lived credential, Worker/DO relay, and Worker/DO through AI Gateway using identical audio/traces.
- Test 20/40/60/100 ms chunks and the required 24 kHz path or an explicit high-quality resampler.
- Disable voice payload logging and validate that logs contain no credential/audio content.

**Gate:** the selected route meets the provisional latency/reliability targets and has a credential/session-recovery story. Default to the relay unless direct evidence shows it is untenable.

### Stage 4 — voice and character, no side effects

- Use `gpt-realtime-2.1`, push-to-talk, one voice chosen before first output, the versioned constitution, and no mutating tools.
- Implement cancellation/truncation correctly and reconnect before the documented 60-minute session limit.
- Test silence, noise, rapid re-press, interruption, weak Wi-Fi, provider failure, expired device credentials, and prompt attempts to override policy.
- Compare model/voice/instruction variants with human ratings for warmth, concision, intelligibility, interruption behaviour, and p50/p95 audible latency.

**Gate:** stable conversations with bounded spend and correct mute/barge-in behaviour; the robot never implies an action happened when it did not.

### Stage 5 — shopping list

- Add authenticated D1-backed read/add/undo, typed schemas, caps, idempotency, audit, visual/verbal confirmation, and offline/retry semantics.
- Test ambiguous quantities, homophones, duplicate network delivery, two rapid turns, reconnect after execution, cross-device isolation, and undo.

**Gate:** no duplicate or cross-owner mutation in retry/security tests; every applied change has a canonical result and auditable idempotency record.

### Stage 6 — printing, dry run before paper

- Build only `prepare`, preview/summary, confirmation, Queue delivery, and a bridge simulator first.
- Exercise duplicates, reordered status, bridge outage, malformed files, excessive pages/copies, cancellation races, expired confirmations, and malicious document text.
- Enable one allowlisted printer and one-page plain-text/PDF jobs only after simulator tests pass.

**Gate:** a confirmed job prints at most once; an unconfirmed, expired, mutated, duplicate, or unauthorized job never reaches the printer. State remains recoverable after every induced failure.

### Stage 7 — hardening and operational readiness

- Signed OTA with rollback and recovery rehearsal; credential rotation/revocation; redacted metrics; spend/action rate limits; alerting; dependency update process.
- Rehearse secure boot/flash/NVS encryption on a disposable unit, then decide whether to enable irreversible production settings.
- Run long-duration power, Wi-Fi, memory fragmentation, AMOLED mitigation, queue outage, provider outage, and bridge outage tests.
- Write owner-facing controls for mute, data deletion, memory inspection/reset, device unlink, and factory reset.

**Gate:** documented recovery from bad OTA, lost credential, lost Wi-Fi, cloud outage and factory reset; security settings do not make field recovery impossible.

### Stage 8 — optional hands-free behaviour

- Evaluate WakeNet and ESP-SR AFE/AEC with the final enclosure and actual playback reference.
- Measure false accepts/rejects, echo/barge-in, CPU/memory, battery, and privacy acceptability.
- Retain a physical mute/listening indicator and a push-to-talk fallback.

**Gate:** hands-free mode is no worse than push-to-talk on privacy controls and meets explicit acoustic and resource budgets. Otherwise, ship push-to-talk.

## 9. Decision and evidence ledger

### Verified facts

- The board has the documented ESP32-S3R8, 8 MB PSRAM, 16 MB flash, 368 × 448 touch AMOLED, ES8311 audio path, Wi-Fi/BLE, PMIC, RTC, IMU and microSD; two display/touch revisions exist. ([Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8), [schematic](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8/ESP32-S3-Touch-AMOLED-1.8.pdf), [repository README](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/blob/ba32b5cbca96f0e04b0736d04959b6e832268d3f/README.md))
- Waveshare provides current ESP-IDF board-check, I²S/ES8311, and LVGL 9 examples using its managed BSP. ([examples](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8/tree/ba32b5cbca96f0e04b0736d04959b6e832268d3f/examples/esp-idf))
- `chat-stick` contains a working-design reference for this board's Arduino audio/UI and a raw-audio device → Worker/Durable Object → live-model relay, but its board code selects SH8601 and its shared auth is optional. ([README](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/README.md), [board code](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/devices/firmware/waveshare/src/hal/Board.cpp), [Worker](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/server/src/index.ts), [session object](https://github.com/steveruizok/chat-stick/blob/3321c9bfc9771ee8b3adc4815f6c72890d3db125/server/src/live-session.ts))
- StackChan separates face state from blink/breath/saccade generators and demonstrates audio-power lip motion. ([face state](https://github.com/stack-chan/stack-chan/blob/c25bba5273dbdb23cc0bcc29b7df9494365272c5/firmware/host/modules/ui/state/face-state.ts), [face behaviour](https://github.com/stack-chan/stack-chan/blob/c25bba5273dbdb23cc0bcc29b7df9494365272c5/firmware/host/modules/ui/components/face/behaviors/face.ts), [lip sync](https://github.com/stack-chan/stack-chan/blob/c25bba5273dbdb23cc0bcc29b7df9494365272c5/firmware/mods/examples/lip_sync/mod.js))
- OpenAI documents GPT-Realtime-2.1, server WebSockets, client WebRTC/ephemeral credentials, audio streaming/tool calls, session/voice constraints, VAD and push-to-talk control. ([model](https://developers.openai.com/api/docs/models/gpt-realtime-2.1), [WebSocket](https://developers.openai.com/api/docs/guides/realtime-websocket), [WebRTC](https://developers.openai.com/api/docs/guides/realtime-webrtc), [conversations](https://developers.openai.com/api/docs/guides/realtime-conversations), [official meeting-assistant example](https://github.com/openai/openai-realtime-meeting-assistant), [official Python SDK Realtime example](https://github.com/openai/openai-python/blob/main/README.md))
- Cloudflare currently supports Worker/Durable Object/Agent WebSockets and an OpenAI Realtime AI Gateway path; Queue delivery is at least once; AI Gateway payload logging needs an explicit privacy decision. ([Workers WebSockets](https://developers.cloudflare.com/workers/runtime-apis/websockets/), [Durable Objects WebSockets](https://developers.cloudflare.com/durable-objects/best-practices/websockets/), [Agents API](https://developers.cloudflare.com/agents/runtime/agents-api/), [Gateway Realtime](https://developers.cloudflare.com/ai-gateway/usage/websockets-api/realtime-api/), [Queue delivery](https://developers.cloudflare.com/queues/reference/delivery-guarantees/), [Gateway logging](https://developers.cloudflare.com/ai-gateway/observability/logging/))
- The attached USB device enumerated as an Espressif USB Serial/JTAG unit; no serial port was opened and no device state was changed.

### Inferences and recommendations

- ESP-IDF + the revision-aware Waveshare BSP + LVGL 9 is the strongest base for this product; `chat-stick`'s Arduino firmware is a reference and benchmark.
- Keep the face/audio scheduler local and deterministic; let the voice model select only bounded semantic affect.
- Use push-to-talk first, raw PCM binary to a per-device Durable Object relay, and keep standard provider keys and action authority off-device.
- Start with GPT-Realtime-2.1; test alternatives against audible p50/p95, cost, conversational quality, and tool correctness.
- Use D1 for list/job/idempotency state, Queue plus an outbound LAN bridge for printing, and AI Gateway only when its measured operational value exceeds its added path/credential/privacy complexity.
- Keep “soul,” durable memory, and authorization as separate versioned domains.

### Unknowns

- Actual board/panel revision and exact BSP runtime detection result.
- Final enclosure acoustics, AEC feasibility, wake-word quality, speaker volume, battery life, heat, Wi-Fi conditions and memory margin.
- Current project/account access, regional availability, rate limits and commercial terms for the selected OpenAI model; exact accepted audio formats must be re-read from the current schema.
- Required shopping-list integration and its authentication/conflict/offline semantics.
- Printer, bridge host, document formats, driver/CUPS/IPP/vendor API, and whether status/cancel can be truthful end to end.
- Number/identity of household users, consent/retention requirements, desired durable memory, and whether remote use or hands-free listening is required.
- Whether the latency targets are achievable through the chosen ISP/Wi-Fi/Cloudflare/OpenAI route.

### Principal risks

| Risk | Earliest control/test |
|---|---|
| Wrong display/touch revision is assumed | Stage 0 physical identification + vendor BSP/board-check |
| Audio under/overruns or poor acoustic echo performance | Stage 1 resource telemetry; Stage 8 only after push-to-talk |
| Latency feels lifeless despite model speed | End-to-end DAC-based traces; local thinking/listening animation; chunk bake-off |
| Standard/cloud credential extracted from firmware | Never place standard key on device; per-device revocable credential; encrypted storage and later secure boot/flash encryption |
| Shared-token impersonation or cross-device routing | Unique identity, server-bound object ID, authorization header, rotation/revocation |
| Duplicate shopping/print effects after retry | Transactional idempotency records; Queue consumer dedupe; immutable prepared print job |
| Prompt injection causes data access or arbitrary printing | Typed narrow tools, allowlists, content treated as data, prepare/commit, server policy outside prompt |
| Accidental/private audio retention | Push-to-talk, visible listening state, minimal retention, Gateway payload logging disabled, redacted diagnostics |
| Static face damages AMOLED or wastes battery | Dark moving UI, adaptive dim, pixel wandering, screen sleep, long-run panel/power tests |
| OTA/security setting bricks field units | Signed OTA/rollback/recovery first; disposable-board eFuse rehearsal; documented key custody |
| Cloud/Internet outage makes the object appear dead | Local face/state/mute always work; honest offline animation and bounded reconnect |
| “Personality” overrides truth or safety | Constitution cannot change tool schemas, authorization, confirmation, or verified action results |

## 10. Immediate next decision

The first implementation task should not be “connect the model.” It should be a disposable hardware-characterization build derived from Waveshare's pinned ESP-IDF board-check, I²S and LVGL examples, followed by the offline face. In parallel, choose the actual shopping-list system and printer/bridge path. Those facts determine the tool contracts; the measured board margin and audio behaviour determine whether the recommended realtime architecture is viable.

No firmware or product code was implemented as part of this research.
