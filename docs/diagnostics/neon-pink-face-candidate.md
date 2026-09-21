# Neon pink face candidate

Date: 2026-09-21
Branch: `feature/neon-pink-face`
Base: `62b21dc Restore antenna and remove bottom controls`

## Decision

Return to the procedural neon face that was accepted on-device through
`0436008`, recoloured from neon cyan to neon pink. The sprite-composited
face from 2026-08-05 remains in `firmware/main/face.c` behind
`FACE_USE_SPRITES`, now `0`, and its asset file is no longer compiled.

## Palette

| Role | Cyan (before) | Pink (after) |
| --- | --- | --- |
| Deep glow, outer two passes and pupil halo | `#006775` | `#75003b` |
| Ice core, inner pass and pupil core | `#b9ffff` | `#ffb9dc` |

The pink values are the cyan values rotated to a hot-pink hue with the same
saturation and value, so the three-pass dimmed RGB565 glow keeps its shape on
black. RGB565 gives red five bits where green had six, so the faintest outer
pass quantises to one fewer step in its dominant channel; the off-device
render shows this is not visible at 1x.

Off-device stills, rendered with `tools/face_neon_preview.py`:

- `design/generated/face-neon-pink-preview.png`
- `design/generated/face-neon-cyan-vs-pink.png`

## What was carried over from the sprite work

- Off-device preview before flashing. The sprite effort added
  `tools/face_sprite_preview.py`; this change adds the equivalent for the
  procedural renderer so a palette or geometry change can be reviewed
  without the device attached.
- The `FACE_USE_SPRITES` switch, the full-screen drift guard in
  `update_position_drift`, and the sprite renderer are kept intact so the
  sprite direction can be revisited by flipping one define and restoring
  `face_sprite_assets.c` to the component sources.

## What was deliberately not carried over

- Full-screen canvas. The sprite candidate moved to a `448x368` canvas and
  was accepted as "responsive enough". The neon face relies on continuous
  blink, gaze, and breathing motion, and every frame invalidates the whole
  canvas. The display path flushes through one 110-row internal buffer with
  at least 35 ms between transfers, so a taller canvas costs extra transfers
  per frame and lowers the visible cadence below the roughly 14 FPS recorded
  for the bounded canvas. The bounded canvas also keeps the ±3 px anti
  burn-in drift, which the full-screen path had to disable. Geometry is
  therefore unchanged from `0436008`.
- Sprite expression mapping. The parametric pose tables already cover mood
  and activity; nothing from the sprite selection logic was needed.

## Build

- `idf.py -C firmware build` succeeds on ESP-IDF 6.0.2 with no warnings
  from `face.c`.
- App size reported by ESP-IDF: `0x1610f0` (sprite candidate was
  `0x23b750`). Smallest app partition: `0x800000`, 83% free.

## Smoke check

Flashed `d35532d` to `/dev/cu.usbmodem1101` on 2026-09-21; app SHA-256
`9d3df39facc38a1278222f6b563abdf98486366fce3d6b58ec92d8228ead1cdd`.
Serial capture over the first 45 seconds after a reset:

- Flash verification passed and the device reported app version `d35532d`.
- Display port came up as `448x368 software-rotated landscape, one 110-row
  internal DMA buffer, minimum interval=35000us`, unchanged.
- Face telemetry settled at 8.4–9.1 FPS with 32–40 ms average render time
  and about 27 panel submissions per second, so the 344x286 canvas is
  flushing as three transfers per frame under the 35 ms pacing.
- 1,048 display submissions, 1,048 completions, zero submit errors, zero
  overlaps.
- Wi-Fi connected, authenticated WSS established, relay protocol ready.
- One hold-to-speak turn completed end to end: PTT press and release,
  remote playback from first codec write at 1,251 ms release latency,
  semantic affect applied, playback complete with zero failures.
- Free internal RAM held around 55 KB with a 39 KB minimum; PSRAM steady.

## Remaining before promotion

- Owner visual confirmation of the pink on the AMOLED and of blink, gaze,
  and speaking-mouth responsiveness. Telemetry cannot prove panel output.
- Confirm the face is visible after a true cold power-off boot, not only
  the post-flash reset above.
- Run a 20–30 minute visible soak before promoting to `main`.
