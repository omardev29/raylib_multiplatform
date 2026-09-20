#!/usr/bin/env python3
"""Write tests/fixtures/anim.aseprite: four 4x4 frames and two tags.

WHY THIS SCRIPT EXISTS AND IS COMMITTED. tests/animation_test.cpp needs a real
Aseprite file -- parsing a fake one would be testing the fake. Aseprite is not
installed anywhere in this project and will not be, so the fixture is generated
from the published file format instead, and the generator is committed next to
it so the fixture can be explained, changed and regenerated rather than being a
blob nobody dares touch.

The format is the one documented at
https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md
Only the parts cute_aseprite needs are written: the header, one layer, one raw
(uncompressed) cel per frame, and a tags chunk.

Frame durations are deliberately DIFFERENT -- 100, 200, 100 and 50 ms -- because
a fixture where every frame lasts the same length cannot tell "reads the
durations from the file" apart from "assumes a fixed frame rate", and that is
exactly the thing this layer promises.

    python3 tools/make_aseprite_fixture.py
"""

import pathlib
import struct

W, H = 4, 4
# (duration in ms, RGBA colour that fills the frame)
FRAMES = [
    (100, (255, 0, 0, 255)),
    (200, (0, 255, 0, 255)),
    (100, (0, 0, 255, 255)),
    (50, (255, 255, 0, 255)),
]
# (name, from, to, direction)  0 = forwards, 1 = backwards, 2 = ping-pong
TAGS = [("walk", 0, 1, 0), ("idle", 2, 3, 0), ("swing", 0, 3, 2)]


def u8(v):
    return struct.pack("<B", v)


def u16(v):
    return struct.pack("<H", v)


def u32(v):
    return struct.pack("<I", v)


def s16(v):
    return struct.pack("<h", v)


def ase_string(text):
    raw = text.encode("utf-8")
    return u16(len(raw)) + raw


def chunk(chunk_type, body):
    return u32(len(body) + 6) + u16(chunk_type) + body


def layer_chunk():
    body = b""
    body += u16(1)  # flags: visible
    body += u16(0)  # type: normal
    body += u16(0)  # child level
    body += u16(0)  # default width, ignored
    body += u16(0)  # default height, ignored
    body += u16(0)  # blend mode: normal
    body += u8(255)  # opacity
    body += b"\x00" * 3  # reserved
    body += ase_string("Layer 1")
    return chunk(0x2004, body)


def cel_chunk(colour):
    body = b""
    body += u16(0)  # layer index
    body += s16(0)  # x
    body += s16(0)  # y
    body += u8(255)  # opacity
    body += u16(0)  # cel type 0: raw pixels, so no zlib on either side
    body += s16(0)  # z index
    body += b"\x00" * 5  # reserved
    body += u16(W) + u16(H)
    body += bytes(colour) * (W * H)
    return chunk(0x2005, body)


def tags_chunk():
    body = u16(len(TAGS)) + b"\x00" * 8
    for name, first, last, direction in TAGS:
        body += u16(first) + u16(last)
        body += u8(direction)
        body += u16(0)  # repeat: 0 = forever
        body += b"\x00" * 6  # reserved
        body += b"\x00" * 3  # deprecated RGB
        body += u8(0)  # extra byte
        body += ase_string(name)
    return chunk(0x2018, body)


def frame(index):
    chunks = b""
    count = 0
    if index == 0:
        chunks += layer_chunk() + tags_chunk()
        count += 2
    chunks += cel_chunk(FRAMES[index][1])
    count += 1

    header = u32(len(chunks) + 16)
    header += u16(0xF1FA)
    header += u16(count if count < 0xFFFF else 0xFFFF)  # old chunk count
    header += u16(FRAMES[index][0])  # duration, milliseconds
    header += b"\x00" * 2  # reserved
    header += u32(count)  # new chunk count
    return header + chunks


def build():
    frames = b"".join(frame(i) for i in range(len(FRAMES)))

    header = b""
    header += u32(128 + len(frames))  # file size
    header += u16(0xA5E0)  # magic
    header += u16(len(FRAMES))
    header += u16(W) + u16(H)
    header += u16(32)  # colour depth: RGBA
    header += u32(1)  # flags: layer opacity is valid
    header += u16(100)  # deprecated speed
    header += u32(0) + u32(0)
    header += u8(0)  # palette entry for transparency
    header += b"\x00" * 3  # ignored
    header += u16(0)  # colours in the palette
    header += u8(1) + u8(1)  # pixel width and height (1:1)
    header += s16(0) + s16(0)  # grid x, y
    header += u16(16) + u16(16)  # grid width, height
    header += b"\x00" * 84  # reserved, to 128 bytes
    assert len(header) == 128, len(header)
    return header + frames


def main():
    out = pathlib.Path(__file__).resolve().parent.parent / "tests" / "fixtures"
    out.mkdir(parents=True, exist_ok=True)
    path = out / "anim.aseprite"
    path.write_bytes(build())
    print(f"wrote {path} ({path.stat().st_size} bytes, {len(FRAMES)} frames, "
          f"{len(TAGS)} tags)")


if __name__ == "__main__":
    main()
