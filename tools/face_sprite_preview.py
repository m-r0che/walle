#!/usr/bin/env python3
"""Preview the component sprite face in Walle's 448x368 logical screen."""
import json
import sys
from pathlib import Path
from typing import Iterable

from PIL import Image

FACE_W = 448
FACE_H = 368
SCALE = 1.05
PAD = 3
OFFSET_X = -5
OFFSET_Y = -9

LEFT_EYE = (147, 190)
RIGHT_EYE = (307, 190)
LEFT_BROW = (147, 143)
RIGHT_BROW = (307, 143)
MOUTH_X = 224
NOSE_CENTER = (224, 231)

EYE_OVERRIDES = {
    "eye_open_left": "eye_open_left",
    "eye_open_right": "eye_open_right",
    "eye_half_left": "eye_half_left",
    "eye_half_right": "eye_half_right",
    "eye_worried_left": "eye_worried_left",
    "eye_worried_right": "eye_worried_right",
    "eye_devious_left": "eye_devious_left",
    "eye_devious_right": "eye_devious_right",
    "eye_angry_left": "eye_angry_left",
    "eye_angry_right": "eye_angry_right",
    "eye_teary_left": "eye_teary_left",
    "eye_teary_right": "eye_teary_right",
    "eye_happy_closed_left": "eye_closed_happy_left",
    "eye_happy_closed_right": "eye_closed_happy_right",
    "eye_sleep_closed_left": "eye_closed_sleep_left",
    "eye_sleep_closed_right": "eye_closed_sleep_right",
    "pupil_small": "pupil_small",
    "pupil_medium": "pupil_medium",
    "pupil_large": "pupil_large",
    "pupil_heart": "pupil_heart",
}

ROOT = Path(__file__).resolve().parents[1]
PARTS_DIR = ROOT / "design" / "generated" / "face-parts"
EYE_PARTS_DIR = ROOT / "design" / "generated" / "eye-parts"
NAMES = json.loads((ROOT / "design" / "face-parts-names.json").read_text())["parts"]
EYE_NAMES = json.loads((ROOT / "design" / "eye-parts-names.json").read_text())["parts"]
MANIFEST = {item["name"]: item for item in json.loads((PARTS_DIR / "manifest.json").read_text())}


def remove_baked_orange_from_head(im: Image.Image) -> None:
    px = im.load()
    nose_x = 168
    nose_y = 173
    radius_x = 22
    radius_y = 22
    limit = radius_x * radius_x * radius_y * radius_y
    for y in range(nose_y - radius_y, nose_y + radius_y + 1):
        for x in range(nose_x - radius_x, nose_x + radius_x + 1):
            if 0 <= x < im.width and 0 <= y < im.height:
                dx = x - nose_x
                dy = y - nose_y
                if dx * dx * radius_y * radius_y + dy * dy * radius_x * radius_x <= limit:
                    px[x, y] = px[x, max(0, y - 36)]
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = px[x, y]
            if a > 0 and r > 160 and 45 < g < 190 and b < 100 and r - g > 45:
                px[x, y] = (0, 0, 0, 0)


def scale_for(logical_name: str) -> tuple[float, float]:
    if logical_name in {
        "eye_happy_closed_left", "eye_happy_closed_right",
        "eye_sleep_closed_left", "eye_sleep_closed_right",
    }:
        return 0.62, 0.62
    if logical_name.startswith("eye_"):
        return 0.98, 0.80
    if logical_name.startswith("pupil_"):
        if logical_name == "pupil_small":
            return 0.92, 0.92
        if logical_name == "pupil_medium":
            return 0.72, 0.72
        if logical_name == "pupil_large":
            return 0.58, 0.58
        if logical_name == "pupil_heart":
            return 0.58, 0.58
    if logical_name.startswith("mouth_"):
        return 0.58, 0.58
    return SCALE, SCALE


def part_image(logical_name: str) -> Image.Image:
    if logical_name in EYE_OVERRIDES:
        part_name = EYE_NAMES[EYE_OVERRIDES[logical_name]]
        im = Image.open(EYE_PARTS_DIR / f"{part_name}.png").convert("RGBA")
    else:
        part_name = NAMES[logical_name]
        im = Image.open(PARTS_DIR / f"{part_name}.png").convert("RGBA")
        if logical_name == "head_blank":
            remove_baked_orange_from_head(im)
    scale_x, scale_y = scale_for(logical_name)
    size = (max(1, round(im.width * scale_x)), max(1, round(im.height * scale_y)))
    return im.resize(size, Image.Resampling.LANCZOS)


def part_sheet_origin(logical_name: str) -> tuple[int, int]:
    part_name = NAMES[logical_name]
    x0, y0, _x1, _y1 = MANIFEST[part_name]["box"]
    return (OFFSET_X + round((x0 - PAD) * SCALE), OFFSET_Y + round((y0 - PAD) * SCALE))


def paste(canvas: Image.Image, logical_name: str, x: int, y: int) -> None:
    im = part_image(logical_name)
    canvas.alpha_composite(im, (x, y))


def paste_head(canvas: Image.Image) -> None:
    for name in ("head_blank", "ear_left", "ear_right"):
        paste(canvas, name, *part_sheet_origin(name))


def paste_center(canvas: Image.Image, name: str, center: tuple[int, int], dx: int = 0, dy: int = 0) -> None:
    im = part_image(name)
    canvas.alpha_composite(im, (round(center[0] - im.width / 2 + dx), round(center[1] - im.height / 2 + dy)))


def paste_eye_pair(canvas: Image.Image, left: str, right: str, pupils: str | None = "pupil_medium", gaze: tuple[int, int] = (0, 0)) -> None:
    paste_center(canvas, left, LEFT_EYE)
    paste_center(canvas, right, RIGHT_EYE)
    if pupils is not None:
        paste_center(canvas, pupils, LEFT_EYE, gaze[0], gaze[1] + 5)
        paste_center(canvas, pupils, RIGHT_EYE, gaze[0], gaze[1] + 5)


def paste_brows(canvas: Image.Image, left: str | None, right: str | None) -> None:
    if left:
        paste_center(canvas, left, LEFT_BROW)
    if right:
        paste_center(canvas, right, RIGHT_BROW)


def draw_blush_tick(canvas: Image.Image, x: int, y: int) -> None:
    orange = (255, 127, 24, 230)
    for step in range(12):
        cx = x + step // 3
        cy = y + step
        for dy in range(-1, 2):
            for dx in range(-1, 2):
                px = cx + dx
                py = cy + dy
                if 0 <= px < FACE_W and 0 <= py < FACE_H:
                    canvas.putpixel((px, py), orange)


def fill_ellipse(canvas: Image.Image, center: tuple[int, int], rx: int, ry: int, color: tuple[int, int, int, int]) -> None:
    px = canvas.load()
    cx, cy = center
    limit = rx * rx * ry * ry
    for yy in range(cy - ry, cy + ry + 1):
        for xx in range(cx - rx, cx + rx + 1):
            if 0 <= xx < FACE_W and 0 <= yy < FACE_H:
                if ((xx - cx) * (xx - cx)) * ry * ry + ((yy - cy) * (yy - cy)) * rx * rx <= limit:
                    px[xx, yy] = color


def paste_nose(canvas: Image.Image) -> None:
    fill_ellipse(canvas, NOSE_CENTER, 12, 12, (0, 0, 0, 255))
    fill_ellipse(canvas, NOSE_CENTER, 8, 8, (255, 127, 24, 255))


def paste_blush(canvas: Image.Image) -> None:
    for x in (92, 106, 120):
        draw_blush_tick(canvas, x, 247)
    for x in (318, 332, 346):
        draw_blush_tick(canvas, x, 247)


def paste_sound(canvas: Image.Image) -> None:
    paste_center(canvas, "accent_sound_wave_small", (416, 190))


def compose(pose: str) -> Image.Image:
    canvas = Image.new("RGBA", (FACE_W, FACE_H), (0, 0, 0, 255))
    paste_head(canvas)
    paste_nose(canvas)
    paste_blush(canvas)

    if pose == "warm":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right", "pupil_small")
        paste_center(canvas, "mouth_smile_gentle", (MOUTH_X, 272))
    elif pose == "happy":
        paste_eye_pair(canvas, "eye_happy_closed_left", "eye_happy_closed_right", None)
        paste_center(canvas, "mouth_smile_gentle", (MOUTH_X, 272))
    elif pose == "curious":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right", "pupil_small", gaze=(12, -2))
        paste_brows(canvas, "brow_raised_left", "brow_raised_right")
        paste_center(canvas, "mouth_surprised_o", (MOUTH_X, 273))
    elif pose == "listening":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right", "pupil_medium")
        paste_center(canvas, "mouth_smile_gentle", (MOUTH_X, 272))
        paste_sound(canvas)
    elif pose == "speaking":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right", "pupil_small")
        paste_center(canvas, "mouth_talk_wide", (MOUTH_X, 286))
    elif pose == "thinking":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right", "pupil_small", gaze=(10, -3))
        paste_center(canvas, "mouth_smile_gentle", (MOUTH_X, 272))
    elif pose == "concerned":
        paste_eye_pair(canvas, "eye_worried_left", "eye_worried_right", "pupil_small")
        paste_brows(canvas, "brow_raised_left", "brow_raised_right")
        paste_center(canvas, "mouth_sad_soft", (MOUTH_X, 276))
    elif pose == "sleeping":
        paste_eye_pair(canvas, "eye_sleep_closed_left", "eye_sleep_closed_right", None)
        paste_center(canvas, "mouth_sleepy_pout", (MOUTH_X, 276))
        paste_center(canvas, "accent_z_large", (62, 62))
        paste_center(canvas, "accent_z_small", (113, 39))
    else:
        raise SystemExit(f"unknown pose: {pose}")
    return canvas


def montage(poses: Iterable[str]) -> Image.Image:
    pose_list = list(poses)
    sheet = Image.new("RGBA", (FACE_W * 2, FACE_H * ((len(pose_list) + 1) // 2)), (0, 0, 0, 255))
    for index, pose in enumerate(pose_list):
        sheet.alpha_composite(compose(pose), ((index % 2) * FACE_W, (index // 2) * FACE_H))
    return sheet


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "design" / "generated" / "face-sprite-preview.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    montage(["warm", "listening", "thinking", "speaking", "curious", "happy", "concerned", "sleeping"]).save(out)
    print(out)


if __name__ == "__main__":
    main()
