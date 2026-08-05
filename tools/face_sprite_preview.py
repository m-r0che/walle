#!/usr/bin/env python3
"""Preview the component sprite face in Walle's 448x368 logical screen.

This preview uses the same large, no-clipping placement as firmware: the body
fills most of the existing 344x286 face canvas, while mouths are independently
scaled smaller to preserve the composite-reference expression style.
"""
import json
import sys
from pathlib import Path
from typing import Iterable

from PIL import Image

FACE_W = 448
FACE_H = 368
SCALE = 0.89
PAD = 3
OFFSET_X = 26
OFFSET_Y = 23

LEFT_EYE = (155, 213)
RIGHT_EYE = (291, 213)
LEFT_BROW = (155, 166)
RIGHT_BROW = (291, 166)
MOUTH_X = 224

ROOT = Path(__file__).resolve().parents[1]
PARTS_DIR = ROOT / "design" / "generated" / "face-parts"
NAMES = json.loads((ROOT / "design" / "face-parts-names.json").read_text())["parts"]
MANIFEST = {item["name"]: item for item in json.loads((PARTS_DIR / "manifest.json").read_text())}


def part_image(logical_name: str) -> Image.Image:
    part_name = NAMES[logical_name]
    im = Image.open(PARTS_DIR / f"{part_name}.png").convert("RGBA")
    scale_x = SCALE
    scale_y = SCALE
    if logical_name.startswith("eye_"):
        scale_x = 1.16
        scale_y = 0.98
    elif logical_name.startswith("pupil_"):
        scale_x = 0.92
        scale_y = 0.95
    elif logical_name.startswith("mouth_"):
        scale_x = 0.58
        scale_y = 0.58
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
    for name in ("head_blank", "antenna", "ear_left", "ear_right"):
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


def paste_blush(canvas: Image.Image) -> None:
    for x in (108, 120, 132):
        draw_blush_tick(canvas, x, 269)
    for x in (307, 319, 331):
        draw_blush_tick(canvas, x, 269)


def paste_sound(canvas: Image.Image) -> None:
    paste_center(canvas, "accent_sound_wave_small", (394, 213))


def compose(pose: str) -> Image.Image:
    canvas = Image.new("RGBA", (FACE_W, FACE_H), (0, 0, 0, 255))
    paste_head(canvas)
    paste_blush(canvas)

    if pose == "warm":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right", "pupil_small")
        paste_center(canvas, "mouth_smile_gentle", (MOUTH_X, 279))
    elif pose == "happy":
        paste_eye_pair(canvas, "eye_happy_closed_left", "eye_happy_closed_right", None)
        paste_center(canvas, "mouth_smile_gentle", (MOUTH_X, 279))
    elif pose == "curious":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right", "pupil_small", gaze=(12, -2))
        paste_brows(canvas, "brow_raised_left", "brow_raised_right")
        paste_center(canvas, "mouth_surprised_o", (MOUTH_X, 280))
    elif pose == "listening":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right", "pupil_medium")
        paste_center(canvas, "mouth_smile_gentle", (MOUTH_X, 279))
        paste_sound(canvas)
    elif pose == "speaking":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right", "pupil_small")
        paste_center(canvas, "mouth_talk_wide", (MOUTH_X, 291))
    elif pose == "thinking":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right", "pupil_small", gaze=(10, -3))
        paste_center(canvas, "mouth_smile_gentle", (MOUTH_X, 279))
    elif pose == "concerned":
        paste_eye_pair(canvas, "eye_worried_left", "eye_worried_right", "pupil_small")
        paste_brows(canvas, "brow_raised_left", "brow_raised_right")
        paste_center(canvas, "mouth_sad_soft", (MOUTH_X, 282))
    elif pose == "sleeping":
        paste_eye_pair(canvas, "eye_sleep_closed_left", "eye_sleep_closed_right", None)
        paste_center(canvas, "mouth_sleepy_pout", (MOUTH_X, 282))
        paste_center(canvas, "accent_z_large", (61, 86))
        paste_center(canvas, "accent_z_small", (105, 65))
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
