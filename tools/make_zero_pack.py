#!/usr/bin/env python3
"""Write tests/fixtures/pack_zero/resources.rres: a pack that holds nothing.

WHY THIS SCRIPT EXISTS AND IS COMMITTED. tests/assets_test.cpp needs a real
.rres file whose central directory declares zero entries, because that is the
one shape that made open_pack() leak: rres allocates the entry array before it
knows the count is zero, and close_pack() returns early while no pack is open,
so nothing would ever free it. A binary fixture with no generator beside it is
a blob nobody dares change, so the bytes are written here instead and
tests/configure_test.py checks that the committed file is still exactly what
this script produces.

THE LAYOUT, which is rres 1.0's and the same one tools/rres_pack.c writes:

    header (16)          "rres", version 100, chunkCount, cdOffset, reserved
    RAWD info(32) + data one ordinary data chunk
    CDIR info(32) + data the central directory: propCount 1, props[0] = 0

Chunk data is propCount(4) + props[](4 each) + raw, and info.crc32 is a plain
zlib CRC-32 over exactly those bytes -- rres.h's own rresComputeCRC32 is the
standard table with an inverted start and end, which is what zlib.crc32 is.

TWO THINGS ARE DELIBERATE AND THE FIXTURE IS USELESS WITHOUT THEM.

  * cdOffset is NOT zero. rres reads it as `fseek(file, cdOffset, SEEK_CUR)`
    from byte 16, so it is relative to the end of the header -- and a cdOffset
    of 0 means "no central directory at all", which rres answers by returning
    an empty struct WITHOUT allocating. That path never leaked. So the file
    carries one data chunk ahead of the directory, to push it off zero.
  * propCount is 1 and props[0] is 0. rres reads the entry count out of
    props[0] without checking propCount, so a directory with no properties at
    all is a null dereference inside rres rather than the case under test.

That makes this the shape of a pack that was BUILT and then corrupted -- its
data chunks still there and its directory emptied -- which is exactly what
open_pack()'s guard is for.

    python3 tools/make_zero_pack.py [output path]
"""

import pathlib
import struct
import sys
import zlib


def chunk(four_cc: bytes, ident: int, data: bytes) -> bytes:
    """One resource chunk: its 32-byte info header and then its data."""
    info = struct.pack(
        "<4sIBBHIIIII",
        four_cc,
        ident,
        0,  # compType   = RRES_COMP_NONE
        0,  # cipherType = RRES_CIPHER_NONE
        0,  # flags
        len(data),  # packedSize
        len(data),  # baseSize, the same while nothing is compressed
        0,  # nextOffset
        0,  # reserved
        zlib.crc32(data) & 0xFFFFFFFF,
    )
    assert len(info) == 32, len(info)
    return info + data


def build() -> bytes:
    # propCount 1, props[0] 0: one property, and it says "no entries".
    raw = chunk(b"RAWD", 0x11111111, struct.pack("<II", 1, 0))
    cdir = chunk(b"CDIR", 0, struct.pack("<II", 1, 0))
    header = struct.pack(
        "<4sHHII",
        b"rres",
        100,  # version
        2,  # chunkCount: the data chunk and the directory
        16 + len(raw) - 16,  # cdOffset, relative to the end of the header
        0,  # reserved
    )
    assert len(header) == 16, len(header)
    return header + raw + cdir


def main():
    if len(sys.argv) > 1:
        path = pathlib.Path(sys.argv[1])
    else:
        path = (pathlib.Path(__file__).resolve().parent.parent / "tests" / "fixtures" /
                "pack_zero" / "resources.rres")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(build())
    print(f"wrote {path} ({path.stat().st_size} bytes, 0 entries)")


if __name__ == "__main__":
    main()
