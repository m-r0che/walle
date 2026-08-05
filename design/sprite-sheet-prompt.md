# Face sprite sheets — generation prompts

Study 02 companion to `face-lab.html`. Target: on-device animated sprite frames
for the 368×448 AMOLED. 16 expressions × 4 animation frames = 64 tiles, split
across **four sheets** so each sheet stays a model-friendly 4×4 grid:

- Each sheet: 4 rows (one expression per row) × 4 columns (animation frames
  left to right), tiles 368×448, sheet 1472×1792.
- Sheet 1: OPEN, BLINK, PEEK, WHAT?!
- Sheet 2: DOZY, WORRIED, HAPPY, INTERESTED
- Sheet 3: DEVIOUS, ANGRY, FURIOUS, SAD
- Sheet 4: SLEEPING, LISTENING, SPEAKING, LOVE

Frame 1 of every row is the canonical pose. Frames 2–4 are small deltas for a
ping-pong idle loop (1→2→3→4→3→2→1), except BLINK and SPEAKING, whose frames
are a sequence of positions the renderer picks directly.

Flat rendering (no bloom/glow — the AMOLED and renderer provide the pop) and a
limited palette so frames survive RGB565 quantization and align when swapped.

## Variant A — detailed (house style)

Shared prompt; swap in the row block for the sheet being generated.

```
A sprite sheet of animated robot face frames for a small desk robot, arranged
in a clean 4×4 grid of 16 equal-sized tiles on a pure black background
(#000000), each tile 368×448 pixels. Each ROW is one facial expression; the
four tiles in a row are consecutive animation frames of that expression, left
to right. The face is identically positioned in every tile, and between frames
in a row only small details move (pupils, eyelids, mouth, accents) — the frames
must look like one face captured mid-motion, not four redesigns.

Style: cute, child-friendly retro cartoon robot face — like a friendly 1950s
diner mascot crossed with Miss Minutes from Loki. Bold, confident, rounded
shapes: thick rounded strokes and simple filled shapes are both welcome. Each
face is minimal: two large circular eyes with small pupils, one simple
expressive mouth, and at most one small accent detail (blush mark, sparkle,
heart, or drifting Z) where the expression calls for it. No eyebrows, no nose,
no face outline, no head or body — the face floats on black. Slight charming
asymmetry; eye shape and pupil placement do the emotional work.

Colour: flat solid colours only — glowing-look cyan/teal (#3EE8E0) as the base,
warm amber (#FFB84D) as a sparing accent (pupils, blush, hearts, sparkles),
plus white and black. No gradients, no glow, no bloom, no soft shadows — crisp
flat shapes with clean edges, saturated and cheerful, never harsh or scary.

<ROW BLOCK — insert the four rows for this sheet>

Flat vector-like rendering, no text, no labels, no tile borders, no watermark.
Consistent eye size, mouth baseline, and face position in all 16 tiles.
```

Row blocks:

Sheet 1:
```
Row 1 — OPEN: round attentive eyes, gentle smile, warm and ready to listen.
Frames: pupils drift slightly and the smile softens, a calm idle loop.
Row 2 — BLINK: calm smile throughout. Frames: eyes fully open → half-closed →
fully closed soft arcs → half-open (the four positions of one blink).
Row 3 — PEEK: half-lidded curious eyes glancing sideways, small smile.
Frames: the sideways glance sweeps a little further and eases back.
Row 4 — WHAT?!: wide mismatched eyes, small round surprised mouth.
Frames: the surprise pops — eyes widen and the mouth grows slightly, then eases.
```

Sheet 2:
```
Row 1 — DOZY: heavy drooping eyelid arcs, sleepy little smile.
Frames: eyelids sag lower and recover, a drowsy bobbing loop.
Row 2 — WORRIED: eyes tilted up at the inner corners, small downturned mouth.
Frames: pupils shift uneasily side to side, mouth wavers slightly.
Row 3 — HAPPY: closed happy arc eyes, big cheerful smile.
Frames: the smile broadens with a gentle bounce and settles.
Row 4 — INTERESTED: one eye slightly larger, bright smile, leaning-in feel.
Frames: pupils enlarge a touch and the lean intensifies, then relaxes.
```

Sheet 3:
```
Row 1 — DEVIOUS: narrowed angled eyes, lopsided smirk.
Frames: the smirk creeps wider and the eyes narrow further.
Row 2 — ANGRY: eyes slanted down toward the middle, flat frown.
Frames: the frown deepens slightly with a small tense tremble.
Row 3 — FURIOUS: sharply slanted eyes, deep frown, still cute never menacing.
Frames: a tight quivering shake — details shift a few pixels between frames.
Row 4 — SAD: outer-drooping eyes, soft downturned mouth.
Frames: eyes and mouth droop a little further, then lift slightly.
```

Sheet 4:
```
Row 1 — SLEEPING: fully closed eye arcs, tiny relaxed mouth, two small amber
Zs beside the face. Frames: the Zs drift upward and fade, breathing rhythm.
Row 2 — LISTENING: round eyes with enlarged pupils, small attentive mouth.
Frames: pupils pulse gently larger and smaller.
Row 3 — SPEAKING: bright engaged eyes. Frames: mouth closed → slightly open →
open oval → wide open (four mouth-opening amounts for lip sync).
Row 4 — LOVE: amber heart-shaped pupils, big delighted smile.
Frames: the hearts pulse like a heartbeat, smile bounces happily.
```

Negative prompt (where supported): human face, realistic, metallic, 3D render,
gradient, glow, bloom, text, watermark, eyebrows, nose, head outline.

## Variant B — loose brief, model decides the design

Same mechanical constraints, but the visual design is left open. Use this to
explore directions the detailed prompt would never produce, then fold the
winning look back into the Variant A structure.

```
A sprite sheet of animated faces for a cute little desk robot, arranged in a
clean 4×4 grid of 16 equal-sized tiles on a pure black background (#000000),
each tile 368×448 pixels. Each ROW is one facial expression; the four tiles in
a row are consecutive animation frames of that expression, left to right —
between frames only small details move (eyes, pupils, mouth, tiny accents), so
each row reads as one face in gentle motion, never a redesign. The face is
identically positioned in every tile so frames align when swapped.

Design the face however you like — it should feel cute, child-friendly,
colourful, and a little retro, with the charm of a vintage cartoon mascot
(think Miss Minutes from Loki). Keep it simple enough to read at a glance on a
small screen: just a face, no head, body, or background scenery.

Render in flat solid colours with crisp edges — no gradients, glow, bloom, or
soft shadows — using a small palette of a few colours at most.

The 4 expressions, one per row:
<ROWS — e.g. "Row 1: OPEN (attentive, ready to listen) · Row 2: BLINK (frames
are the stages of one blink) · Row 3: PEEK (sideways glance) · Row 4: WHAT?!
(surprised)">

No text, labels, tile borders, or watermarks. The design must stay identical
across all 16 tiles — only the expression and its animation change.
```

Row lists for the four sheets (loose wording):
1. OPEN (attentive) · BLINK (stages of one blink) · PEEK (sideways glance) ·
   WHAT?! (surprised)
2. DOZY (sleepy) · WORRIED · HAPPY · INTERESTED
3. DEVIOUS (playful smirk) · ANGRY · FURIOUS (still cute) · SAD
4. SLEEPING (drifting Zs) · LISTENING · SPEAKING (frames are four
   mouth-opening amounts, closed to wide) · LOVE (delighted)

## Device pipeline notes

- Generate at the largest available size, then downscale each sheet to exactly
  1472×1792 and slice into 368×448 tiles; row = expression, column = frame.
- Cross-sheet consistency: generate sheet 1 first, then pass its OPEN frame 1
  (or the whole sheet) as an image reference when generating sheets 2–4.
  Within-row drift matters most — frames swap rapidly; expression-to-expression
  changes are covered by a transition anyway.
- Playback: ping-pong frames 1→2→3→4→3→2 for idle loops. BLINK's four frames
  are one blink cycle. SPEAKING's four frames are amplitude buckets — map
  playback RMS to frame index for lip sync instead of looping.
- Flat colours + black background quantize cleanly to RGB565 and compress well
  (LVGL image converter, RLE-friendly). Bloom/gradients band badly — keep them
  out of the source art; any glow is the renderer's job.
