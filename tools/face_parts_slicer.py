#!/usr/bin/env python3
"""Slice a face parts sheet (flat art on black) into alpha-masked part PNGs.

Parts must be closed shapes that never touch each other. Background is any
dark region reachable from the image border, so black fills inside closed
outlines (mouth interiors, pupils) are preserved. Emits part_NN.png crops,
an annotated overview, and a manifest.json with bounding boxes for naming.

Usage: face_parts_slicer.py <sheet.png> <output_dir>
"""
import json
import os
import sys
from collections import deque

from PIL import Image, ImageDraw

DARK_SUM = 90      # r+g+b at or below this counts as background-black
MIN_PIXELS = 150   # drop stray specks smaller than this
PAD = 3            # transparent padding around each crop


def main(sheet_path, out_dir):
    im = Image.open(sheet_path).convert('RGB')
    w, h = im.size
    px = im.load()

    bg = [[False] * w for _ in range(h)]
    q = deque()
    for x in range(w):
        q.append((x, 0)); q.append((x, h - 1))
    for y in range(h):
        q.append((0, y)); q.append((w - 1, y))
    while q:
        x, y = q.popleft()
        if x < 0 or y < 0 or x >= w or y >= h or bg[y][x]:
            continue
        r, g, b = px[x, y]
        if r + g + b > DARK_SUM:
            continue
        bg[y][x] = True
        q.extend([(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)])

    label = [[0] * w for _ in range(h)]
    comps = []
    for sy in range(h):
        for sx in range(w):
            if bg[sy][sx] or label[sy][sx]:
                continue
            cid = len(comps) + 1
            q = deque([(sx, sy)])
            label[sy][sx] = cid
            minx, miny, maxx, maxy, n = sx, sy, sx, sy, 0
            while q:
                x, y = q.popleft()
                n += 1
                minx, maxx = min(minx, x), max(maxx, x)
                miny, maxy = min(miny, y), max(maxy, y)
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        nx, ny = x + dx, y + dy
                        if 0 <= nx < w and 0 <= ny < h \
                                and not bg[ny][nx] and not label[ny][nx]:
                            label[ny][nx] = cid
                            q.append((nx, ny))
            comps.append({'id': cid, 'box': [minx, miny, maxx, maxy], 'n': n})

    comps = [c for c in comps if c['n'] >= MIN_PIXELS]
    comps.sort(key=lambda c: (c['box'][1] // 120, c['box'][0]))

    os.makedirs(out_dir, exist_ok=True)
    annotated = im.copy()
    draw = ImageDraw.Draw(annotated)
    manifest = []
    for i, c in enumerate(comps):
        x0, y0, x1, y1 = c['box']
        crop = im.crop((x0 - PAD, y0 - PAD, x1 + PAD + 1, y1 + PAD + 1)).convert('RGBA')
        cp = crop.load()
        for yy in range(crop.height):
            for xx in range(crop.width):
                gx, gy = x0 - PAD + xx, y0 - PAD + yy
                if not (0 <= gx < w and 0 <= gy < h) or bg[gy][gx]:
                    cp[xx, yy] = (0, 0, 0, 0)
        name = f'part_{i:02d}'
        crop.save(os.path.join(out_dir, f'{name}.png'))
        manifest.append({'name': name, 'box': c['box'], 'pixels': c['n']})
        draw.rectangle([x0 - 2, y0 - 2, x1 + 2, y1 + 2], outline=(255, 0, 255), width=2)
        draw.text((x0, max(0, y0 - 16)), str(i), fill=(255, 0, 255))

    annotated.save(os.path.join(out_dir, 'annotated.png'))
    with open(os.path.join(out_dir, 'manifest.json'), 'w') as f:
        json.dump(manifest, f, indent=2)
    print(f'{len(comps)} parts -> {out_dir}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
