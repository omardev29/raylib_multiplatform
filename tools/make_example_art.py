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


def main() -> int:
    RUNNER.mkdir(parents=True, exist_ok=True)
    for name, make in (("sky.png", sky), ("hills.png", hills), ("ground.png", ground)):
        make().save(RUNNER / name, optimize=True)
        print(f"  wrote {RUNNER.relative_to(REPO) / name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
