# Sprite face candidate

Date: 2026-08-05
Branch: `feature/sprite-face`
Base: `0436008 Force Walle output volume to maximum`

## Candidate firmware

- Build artifact: `firmware/build/walle.bin`
- SHA-256: recorded after the final post-commit build/flash, because the ESP-IDF app descriptor embeds the git version and therefore changes when this note is amended.
- App size reported by ESP-IDF: `0x2016e0`
- Smallest app partition: `0x800000`
- Free app partition space: `0x5fe920` / 75%

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
- Existing 344x286 PSRAM face canvas remains unchanged.
- Sprite parts are composited into that PSRAM canvas before the existing display path.
- No audio, network, WebSocket, or cloud protocol changes.
- Mouth selection still follows local `playback_level`, which is derived from PCM entering the codec.
- Truthful activity remains device-owned.

## First smoke check

After flashing the candidate to `/dev/cu.usbmodem1101`:

- Flash verification passed.
- Cloud connection reported ready.
- Provider reported `openai`.
- Playback mode remained `buffered`.
- Sanitized telemetry showed zero queue/audio/protocol/socket errors while idle.

## Not yet accepted

This candidate still requires physical inspection:

- Confirm the face is visible after a true cold boot.
- Confirm there is no wrapping/corruption.
- Confirm the sprite style reads well at device size and 45% brightness.
- Confirm hold-to-speak still feels responsive.
- Confirm speaking mouth updates are lively enough and do not starve audio/network work.
- Run a 20-30 minute visible soak before promoting to `main`.
