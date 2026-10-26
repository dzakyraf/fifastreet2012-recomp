#!/usr/bin/env python3
"""Lays a run's screenshots (runtime/gpu/vulkan/screenshot.h) out on one sheet.

usage: shots_sheet.py <shots dir> [out.png] [--from SEC] [--to SEC] [--cols N] [--width PX]

Each picture is labelled with its time, so the sheet reads as the run's timeline.
"""
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw


def main():
    args = sys.argv[1:]
    opts = {"--from": 0.0, "--to": 1e9, "--cols": 4, "--width": 400}
    rest = []
    i = 0
    while i < len(args):
        if args[i] in opts:
            opts[args[i]] = type(opts[args[i]])(args[i + 1])
            i += 2
        else:
            rest.append(args[i])
            i += 1
    if not rest:
        sys.exit(__doc__)
    shots = Path(rest[0])
    out = Path(rest[1]) if len(rest) > 1 else shots / "sheet.png"

    pictures = []
    for path in sorted(shots.glob("shot_*.png")):
        m = re.match(r"shot_(\d+\.\d)s_(\w+)\.png", path.name)
        if m and opts["--from"] <= float(m.group(1)) <= opts["--to"]:
            pictures.append((float(m.group(1)), m.group(2), path))
    if not pictures:
        sys.exit("no shots in range")

    cols, width = opts["--cols"], opts["--width"]
    first = Image.open(pictures[0][2])
    height = width * first.height // first.width
    label = 18
    rows = (len(pictures) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * width, rows * (height + label)), "black")
    draw = ImageDraw.Draw(sheet)
    for n, (sec, why, path) in enumerate(pictures):
        x, y = (n % cols) * width, (n // cols) * (height + label)
        sheet.paste(Image.open(path).convert("RGB").resize((width, height)), (x, y + label))
        draw.text((x + 4, y + 3), f"{sec:.1f} s  {why}", fill="yellow")
    sheet.save(out)
    print(out)


if __name__ == "__main__":
    main()
