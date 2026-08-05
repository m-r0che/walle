# Face sprites — handoff summary (2026-08-05)

Where the sprite-face effort got to, for whoever picks it up next.

## Goal and decided approach

Replace/augment the fully procedural neon face with generated character art:
a **hybrid renderer** — static sprite parts composited into the existing
344×286 RGB565 PSRAM face canvas, with dynamics rendered procedurally on top
(pupil/gaze movement, blink occlusion, RMS-driven mouth selection, drifting
Zs, pulsing hearts, pupil-size pulse for listening).

We explicitly moved away from whole-face animation frames (16 expressions ×
4 frames): the parts approach needs far less art, sidesteps image-model
tile-alignment drift, and keeps blink/gaze/mouth as smooth parametric motion.

## Feasibility (verified against firmware docs)

- 8MB OPI PSRAM, ~7.4MB free minimum (`docs/bringup-log.md`) — entire part
  set is well under 1MB in RGB565. Blit bandwidth at the panel's ~14 FPS
  cadence is a few MB/s: trivial.
- Constraint: internal RAM is tight (~79KB free). Sprite assets must live in
  PSRAM (loaded from flash at boot), never internal RAM.
- The renderer already draws into a PSRAM canvas then submits via the
  110-row internal DMA buffer (≥35ms spacing) — sprite blits slot in as a
  layer before the procedural draws. No display-path changes needed.

## Character design (chosen, generated art exists)

Cute retro robot head: cream faceplate, mint/green rounded bezel, orange
ears/antenna/nose, black background. Flat solid colours, black outlines with
a thin white halo (halo composites fine; can be stripped by eroding masks).

Source images (in ~/Downloads, not yet committed — consider copying into
`design/`):
- `ChatGPT Image Aug 5, 2026, 02_18_31 PM.png` — 16-expression tile sheet
  (the design reference).
- `ChatGPT Image Aug 5, 2026, 02_34_36 PM.png` — **the parts sheet, validated
  and slice-ready.**

## What's been validated

- Composite test: assembled open eyes + pupils + smiling mouth onto the blank
  base head programmatically — coherent, on-character result.
- Auto-slicing: parts never touch, so connected-component segmentation
  extracts all **59 parts** with no manual crop boxes. Black interiors
  (mouths, pupils) on the black background are preserved because closed
  outlines stop the border flood fill.
- Tool committed: `tools/face_parts_slicer.py <sheet.png> <out_dir>` → alpha
  PNG per part + `annotated.png` overview + `manifest.json` bounding boxes.

Part inventory (complete for the expression set): base head + antenna ball +
2 ears; 7 eye-white pairs (open, surprised-tall, half-lidded, worried,
devious, angry, teary); happy + sleeping closed arcs; 2 cream eyelid blink
overlays; pupils in 3 sizes + heart pupil + 2 flame pupils; 11 mouths (incl.
4 talking shapes for lip sync); 9 eyebrow strokes; accents (Z ×2, heart,
sparkle, sound waves, blush ticks).

## Related docs

- `design/parts-sheet-prompt.md` — the prompt that produced the parts sheet
  (Study 03, current approach).
- `design/sprite-sheet-prompt.md` — earlier whole-face sheet prompts
  (Study 02, superseded but kept for reference).
- `design/face-lab.html/.png` — original thick-line neon study (Study 01).

## Next steps (agreed order, not started)

1. Name the 59 parts in `manifest.json` (`eye_open_l`, `mouth_talk_wide`, …)
   using the annotated overview to map indices.
2. Asset packer: RGB565 + A8 (or 1-bit) masks into a flash partition; load to
   PSRAM at boot. Optionally erode masks ~1-2px to drop the white halo.
3. Renderer: blit layer in the existing rasterizer — base head → eye whites →
   pupils → brows → mouth → accents — driven by the existing face-state
   machine choosing parts instead of stroke drawing. Blink = cream eyelid
   shapes (or procedural flat-colour occlusion) over the eye region; SPEAKING
   maps playback RMS to the 4 talking mouths; LISTENING pulses pupil size.
