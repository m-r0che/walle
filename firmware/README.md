# Firmware

ESP-IDF firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.8 V2.

## Pinned toolchain

- ESP-IDF 6.0.2
- Waveshare `esp32_s3_touch_amoled_1_8` BSP 2.0.3
- Target: ESP32-S3

The current firmware is a network-connected push-to-talk voice robot built on the pinned Waveshare display/touch BSP and LVGL 9. It renders a layered neon landscape face with asymmetric brows, solid luminous pupils, interpolated activity/mood/reaction poses, autonomous blink and gaze, slow breathing, and a mouth driven by PCM actually entering the codec. The entire face has one meaning: hold to speak. Truthful listening, thinking, speaking, error, offline, and sleeping activity remains device-owned. Local interaction and fallback remain available without cloud services.

A direct RGB565 rasterizer draws into a 344 × 286 PSRAM canvas. LVGL rotates bounded landscape regions into one project-owned internal DMA buffer retained until `on_color_trans_done`, while the CO5300 remains in its proven portrait address mode. Hardware axis swapping visibly corrupted wide windows and is rejected. Production uses a 110-row buffer and enforces at least 35 ms between every panel submission; no display reinitialization occurs after the first visible frame. The complete face therefore has a physically bounded visible cadence of approximately 14 FPS even when local interpolation is requested more frequently.

Committed microphone PCM enters a bounded nonblocking PSRAM queue and is sent as sequenced 960-sample frames. Returned generated PCM becomes playable after 500 ms of contiguous validation against the turn and authoritative sample counts. Untouched local capture remains fallback until the first remote codec write; after remote playback owns the speaker, a fault stops rather than mixing sources. The QMI8658 is sampled at low priority for meaningful movement only: after 60 seconds of idle stillness the local face sleeps with drifting `Z`s, and motion or touch wakes it without changing network/session ownership.

## Environment

ESP-IDF is installed outside the repository:

```bash
. "$HOME/.espressif/frameworks/esp-idf-v6.0.2/export.sh"
```

## Local network credentials

Run the interactive setup from the repository root:

```bash
./tools/configure-device.py
```

It prompts for home Wi-Fi and an optional office Wi-Fi, hides passwords while typing, imports the already-generated relay token, and writes `firmware/main/credentials.h` with mode `0600`. That filename is ignored by Git. This first version supports open and WPA/WPA2/WPA3-Personal networks; office captive portals or enterprise username/certificate authentication need a different path. The generated header embeds prototype credentials in the firmware image; because flash encryption is disabled, someone with physical flash access could extract them.

The network manager scans for both entries, selects the strongest visible configured access point, and rescans after disconnection rather than requiring a reflash between home and office. It verifies the Workers.dev certificate chain with the ESP-IDF certificate bundle and authenticates with the generated bearer credential. WebSocket callbacks never wait on the stream mutex; the manager owns teardown and queue reset. Each client has a wire-visible session epoch, connect/ready/pong deadlines, capped jittered retry, and a bounded two-minute prototype lifetime that forces eventual migration across relay deployments.

## Build

```bash
idf.py -C firmware set-target esp32s3
idf.py -C firmware build
idf.py -C firmware -p /dev/cu.usbmodem2101 -b 460800 flash
```

The `set-target` command is only needed for a new build directory. Generated `sdkconfig`, build products, and managed components are intentionally ignored.

Do not flash a device until its existing flash has been privately backed up and verified. The attached development device's backup and validation results are recorded in [`docs/bringup-log.md`](../docs/bringup-log.md).

## Interaction

- Press and hold anywhere on the face while speaking.
- Release to commit the recording; short taps are discarded without losing the beginning of valid turns.
- After a truthful thinking transition, the device plays validated buffered remote speech or untouched local fallback at the selected volume.
- Playback RMS drives mouth opening locally.
- Pressing the face during replay cancels playback and immediately starts a new recording.
- Use the compact transparent lower-corner `−` and `+` controls to adjust output from 10–100% in 10% steps. The value briefly appears, is applied by the audio task, and persists in NVS.
- After 60 seconds of idle stillness the face sleeps locally. Move the device or hold the face to wake; a continuing hold proceeds directly into push-to-talk.

The current prototype uses fixed 45% display brightness and bounded PSRAM storage: 220 ms pre-commit, 30-second local capture, 30-second generated-response retention, and 500 ms playback staging. The local capture remains untouched until remote playback commits. The cloud sender receives committed prebuffer/live samples concurrently in chunks of at most 256 samples, with two queue slots reserved for turn control. The network owner preserves that codec cadence while batching samples into 960-sample WebSocket frames and flushing the final partial frame before commit. Generated output crosses a bounded zero-wait SPSC queue into an independently bounded rolling 30-second PSRAM response buffer; push-to-talk input is bounded to 30 seconds. Echo responses remain complete-turn validated. Explicitly negotiated OpenAI responses may begin after 500 ms of contiguous validated PCM; local fallback remains authoritative until the first remote codec write, after which a fault stops playback rather than mixing in local echo. Physical tests passed exact remote playback, dropped-frame and wrong-count fallback, interruption, reconnect, display soak, and true cold boot. The canvas follows a bounded ±3 px minute-scale drift pattern to reduce static AMOLED exposure. Runtime panel dimming remains disabled until it is retested on the corrected transfer path. Face sleep is cosmetic and local: the device does not enter ESP light/deep sleep, Wi-Fi remains connected, and microphone draining continues.

## Hardware diagnostics

Repeatable diagnostics live outside the production app:

- [`diagnostics/audio_duplex`](../diagnostics/audio_duplex/) — finite low-volume speaker/microphone loop test
- [`diagnostics/display_audio_soak`](../diagnostics/display_audio_soak/) — 30-second face plus muted duplex-I²S stress test
- [`diagnostics/display_transfer`](../diagnostics/display_transfer/) — finite CO5300 transfer-completion and buffer-ownership test
