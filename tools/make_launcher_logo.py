#!/usr/bin/env python3
"""Draws the launcher's wordmark (res/drawable-nodpi/launcher_logo.webp): FIFA STREET, a red rule and
"RECOMPILED FOR ANDROID", on a transparent background for the launcher's dark page.

usage: tools/make_launcher_logo.py            (writes the file in place)
Fonts: Arial Black (macOS) for the name, third_party/fonts/BarlowCondensed-Medium.ttf for the rest."""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "android/app/src/main/res/drawable-nodpi/launcher_logo.webp")
W, H, S = 842, 199, 3                      # the size the layout expects; drawn at 3x and shrunk
WHITE, BLUE, RED = (246, 238, 232, 255), (47, 168, 255, 255), (255, 59, 47, 255)

NAME = "/System/Library/Fonts/Supplemental/Arial Black.ttf"
SUB = os.path.join(ROOT, "third_party/fonts/BarlowCondensed-Medium.ttf")


def tracked(draw, xy, text, font, fill, tracking):
    """Text with extra space between the letters."""
    x, y = xy
    for ch in text:
        draw.text((x, y), ch, font=font, fill=fill)
        x += draw.textlength(ch, font=font) + tracking
    return x


def italic(layer, slant=0.22):
    """Shears a layer to the right, about its bottom edge."""
    w, h = layer.size
    return layer.transform((w + int(h * slant), h), Image.AFFINE, (1, slant, -h * slant, 0, 1, 0), Image.BICUBIC)


img = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))

# FIFA over STREET, sheared as one block, with a dark shadow so it reads over the artwork.
name = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
d = ImageDraw.Draw(name)
big = ImageFont.truetype(NAME, 96 * S)
d.text((8 * S, -10 * S), "FIFA", font=big, fill=WHITE)
d.text((8 * S, 88 * S), "STREET", font=big, fill=BLUE)
name = italic(name)
shadow = Image.new("RGBA", name.size, (0, 0, 0, 0))
shadow.paste((0, 0, 0, 150), (4 * S, 4 * S), name)
img.alpha_composite(shadow.crop((0, 0, W * S, H * S)))
img.alpha_composite(name.crop((0, 0, W * S, H * S)))

d = ImageDraw.Draw(img)
rule_x = 488 * S
d.polygon([(rule_x + 22 * S, 14 * S), (rule_x + 34 * S, 14 * S), (rule_x + 12 * S, 185 * S), (rule_x, 185 * S)], fill=RED)

sub = ImageFont.truetype(SUB, 54 * S)
tracked(d, (530 * S, 22 * S), "RECOMPILED", sub, WHITE, 4 * S)
tracked(d, (514 * S, 96 * S), "FOR ANDROID", sub, BLUE, 4 * S)

img = img.resize((W, H), Image.LANCZOS)
img.save(OUT, "WEBP", quality=95, method=6)
print("wrote", OUT, img.size)
