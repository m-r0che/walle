# Firmware

ESP-IDF firmware for the Waveshare ESP32-S3-Touch-AMOLED-1.8 V2.

## Pinned toolchain

- ESP-IDF 6.0.2
- Waveshare `esp32_s3_touch_amoled_1_8` BSP 2.0.3
- Target: ESP32-S3

The current firmware is a network-connected push-to-talk echo prototype built on the pinned Waveshare display/touch BSP and LVGL 9. It renders an eyebrow-free thick-line neon face with circular open eyes, independently moving pupils, breathing, blinking, gaze/saccades, and eleven selectable authored expressions. Truthful listening/thinking/speaking states override the demo affect while audio is active. Local interaction and echo remain available without cloud services. When the authenticated relay is ready, committed microphone PCM is also copied into a bounded, non-blocking PSRAM queue and sent as sequenced binary frames; queue pressure or relay failure only drops the cloud copy and cannot block local capture or replay.

A direct RGB565 rasterizer draws into a 320 × 220 PSRAM canvas centered on the 368 × 448 display. LVGL's software renderer uses one project-owned internal DMA buffer, retained until `on_color_trans_done`. A direct diagnostic reproduced a physical CO5300 limit that completion telemetry misses: sustained/bursty partial writes can blank the panel despite every callback succeeding. The former offline build avoided that limit with one 220-row complete-face transaction, but linking Wi-Fi/TLS left insufficient contiguous internal RAM for that 140.8 KB allocation. Production now uses a 110-row buffer and enforces at least 35 ms between every panel submission, eliminating paired bursts and holding traffic near 28.6 transactions per second. This yields approximately 14.3 FPS. A 2.5-minute physical soak exceeded 4,400 transfers with zero overlap/submit errors, stable ~79 KB free internal RAM (~74 KB minimum), an authenticated WSS connection, and a visible panel. Cold-boot testing also proved that a second post-UI reset/init blacks the CO5300 after its first valid frame, so the initial `display_port_start()` initialization is authoritative and the hazardous reinitialization API has been removed.

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

- Press and hold the main face while speaking.
- Release to stop recording.
- After a short thinking expression, the device replays up to six seconds through the speaker at the selected volume.
- Playback RMS drives mouth opening locally.
- The first 180 ms are retained in a pre-commit ring; short taps are discarded without losing the beginning of valid recordings.
- Pressing again during replay cancels playback locally and immediately starts a new recording.
- Tap the small lower-left ring to toggle logical mute. Muted mode retains no microphone samples, but the present codec implementation continues clocking and discarding input frames; it is not yet a hardware microphone disconnect.
- Use the large bottom `−` and `+` buttons to adjust output from 10–100% in 10% steps. Changes are queued through the audio task and persisted in NVS.
- Tap the center expression button to cycle `OPEN`, `PEEK`, `WHAT?!`, `DOZY`, `WORRY`, `HAPPY`, `CURIOUS`, `DEVIOUS`, `ANGRY`, `FURY`, and `SAD`.

The current prototype uses fixed 45% display brightness and bounded PSRAM rings: 220 ms pre-commit, six-second offline capture, and 500 ms playback staging (322,560 bytes total). The six-second capture capacity preserves the local echo test. The cloud sender now receives committed prebuffer/live samples concurrently in chunks of at most 256 samples, with two queue slots reserved for turn control. Echoed output is sequence/length/hash checked and counted but is not yet played. The canvas follows a bounded ±3 px minute-scale drift pattern to reduce static AMOLED exposure. Runtime panel dimming remains disabled until it is retested on the corrected transfer path. The device does not enter ESP light/deep sleep, and microphone draining continues.

## Hardware diagnostics

Repeatable diagnostics live outside the production app:

- [`diagnostics/audio_duplex`](../diagnostics/audio_duplex/) — finite low-volume speaker/microphone loop test
- [`diagnostics/display_audio_soak`](../diagnostics/display_audio_soak/) — 30-second face plus muted duplex-I²S stress test
- [`diagnostics/display_transfer`](../diagnostics/display_transfer/) — finite CO5300 transfer-completion and buffer-ownership test
