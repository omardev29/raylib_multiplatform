#!/usr/bin/env python3
"""The art the examples carry, drawn by this script rather than by a person.

Every PNG under examples/*/*/resources/ that this file knows how to make is
generated here, deterministically, from a few lines of arithmetic: a gradient,
a couple of sine curves, a strip. It is committed next to the pictures so that
the pictures can be explained, changed and regenerated instead of being blobs
nobody dares touch -- the same reason tools/make_aseprite_fixture.py exists.

Nothing here is anyone's work but ours, so nothing here needs a licence line.

    python3 tools/make_example_art.py        # rewrites every file it owns

Pillow only. It is in the build image (python3-pil) and on every runner.
"""
from __future__ import annotations

import math
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("make_example_art.py: Pillow is not installed (pip install pillow)")
    sys.exit(1)

REPO = Path(__file__).resolve().parent.parent
RUNNER = REPO / "examples" / "games" / "05_endless_runner" / "resources"

WIDTH = 800  # every strip is one screen wide and repeats seamlessly


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(len(a)))


def sky() -> Image.Image:
    """A dusk gradient, 800x450, with a low sun. Repeats trivially: no detail
    changes along x."""
    img = Image.new("RGB", (WIDTH, 450))
    top, mid, horizon = (28, 32, 78), (120, 70, 110), (240, 150, 90)
    px = img.load()
    for y in range(450):
        t = y / 449.0
        c = lerp(top, mid, t / 0.65) if t < 0.65 else lerp(mid, horizon, (t - 0.65) / 0.35)
        for x in range(WIDTH):
            px[x, y] = c
    draw = ImageDraw.Draw(img)
    draw.ellipse([560, 250, 640, 330], fill=(255, 210, 120))
    return img


def hills() -> Image.Image:
    """Two rows of hills on a transparent strip, 800x240. Sines whose periods
    divide 800, so the right edge meets the left edge exactly -- that is what
    Parallax needs to draw the strip twice with no seam."""
    img = Image.new("RGBA", (WIDTH, 240), (0, 0, 0, 0))
    px = img.load()
    back, front = (70, 60, 120, 255), (40, 44, 92, 255)
    for x in range(WIDTH):
        a = 2 * math.pi * x / WIDTH
        back_y = 110 + 40 * math.sin(a * 2) + 18 * math.sin(a * 5 + 1.0)
        front_y = 170 + 30 * math.sin(a * 3 + 0.7) + 12 * math.sin(a * 8)
        for y in range(240):
            if y >= front_y:
                px[x, y] = front
            elif y >= back_y:
                px[x, y] = back
    return img


def ground() -> Image.Image:
    """The strip the player runs on, 800x90: grass over earth, with a few
    darker stones so the scrolling is visible."""
    img = Image.new("RGB", (WIDTH, 90), (78, 54, 40))
    draw = ImageDraw.Draw(img)
    draw.rectangle([0, 0, WIDTH, 14], fill=(96, 150, 72))
    draw.rectangle([0, 14, WIDTH, 18], fill=(64, 110, 52))
    for i in range(24):
        x = (i * 173) % WIDTH
        y = 30 + (i * 37) % 50
        draw.ellipse([x, y, x + 8, y + 5], fill=(60, 40, 30))
    return img


def runner() -> Image.Image:
    """The character, 36x54, in a pose that reads at a glance. Flat shapes and
    one outline colour: at 36 pixels wide there is no room for anything else,
    and a silhouette is what a runner is seen as anyway."""
    img = Image.new("RGBA", (36, 54), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    shirt, skin, dark = (240, 196, 80, 255), (250, 236, 214, 255), (46, 32, 44, 255)
    draw.line([(21, 25), (32, 31)], fill=shirt, width=5)  # arm, forward
    draw.line([(15, 25), (5, 21)], fill=shirt, width=5)  # arm, trailing
    draw.ellipse([13, 3, 25, 15], fill=skin)  # head
    draw.polygon([(12, 5), (26, 3), (26, 7), (13, 9)], fill=dark)  # cap
    draw.ellipse([20, 8, 23, 11], fill=dark)  # and an eye, looking ahead
    draw.polygon([(14, 15), (24, 15), (26, 34), (12, 34)], fill=shirt)  # body
    draw.line([(19, 33), (29, 45)], fill=dark, width=6)  # leg, forward
    draw.line([(17, 33), (8, 43)], fill=dark, width=6)  # leg, pushing off
    draw.line([(27, 45), (33, 49)], fill=dark, width=5)  # and the feet
    draw.line([(5, 42), (10, 46)], fill=dark, width=5)
    return img


def rock() -> Image.Image:
    """The obstacle, 40x60: a boulder with a lit face and a shadow side, so it
    reads as an object and not as the grey rectangle it used to be."""
    img = Image.new("RGBA", (40, 60), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    draw.polygon([(4, 59), (2, 26), (14, 6), (28, 4), (38, 24), (36, 59)],
                 fill=(92, 84, 96, 255))
    draw.polygon([(14, 6), (28, 4), (34, 22), (18, 30)], fill=(126, 118, 132, 255))
    draw.polygon([(4, 59), (2, 26), (12, 34), (12, 59)], fill=(62, 56, 70, 255))
    return img


def main() -> int:
    RUNNER.mkdir(parents=True, exist_ok=True)
    for name, make in (("sky.png", sky), ("hills.png", hills), ("ground.png", ground),
                       ("runner.png", runner), ("rock.png", rock)):
        make().save(RUNNER / name, optimize=True)
        print(f"  wrote {RUNNER.relative_to(REPO) / name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
