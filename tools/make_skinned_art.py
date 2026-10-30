#!/usr/bin/env python3
"""The art of examples/ui/09_skinned, drawn by this script rather than by a person.

The example is a game's shop screen in rmp::ui, skinned: every button, panel
and bar a picture, every icon a slice of one Aseprite file. So the art it needs
is the art an artist would hand over -- and this writes it, deterministically:

  skin.aseprite     one 72x48 frame, and the slices the example looks up by
                    name: "button", "button_down" and "panel" are nine-slices
                    (Aseprite's 9-Slices, a centre and four margins); "coin",
                    "heart", "gear", "star", "play" and "lock" are 12x12 icons;
                    "bar" is the fill of a progress bar, a nine-slice too.
  spinner.aseprite  four 16x16 frames of a turning star and a tag, "spin": the
                    animated sprite a kept rmp::ui::Image plays.
  click.wav, hover.wav, nope.wav
                    square-wave blips, as Pong's (tools/make_example_art.py).

Written from the published format
(https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md): the
header, one layer, one raw cel per frame, a tags chunk and slice chunks -- the
parts the framework reads. Nothing here is anyone's work but ours: CC0, as
examples/ui/09_skinned/resources/License-art.txt says.

    python3 tools/make_skinned_art.py      # rewrites every file it owns
"""
from __future__ import annotations

import importlib.util
import struct
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    Image = ImageDraw = None

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "examples" / "ui" / "09_skinned" / "resources"

GOLD = (236, 184, 72, 255)
GOLD_DARK = (150, 98, 30, 255)
WOOD = (94, 58, 40, 255)
WOOD_DARK = (58, 34, 26, 255)
WOOD_LIGHT = (128, 84, 56, 255)
INK = (32, 20, 16, 255)
RED = (214, 64, 64, 255)
GREEN = (96, 192, 96, 255)
BLUE = (88, 140, 232, 255)
WHITE = (250, 246, 236, 255)
CLEAR = (0, 0, 0, 0)

# name -> (x, y, w, h, centre or None): where each slice is on the skin.
SLICES = {
    "button": (0, 0, 24, 24, (8, 8, 8, 8)),
    "button_down": (24, 0, 24, 24, (8, 8, 8, 8)),
    "panel": (48, 0, 24, 24, (8, 8, 8, 8)),
    "coin": (0, 24, 12, 12, None),
    "heart": (12, 24, 12, 12, None),
    "gear": (24, 24, 12, 12, None),
    "star": (36, 24, 12, 12, None),
    "play": (48, 24, 12, 12, None),
    "lock": (60, 24, 12, 12, None),
    "bar": (0, 36, 24, 12, (4, 4, 16, 4)),
}


# ---- the Aseprite format ------------------------------------------------------

def u8(v: int) -> bytes:
    return struct.pack("<B", v)


def u16(v: int) -> bytes:
    return struct.pack("<H", v)


def u32(v: int) -> bytes:
    return struct.pack("<I", v)


def s32(v: int) -> bytes:
    return struct.pack("<i", v)


def text(s: str) -> bytes:
    data = s.encode("utf-8")
    return u16(len(data)) + data


def chunk(kind: int, body: bytes) -> bytes:
    return u32(len(body) + 6) + u16(kind) + body


def layer_chunk() -> bytes:
    # visible + editable, a normal layer, level 0, blend normal, opaque.
    return chunk(0x2004, u16(3) + u16(0) + u16(0) + u16(0) + u16(0) + u16(0) + u8(255)
                 + b"\0\0\0" + text("art"))


def cel_chunk(picture: "Image.Image") -> bytes:
    w, h = picture.size
    pixels = picture.convert("RGBA").tobytes()
    # layer 0, at 0,0, opaque, a raw cel (type 0), no z-index.
    head = u16(0) + struct.pack("<hh", 0, 0) + u8(255) + u16(0) + struct.pack("<h", 0) + b"\0" * 5
    return chunk(0x2005, head + u16(w) + u16(h) + pixels)


def tags_chunk(tags: list[tuple[str, int, int]]) -> bytes:
    body = u16(len(tags)) + b"\0" * 8
    for name, first, last in tags:
        body += u16(first) + u16(last) + u8(0) + u16(0) + b"\0" * 6 + b"\0\0\0" + b"\0" + text(name)
    return chunk(0x2018, body)


def slice_chunk(name: str, x: int, y: int, w: int, h: int, centre) -> bytes:
    flags = 1 if centre else 0
    body = u32(1) + u32(flags) + u32(0) + text(name)
    body += u32(0) + s32(x) + s32(y) + u32(w) + u32(h)
    if centre:
        cx, cy, cw, ch = centre
        body += s32(cx) + s32(cy) + u32(cw) + u32(ch)
    return chunk(0x2022, body)


def aseprite(frames: list["Image.Image"], extra: list[bytes], durations: list[int]) -> bytes:
    """A file of `frames` (all one size), one layer, `extra` chunks in the first frame."""
    w, h = frames[0].size
    body = b""
    for i, picture in enumerate(frames):
        chunks = ([layer_chunk()] + extra if i == 0 else []) + [cel_chunk(picture)]
        data = b"".join(chunks)
        body += u32(len(data) + 16) + u16(0xF1FA) + u16(len(chunks)) + u16(durations[i]) + b"\0\0" \
            + u32(len(chunks)) + data
    header = u32(128 + len(body)) + u16(0xA5E0) + u16(len(frames)) + u16(w) + u16(h) + u16(32) \
        + u32(1) + u16(100) + b"\0" * 8 + u8(0) + b"\0" * 3 + u16(0) + u8(1) + u8(1) \
        + struct.pack("<hh", 0, 0) + u16(16) + u16(16) + b"\0" * 84
    return header + body


# ---- the pictures -------------------------------------------------------------

def frame_box(draw, x: int, y: int, w: int, h: int, face, edge, light) -> None:
    """A wooden frame with a gold rim: rounded corners, a light top-left edge,
    a dark bottom-right one -- what reads as a raised button at any size."""
    draw.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=5, fill=GOLD_DARK)
    draw.rounded_rectangle([x + 1, y + 1, x + w - 2, y + h - 2], radius=4, fill=GOLD)
    draw.rounded_rectangle([x + 3, y + 3, x + w - 4, y + h - 4], radius=3, fill=edge)
    draw.rectangle([x + 4, y + 4, x + w - 5, y + h - 5], fill=face)
    draw.line([x + 4, y + 4, x + w - 5, y + 4], fill=light)
    draw.line([x + 4, y + 4, x + 4, y + h - 5], fill=light)


def skin() -> "Image.Image":
    img = Image.new("RGBA", (72, 48), CLEAR)
    d = ImageDraw.Draw(img)
    frame_box(d, 0, 0, 24, 24, WOOD, WOOD_DARK, WOOD_LIGHT)          # button
    frame_box(d, 24, 0, 24, 24, WOOD_DARK, WOOD_DARK, WOOD)          # button_down
    frame_box(d, 48, 0, 24, 24, (52, 36, 30, 255), WOOD_DARK, WOOD)  # panel
    # coin
    d.ellipse([1, 25, 10, 34], fill=GOLD_DARK)
    d.ellipse([2, 26, 9, 33], fill=GOLD)
    d.line([5, 28, 5, 31], fill=GOLD_DARK)
    # heart
    d.ellipse([13, 26, 18, 31], fill=RED)
    d.ellipse([17, 26, 22, 31], fill=RED)
    d.polygon([(13, 29), (22, 29), (17, 35)], fill=RED)
    # gear
    d.ellipse([25, 25, 34, 34], fill=(170, 170, 180, 255))
    for ox, oy in ((29, 24), (29, 34), (24, 29), (34, 29)):
        d.rectangle([ox, oy, ox + 1, oy + 1], fill=(170, 170, 180, 255))
    d.ellipse([28, 28, 31, 31], fill=CLEAR)
    # star
    d.polygon([(41, 24), (43, 28), (47, 29), (44, 32), (45, 35), (41, 33), (37, 35), (38, 32),
               (35, 29), (39, 28)], fill=GOLD)
    # play
    d.polygon([(51, 26), (58, 30), (51, 34)], fill=GREEN)
    # lock
    d.rectangle([62, 30, 69, 35], fill=(170, 170, 180, 255))
    d.arc([63, 25, 68, 32], 180, 360, fill=(170, 170, 180, 255), width=2)
    # bar: a green fill with a lighter top line, four-pixel rounded ends
    d.rounded_rectangle([0, 36, 23, 47], radius=4, fill=(40, 120, 48, 255))
    d.rounded_rectangle([1, 37, 22, 46], radius=3, fill=GREEN)
    d.line([4, 38, 19, 38], fill=(170, 230, 160, 255))
    return img


def spinner_frames() -> list["Image.Image"]:
    """A star turning a quarter at a time: four frames, 16x16."""
    out = []
    for i in range(4):
        img = Image.new("RGBA", (16, 16), CLEAR)
        d = ImageDraw.Draw(img)
        colour = (GOLD, BLUE, GREEN, RED)[i]
        d.ellipse([3, 3, 12, 12], outline=colour, width=2)
        x, y = ((7, 0), (12, 7), (7, 12), (0, 7))[i]
        d.rectangle([x, y, x + 3, y + 3], fill=WHITE)
        out.append(img)
    return out


def sounds() -> dict[str, bytes]:
    """click: a 720 Hz blip. hover: a quiet 1200 Hz tick. nope: 220 then 160 Hz."""
    spec = importlib.util.spec_from_file_location("make_example_art",
                                                  REPO / "tools" / "make_example_art.py")
    art = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(art)
    return {
        "click.wav": art.tone([720], 0.05),
        "hover.wav": art.tone([1200], 0.02, 0.12),
        "nope.wav": art.tone([220, 160], 0.08),
    }


def files() -> dict[str, bytes]:
    """Every file this script owns, by name, as bytes."""
    out = dict(sounds())
    slices = [slice_chunk(name, *where) for name, where in SLICES.items()]
    out["skin.aseprite"] = aseprite([skin()], slices, [100])
    out["spinner.aseprite"] = aseprite(spinner_frames(), [tags_chunk([("spin", 0, 3)])],
                                       [120, 120, 120, 120])
    return out


def main() -> int:
    if Image is None:
        print("make_skinned_art.py: Pillow is not installed (pip install pillow)")
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    for name, data in files().items():
        (OUT / name).write_bytes(data)
        print(f"  wrote {(OUT / name).relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
