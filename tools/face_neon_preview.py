#!/usr/bin/env python3
"""Render still frames of Walle's procedural neon face off-device.

This ports the RGB565 glow rasterizer in ``firmware/main/face.c`` (centred
344x286 canvas, three-pass glow, per-channel max blending, dimmed colour
packing, integer point truncation) so a palette or geometry change can be
reviewed before anything is flashed. The frames are hand-picked stills; they
do not run the firmware's animated pose state machine.

Example:

    tools/face_neon_preview.py --palette pink \
        --output design/generated/face-neon-pink-preview.png
    tools/face_neon_preview.py --compare cyan pink \
        --output design/generated/face-neon-cyan-vs-pink.png
"""
import argparse
import math
from pathlib import Path

from PIL import Image

FACE_W = 448
FACE_H = 368
CANVAS_W = 344
CANVAS_H = 286
CANVAS_X = (FACE_W - CANVAS_W) // 2
CANVAS_Y = 39
EYE_POINTS = 17
BROW_POINTS = 9
MOUTH_POINTS = 13

# Mirrors the raster_color_t values in face.c: (deep glow, ice core).
PALETTES = {
    "pink": ((0x75, 0x00, 0x3B), (0xFF, 0xB9, 0xDC)),
    "cyan": ((0x00, 0x67, 0x75), (0xB9, 0xFF, 0xFF)),
}

NEUTRAL_POSE = {
    "left_eye_open": 0.99,
    "right_eye_open": 0.94,
    "left_eye_scale": 1.0,
    "right_eye_scale": 1.0,
    "gaze_x": 0.0,
    "gaze_y": 0.0,
    "pupil_scale": 1.0,
    "left_brow_lift": 0.08,
    "right_brow_lift": 0.14,
    "left_brow_angle": -0.02,
    "right_brow_angle": 0.04,
    "smile": 0.24,
    "mouth_open": 0.0,
    "mouth_width": 1.0,
    "tilt": -0.006,
}

FRAMES = [
    ("idle", {}),
    ("speaking", {"mouth_open": 0.7, "smile": 0.35, "pupil_scale": 1.05,
                  "gaze_x": 0.15}),
    ("sleeping", {"left_eye_open": 0.04, "right_eye_open": 0.04,
                  "smile": 0.30, "left_brow_lift": -0.1,
                  "right_brow_lift": -0.1}),
]


def trunc(value: float) -> int:
    """Match a C cast from float to int32_t."""
    return int(value)


def pack_dimmed(color, brightness: int) -> int:
    red, green, blue = color
    r5 = ((red >> 3) * brightness + 127) // 255
    g6 = ((green >> 2) * brightness + 127) // 255
    b5 = ((blue >> 3) * brightness + 127) // 255
    return (r5 << 11) | (g6 << 5) | b5


def rgb565_to_rgb888(pixel: int):
    r5 = (pixel >> 11) & 0x1F
    g6 = (pixel >> 5) & 0x3F
    b5 = pixel & 0x1F
    return ((r5 * 255 + 15) // 31, (g6 * 255 + 31) // 63, (b5 * 255 + 15) // 31)


class Canvas:
    def __init__(self, palette):
        self.deep, self.ice = palette
        self.pixels = [0] * (CANVAS_W * CANVAS_H)

    def brighten(self, x: int, y: int, source: int) -> None:
        x -= CANVAS_X
        y -= CANVAS_Y
        if not (0 <= x < CANVAS_W and 0 <= y < CANVAS_H):
            return
        index = y * CANVAS_W + x
        dest = self.pixels[index]
        red = max((source >> 11) & 0x1F, (dest >> 11) & 0x1F)
        green = max((source >> 5) & 0x3F, (dest >> 5) & 0x3F)
        blue = max(source & 0x1F, dest & 0x1F)
        self.pixels[index] = (red << 11) | (green << 5) | blue

    def disc(self, cx: int, cy: int, radius: int, color: int) -> None:
        limit = radius * radius
        for y in range(-radius, radius + 1):
            for x in range(-radius, radius + 1):
                if x * x + y * y <= limit:
                    self.brighten(cx + x, cy + y, color)

    def segment(self, start, end, radius: int, color: int) -> None:
        x0, y0 = start
        dx = end[0] - x0
        dy = end[1] - y0
        steps = max(abs(dx), abs(dy))
        stride = max(1, radius // 2)
        if steps == 0:
            self.disc(x0, y0, radius, color)
            return
        step = 0
        while step <= steps:
            self.disc(x0 + c_div(dx * step, steps), y0 + c_div(dy * step, steps),
                      radius, color)
            step += stride
        self.disc(end[0], end[1], radius, color)

    def glow_curve(self, points, strong: bool) -> None:
        radii = (10, 6, 3) if strong else (7, 4, 2)
        brightness = (34, 102, 255)
        for pass_index in range(3):
            source = self.ice if pass_index == 2 else self.deep
            color = pack_dimmed(source, brightness[pass_index])
            for index in range(1, len(points)):
                self.segment(points[index - 1], points[index],
                             radii[pass_index], color)

    def to_image(self) -> Image.Image:
        screen = Image.new("RGB", (FACE_W, FACE_H), (0, 0, 0))
        canvas = Image.new("RGB", (CANVAS_W, CANVAS_H))
        canvas.putdata([rgb565_to_rgb888(p) for p in self.pixels])
        screen.paste(canvas, (CANVAS_X, CANVAS_Y))
        return screen


def c_div(numerator: int, denominator: int) -> int:
    """Integer division truncating toward zero, as C does."""
    quotient = abs(numerator) // abs(denominator)
    return quotient if (numerator >= 0) == (denominator > 0) else -quotient


def transform_point(point, tilt: float, vertical_offset: float):
    center_x = FACE_W / 2.0
    center_y = 170.0
    x = point[0] - center_x
    y = point[1] + vertical_offset - center_y
    sine = math.sin(tilt)
    cosine = math.cos(tilt)
    return (trunc(center_x + x * cosine - y * sine),
            trunc(center_y + x * sine + y * cosine))


def transform_points(points, tilt: float, vertical_offset: float):
    return [transform_point(p, tilt, vertical_offset) for p in points]


def build_eye_curves(center_x, center_y, radius, openness):
    half_width = radius
    half_height = radius * openness
    top = []
    bottom = []
    for index in range(EYE_POINTS):
        t = index / (EYE_POINTS - 1)
        upper = math.pi - t * math.pi
        lower = math.pi + t * math.pi
        top.append((trunc(center_x + math.cos(upper) * half_width),
                    trunc(center_y - math.sin(upper) * half_height)))
        bottom.append((trunc(center_x + math.cos(lower) * half_width),
                       trunc(center_y - math.sin(lower) * half_height)))
    return top, bottom


def draw_eye(canvas: Canvas, center_x, center_y, openness, scale, gaze_x,
             gaze_y, pupil_scale, tilt, vertical_offset) -> None:
    radius = 49.0 * scale
    top, bottom = build_eye_curves(center_x, center_y, radius,
                                   max(0.025, openness))
    canvas.glow_curve(transform_points(top, tilt, vertical_offset), True)
    canvas.glow_curve(transform_points(bottom, tilt, vertical_offset), True)
    if openness < 0.16 or pupil_scale <= 0.0:
        return
    pupil = (trunc(center_x + gaze_x * 32.0),
             trunc(center_y + gaze_y * 22.0 * min(max(openness, 0.3), 1.0)))
    pupil = transform_point(pupil, tilt, vertical_offset)
    halo_radius = max(6, trunc(13.0 * pupil_scale))
    core_radius = max(4, trunc(7.0 * pupil_scale))
    canvas.disc(pupil[0], pupil[1], halo_radius, pack_dimmed(canvas.deep, 125))
    canvas.disc(pupil[0], pupil[1], core_radius, pack_dimmed(canvas.ice, 255))


def draw_brow(canvas: Canvas, center_x, lift, angle, tilt,
              vertical_offset) -> None:
    points = []
    for index in range(BROW_POINTS):
        t = index / (BROW_POINTS - 1)
        x_normal = t * 2.0 - 1.0
        arch = math.sin(t * math.pi)
        points.append((trunc(center_x + x_normal * 42.0),
                       trunc(86.0 - lift * 18.0 + angle * x_normal * 17.0
                             - arch * 5.0)))
    canvas.glow_curve(transform_points(points, tilt, vertical_offset), False)


def smoothstep(value: float) -> float:
    value = min(max(value, 0.0), 1.0)
    return value * value * (3.0 - 2.0 * value)


def draw_mouth(canvas: Canvas, pose, vertical_offset) -> None:
    center = []
    for index in range(MOUTH_POINTS):
        t = index / (MOUTH_POINTS - 1)
        x_normal = t * 2.0 - 1.0
        center.append((trunc(FACE_W / 2.0 + x_normal * 52.0 * pose["mouth_width"]),
                       trunc(260.0 + math.sin(t * math.pi) * 40.0 * pose["smile"])))
    tilt = pose["tilt"]
    if pose["mouth_open"] < 0.045:
        canvas.glow_curve(transform_points(center, tilt, vertical_offset), True)
        return
    opening = 3.0 + smoothstep(pose["mouth_open"]) * 18.0
    upper = []
    lower = []
    for index, (x, y) in enumerate(center):
        t = index / (MOUTH_POINTS - 1)
        arch = math.sin(t * math.pi)
        upper.append((x, y - trunc(arch * opening * 0.65)))
        lower.append((x, y + trunc(arch * opening)))
    canvas.glow_curve(transform_points(upper, tilt, vertical_offset), True)
    canvas.glow_curve(transform_points(lower, tilt, vertical_offset), True)


def draw_sleep_z(canvas: Canvas, x: int, y: int, size: int) -> None:
    canvas.glow_curve([(x, y), (x + size, y), (x, y + size),
                       (x + size, y + size)], False)


def render_frame(palette, name: str, overrides) -> Image.Image:
    pose = dict(NEUTRAL_POSE)
    pose.update(overrides)
    canvas = Canvas(palette)
    breath = 0.0
    draw_brow(canvas, 132.0, pose["left_brow_lift"], pose["left_brow_angle"],
              pose["tilt"], breath)
    draw_brow(canvas, 316.0, pose["right_brow_lift"], pose["right_brow_angle"],
              pose["tilt"], breath)
    draw_eye(canvas, 132.0, 154.0, pose["left_eye_open"], pose["left_eye_scale"],
             pose["gaze_x"], pose["gaze_y"], pose["pupil_scale"], pose["tilt"],
             breath)
    draw_eye(canvas, 316.0, 154.0, pose["right_eye_open"],
             pose["right_eye_scale"], pose["gaze_x"], pose["gaze_y"],
             pose["pupil_scale"], pose["tilt"], breath)
    draw_mouth(canvas, pose, breath * 0.65)
    if name == "sleeping":
        draw_sleep_z(canvas, 358, 154, 13)
        draw_sleep_z(canvas, 379, 118, 9)
    return canvas.to_image()


def render_strip(palette_name: str) -> Image.Image:
    palette = PALETTES[palette_name]
    strip = Image.new("RGB", (FACE_W * len(FRAMES), FACE_H), (0, 0, 0))
    for index, (name, overrides) in enumerate(FRAMES):
        strip.paste(render_frame(palette, name, overrides), (index * FACE_W, 0))
    return strip


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--palette", choices=sorted(PALETTES), default="pink")
    parser.add_argument("--compare", nargs=2, metavar=("TOP", "BOTTOM"),
                        choices=sorted(PALETTES),
                        help="stack two palettes vertically instead")
    parser.add_argument("--scale", type=int, default=1,
                        help="nearest-neighbour upscale factor")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if args.compare:
        top = render_strip(args.compare[0])
        bottom = render_strip(args.compare[1])
        image = Image.new("RGB", (top.width, top.height * 2), (0, 0, 0))
        image.paste(top, (0, 0))
        image.paste(bottom, (0, top.height))
    else:
        image = render_strip(args.palette)
    if args.scale > 1:
        image = image.resize((image.width * args.scale, image.height * args.scale),
                             Image.NEAREST)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    image.save(args.output)
    print(f"wrote {args.output} ({image.width}x{image.height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
