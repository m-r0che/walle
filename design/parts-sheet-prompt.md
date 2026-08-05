# Face parts sheet — generation prompt

Study 03. Supersedes the multi-frame animation sheets in
`sprite-sheet-prompt.md` for the hybrid renderer: static sprite parts blitted
into the 344×286 PSRAM face canvas, with pupils/gaze, blink occlusion, mouth
selection, and accents (Zs, hearts, waves) animated procedurally on top.

Attach the chosen character sheet (the cream/mint/orange robot head study)
as the image reference when running this prompt.

## Prompt

```
Using the attached robot face sprite sheet as the exact character design
reference — same palette, line weight, and proportions — create a PARTS SHEET
that decomposes this robot's face into separate reusable pieces for a game
engine. Lay the parts out on a pure black background (#000000) in loose rows
with generous empty space around every piece so each can be cropped cleanly.
Flat solid colours with crisp edges — no gradients, glow, bloom, or shadows.
No text, labels, or watermarks.

Row 1 — BASE HEAD, drawn once, large: the complete head with green bezel,
cream faceplate, orange side ears, orange antenna, and orange nose — but with
a completely BLANK faceplate: no eyes, no eyebrows, no mouth, no blush.

Row 2 — EYE WHITES (empty eyes, NO pupils): pairs of eye shapes matching the
reference expressions, drawn as outline and white fill only so a pupil can be
added later: wide-open round eyes, tall surprised ovals, half-lidded sleepy
eyes, worried eyes with drooping upper curve, narrowed devious eyes, angry
slanted eyes, glossy teary sad eyes.

Row 3 — CLOSED-EYE SHAPES: happy closed arc eyes, peaceful sleeping closed
curves, and plain eyelid shapes filled in faceplate cream for blink overlays.

Row 4 — PUPILS, drawn individually at matching scale: plain black oval pupil
with white glint in three sizes (small, medium, large), orange heart-shaped
pupil, orange-and-yellow flame pupil.

Row 5 — MOUTHS, drawn individually: gentle closed smile, wide cheerful smile,
small open smile, open oval talking mouth, wide-open talking mouth with
tongue, small surprised O, flat frown, deep angry open frown, soft sad
downturned mouth, lopsided smirk, tiny sleepy pout.

Row 6 — SEPARATE EYEBROW STROKES: neutral, raised curious, worried tilted,
angry slanted — drawn as individual strokes, not attached to eyes.

Row 7 — ACCENTS: single Z glyph in two sizes, small orange heart, sparkle,
three nested curved sound-wave arcs, and the small blush tick marks.

Every part must use the exact colours and stroke style of the reference sheet
so pieces composite seamlessly onto the base head. Draw all parts at a
consistent scale relative to the base head. Parts must never touch or overlap
each other.
```

## Eyes-only sheet

Focused follow-up when the eye set needs more coverage than the main parts
sheet. Attach both the 16-tile character sheet and the parts sheet as
references.

```
Using the attached robot face art as the exact character design reference,
create a sprite sheet of ONLY EYES for this robot, laid out on a pure black
background (#000000) in loose rows with generous empty space around every
piece so each can be cropped cleanly. Match the reference art's style,
colours, and line work exactly. Flat solid colours with crisp edges — no
gradients, glow, or shadows. No text, labels, or watermarks. Draw every eye
EMPTY: no pupil, so a pupil can be drawn in later. Where a shape is
asymmetric, draw the left-eye and right-eye versions as a mirrored pair.

Row 1 — open eye pairs: relaxed, wide alert, extra-wide surprised, gently
narrowed content.

Row 2 — emotional eye pairs: half-lidded sleepy, worried, devious narrowed,
angry slanted, sad teary.

Row 3 — blink sequence for one open eye, four stages side by side: fully
open, one-third closed by the upper eyelid, two-thirds closed, fully closed.

Row 4 — closed shapes: happy arc pair, peaceful sleeping pair, one
squeezed-shut scrunched pair.

Row 5 — pupils at matching scale: the robot's normal pupil in four sizes
from small to extra large, plus a heart pupil, a flame pupil, and a tiny
sparkle pupil.

All eyes drawn at a consistent scale relative to each other. Parts must
never touch or overlap.
```

## Notes

- Blush moved off the base head into accents so it can be toggled (e.g. LOVE,
  HAPPY) rather than always-on.
- Eyelid blink overlays are requested in faceplate cream, but procedurally
  drawing a cream shape over the eye region is the fallback if the generated
  ones don't align — the flat faceplate colour makes either work.
- Expect to run this 2–3 times and cherry-pick; parts composite in code, so
  per-part quality matters and layout drift does not. The original 16-tile
  sheet is itself a parts source — eyes and mouths can be cropped straight
  out of it and pupils removed by flood-filling cream/white.
- Slice manually or with a quick script; store parts as RGB565 (+ 1-bit or A8
  mask for the small dynamic pieces) in a flash partition, loaded to PSRAM at
  boot. Internal RAM must never hold sprite assets.
