#!/usr/bin/env python3
"""Generate scaled RGB565+A8 firmware sprites from the named face parts."""
import json
import re
import sys
from pathlib import Path

from PIL import Image

SCALE = 0.80
PAD = 3
OFFSET_X = 61
OFFSET_Y = 53

SELECTED = [
    "head_blank", "antenna", "ear_left", "ear_right",
    "eye_open_left", "eye_open_right",
    "eye_half_left", "eye_half_right",
    "eye_worried_left", "eye_worried_right",
    "eye_devious_left", "eye_devious_right",
    "eye_angry_left", "eye_angry_right",
    "eye_teary_left", "eye_teary_right",
    "eye_happy_closed_left", "eye_happy_closed_right",
    "eye_sleep_closed_left", "eye_sleep_closed_right",
    "pupil_small", "pupil_medium", "pupil_large", "pupil_heart",
    "mouth_smile_gentle", "mouth_smile_wide", "mouth_open_small",
    "mouth_talk_oval", "mouth_talk_wide", "mouth_surprised_o",
    "mouth_frown_flat", "mouth_frown_angry_open", "mouth_sad_soft",
    "mouth_smirk", "mouth_sleepy_pout",
    "brow_raised_left", "brow_raised_right", "brow_soft_left",
    "brow_soft_right", "brow_slant_left", "brow_slant_right",
    "accent_z_large", "accent_z_small", "accent_sound_wave_small",
]


def c_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", name).upper()


def rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def wrap(values, width=12):
    out = []
    for i in range(0, len(values), width):
        out.append("    " + ", ".join(values[i:i + width]) + ",")
    return "\n".join(out)


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    names = json.loads((root / "design" / "face-parts-names.json").read_text())["parts"]
    manifest = {
        item["name"]: item
        for item in json.loads((root / "design" / "generated" / "face-parts" / "manifest.json").read_text())
    }
    parts_dir = root / "design" / "generated" / "face-parts"
    out_h = root / "firmware" / "main" / "face_sprite_assets.h"
    out_c = root / "firmware" / "main" / "face_sprite_assets.c"

    enum_lines = []
    c_chunks = []
    table_lines = []
    for index, logical in enumerate(SELECTED):
        part = names[logical]
        source = Image.open(parts_dir / f"{part}.png").convert("RGBA")
        scale_x = SCALE
        scale_y = SCALE
        if logical.startswith("pupil_"):
            scale_x = 0.62
            scale_y = 0.78
        elif logical.startswith("mouth_"):
            scale_x = 0.58
            scale_y = 0.58
        size = (max(1, round(source.width * scale_x)),
                max(1, round(source.height * scale_y)))
        im = source.resize(size, Image.Resampling.LANCZOS)
        pixels = []
        alpha = []
        for _y in range(im.height):
            for x in range(im.width):
                r, g, b, a = im.getpixel((x, _y))
                pixels.append(f"0x{rgb565(r, g, b):04x}")
                alpha.append(str(a))
        symbol = f"s_{logical}"
        enum_name = f"FACE_SPRITE_{c_name(logical)}"
        enum_lines.append(f"    {enum_name} = {index},")
        c_chunks.append(
            f"static const uint16_t {symbol}_pixels[] = {{\n{wrap(pixels)}\n}};\n"
            f"static const uint8_t {symbol}_alpha[] = {{\n{wrap(alpha, 20)}\n}};\n")
        x0, y0, _x1, _y1 = manifest[part]["box"]
        origin_x = OFFSET_X + round((x0 - PAD) * SCALE)
        origin_y = OFFSET_Y + round((y0 - PAD) * SCALE)
        table_lines.append(
            f"    [{enum_name}] = {{ {im.width}, {im.height}, {origin_x}, {origin_y}, "
            f"{symbol}_pixels, {symbol}_alpha }},")

    out_h.write_text("""#pragma once

#include <stdint.h>

typedef enum {
%s
    FACE_SPRITE_COUNT,
} face_sprite_id_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    int16_t sheet_x;
    int16_t sheet_y;
    const uint16_t *pixels;
    const uint8_t *alpha;
} face_sprite_asset_t;

extern const face_sprite_asset_t g_face_sprites[FACE_SPRITE_COUNT];
""" % "\n".join(enum_lines))

    out_c.write_text("""#include "face_sprite_assets.h"

%s
const face_sprite_asset_t g_face_sprites[FACE_SPRITE_COUNT] = {
%s
};
""" % ("\n".join(c_chunks), "\n".join(table_lines)))
    print(out_h)
    print(out_c)


if __name__ == "__main__":
    main()
