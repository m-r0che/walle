# Sprite face candidate

Date: 2026-08-05
Branch: `feature/sprite-face`
Base: `0436008 Force Walle output volume to maximum`

## Candidate firmware

- Build artifact: `firmware/build/walle.bin`
- Accepted physical candidate: `2c42c2b Fill screen with antenna-free sprite face`
- Flashed binary SHA-256: `efc485192781639ab52dd41c63e06f5821fda7987a38a35aff8c11f8bb46aa29`
- App size reported by ESP-IDF: `0x23b750`
- Smallest app partition: `0x800000`
- Free app partition space: `0x5c48b0` / 72%

## What changed

- Copied the supplied parts sheet and composite reference into `design/assets/`.
- Sliced the parts sheet into component sprites under `design/generated/face-parts/`.
- Added a semantic part-name map in `design/face-parts-names.json`.
- Added preview and firmware-pack generation tools:
  - `tools/face_sprite_preview.py`
  - `tools/face_sprite_pack.py`
- Generated flash-resident RGB565 + A8 sprite assets:
  - `firmware/main/face_sprite_assets.h`
  - `firmware/main/face_sprite_assets.c`
- Added a `FACE_USE_SPRITES` renderer path in `firmware/main/face.c`.

## Preserved constraints

- Display transport is unchanged.
- Panel remains in proven portrait addressing.
- LVGL software rotation path remains unchanged.
- Face canvas is full logical screen size, `448x368`, after physical inspection found the candidate responsive enough.
- Object-level canvas drift is disabled while full-screen so LVGL never slides the canvas off-panel.
- Sprite parts are composited into that PSRAM canvas before the existing display path.
- No audio, network, WebSocket, or cloud protocol changes.
- Mouth selection still follows local `playback_level`, which is derived from PCM entering the codec.
- Truthful activity remains device-owned.

## Smoke check

After flashing `2c42c2b` to `/dev/cu.usbmodem1101`:

- Flash verification passed.
- Device reported firmware `2c42c2b`.
- Cloud connection reported ready.
- Provider reported `openai`.
- Sanitized idle telemetry was clean across a short post-flash soak:
  - `queueDepth`: 0
  - `audioDropped`: 0
  - `audioQueueFull`: 0
  - `audioBackpressure`: 0
  - `protocolErrors`: 0
  - `socketRestarts`: 0
  - `outputEventDrops`: 0

## Physical inspection

Accepted as the current visual direction after physical inspection:

- Full-screen face canvas feels responsive enough.
- Face fills the screen better.
- Sleep `Z`s are no longer the active blocker.
- No display transport constants were changed.

## Remaining before promotion

- Confirm the face is visible after a true cold boot.
- Confirm there is no wrapping/corruption during a longer visible soak.
- Confirm hold-to-speak and speaking mouth remain responsive during real interaction.
- Run a 20-30 minute visible soak before promoting to `main`.
