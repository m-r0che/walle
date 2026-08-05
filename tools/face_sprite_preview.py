#!/usr/bin/env python3
"""Preview the parts-sheet robot face at Walle's device canvas scale.

This is a design-side placement tool, not firmware. It uses the named sliced
parts and composes a few canonical poses into a 448x368 logical screen so the
sprite anchors can be tuned before generating C assets.
"""
import json
import sys
from pathlib import Path
from typing import Iterable

from PIL import Image

FACE_W = 448
FACE_H = 368
CANVAS_X = 52
CANVAS_Y = 39
CANVAS_W = 344
CANVAS_H = 286
SCALE = 0.89
PAD = 3

ROOT = Path(__file__).resolve().parents[1]
PARTS_DIR = ROOT / "design" / "generated" / "face-parts"
NAMES = json.loads((ROOT / "design" / "face-parts-names.json").read_text())[
    "parts"
]
MANIFEST = {
    item["name"]: item
    for item in json.loads((PARTS_DIR / "manifest.json").read_text())
}

HEAD_EXTENTS = {
    "min_x": 29,
    "min_y": 18,
}
OFFSET_X = CANVAS_X - int(round(HEAD_EXTENTS["min_x"] * SCALE))
OFFSET_Y = CANVAS_Y - int(round(HEAD_EXTENTS["min_y"] * SCALE))


def part_image(logical_name: str) -> Image.Image:
    part_name = NAMES[logical_name]
    path = PARTS_DIR / f"{part_name}.png"
    im = Image.open(path).convert("RGBA")
    if SCALE != 1.0:
        size = (max(1, round(im.width * SCALE)),
                max(1, round(im.height * SCALE)))
        im = im.resize(size, Image.Resampling.LANCZOS)
    return im


def part_sheet_origin(logical_name: str) -> tuple[int, int]:
    part_name = NAMES[logical_name]
    x0, y0, _x1, _y1 = MANIFEST[part_name]["box"]
    return (OFFSET_X + int(round((x0 - PAD) * SCALE)),
            OFFSET_Y + int(round((y0 - PAD) * SCALE)))


def paste(canvas: Image.Image, logical_name: str, x: int, y: int) -> None:
    im = part_image(logical_name)
    canvas.alpha_composite(im, (x, y))


def paste_head(canvas: Image.Image) -> None:
    for name in ("head_blank", "antenna", "ear_left", "ear_right"):
        paste(canvas, name, *part_sheet_origin(name))


def paste_center(canvas: Image.Image, name: str, center: tuple[int, int],
                 dx: int = 0, dy: int = 0) -> None:
    im = part_image(name)
    x = int(round(center[0] - im.width / 2 + dx))
    y = int(round(center[1] - im.height / 2 + dy))
    canvas.alpha_composite(im, (x, y))


def paste_eye_pair(canvas: Image.Image, left: str, right: str,
                   pupils: str | None = "pupil_medium",
                   gaze: tuple[int, int] = (0, 0)) -> None:
    left_center = (138, 165)
    right_center = (310, 165)
    paste_center(canvas, left, left_center)
    paste_center(canvas, right, right_center)
    if pupils is not None:
        paste_center(canvas, pupils, left_center, gaze[0], gaze[1] + 5)
        paste_center(canvas, pupils, right_center, gaze[0], gaze[1] + 5)


def paste_brows(canvas: Image.Image, left: str | None, right: str | None) -> None:
    if left:
        paste_center(canvas, left, (138, 129))
    if right:
        paste_center(canvas, right, (310, 129))


def paste_sound(canvas: Image.Image) -> None:
    paste_center(canvas, "accent_sound_wave_small", (384, 155))


def compose(pose: str) -> Image.Image:
    canvas = Image.new("RGBA", (FACE_W, FACE_H), (0, 0, 0, 255))
    paste_head(canvas)

    if pose == "warm":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right",
                       "pupil_small")
        paste_center(canvas, "mouth_smile_gentle", (224, 246))
    elif pose == "happy":
        paste_eye_pair(canvas, "eye_happy_closed_left",
                       "eye_happy_closed_right", None)
        paste_center(canvas, "mouth_smile_wide", (224, 246))
    elif pose == "curious":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right",
                       "pupil_small", gaze=(15, -2))
        paste_brows(canvas, "brow_raised_left", "brow_raised_right")
        paste_center(canvas, "mouth_surprised_o", (224, 248))
    elif pose == "listening":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right",
                       "pupil_medium")
        paste_center(canvas, "mouth_smile_gentle", (224, 246))
        paste_sound(canvas)
    elif pose == "speaking":
        paste_eye_pair(canvas, "eye_open_left", "eye_open_right",
                       "pupil_small")
        paste_center(canvas, "mouth_talk_wide", (224, 248))
    elif pose == "thinking":
        paste_eye_pair(canvas, "eye_devious_left", "eye_devious_right",
                       "pupil_small", gaze=(10, -3))
        paste_brows(canvas, "brow_soft_left", "brow_soft_right")
        paste_center(canvas, "mouth_smirk", (224, 250))
    elif pose == "concerned":
        paste_eye_pair(canvas, "eye_worried_left", "eye_worried_right",
                       "pupil_small")
        paste_brows(canvas, "brow_raised_left", "brow_raised_right")
        paste_center(canvas, "mouth_sad_soft", (224, 250))
    elif pose == "sleeping":
        paste_eye_pair(canvas, "eye_sleep_closed_left",
                       "eye_sleep_closed_right", None)
        paste_center(canvas, "mouth_sleepy_pout", (224, 248))
        paste_center(canvas, "accent_z_large", (65, 106))
        paste_center(canvas, "accent_z_small", (106, 83))
    else:
        raise SystemExit(f"unknown pose: {pose}")
    return canvas


def montage(poses: Iterable[str]) -> Image.Image:
    pose_list = list(poses)
    thumb_w, thumb_h = FACE_W, FACE_H
    sheet = Image.new("RGBA", (thumb_w * 2, thumb_h * ((len(pose_list) + 1) // 2)),
                      (0, 0, 0, 255))
    for index, pose in enumerate(pose_list):
        im = compose(pose)
        sheet.alpha_composite(im, ((index % 2) * thumb_w,
                                   (index // 2) * thumb_h))
    return sheet


def main() -> None:
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "design" / "generated" / "face-sprite-preview.png"
    out.parent.mkdir(parents=True, exist_ok=True)
    poses = ["warm", "listening", "thinking", "speaking",
             "curious", "happy", "concerned", "sleeping"]
    montage(poses).save(out)
    print(out)


if __name__ == "__main__":
    main()
