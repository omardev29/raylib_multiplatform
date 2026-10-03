#!/usr/bin/env python3
"""The art, the sounds and the first world of examples/games/07_platformer.

Everything the platformer shows or plays comes from four packs by Kenney
(https://kenney.nl), all CC0 -- public domain, so they can sit in the template
as they are -- and a music loop of his from the same licence. The packs are
downloaded by hand ONCE and are not committed: this script checks each one
against the sha256 below, refuses anything else, and makes from them what IS
committed, in examples/games/07_platformer/resources/:

  tiles.png, characters.png, backgrounds.png   the pack's packed sheets, as they are
  player / walker / bat / coin / flag .aseprite  cut from those sheets, with tags,
                                               because the framework reads .aseprite
  jump, coin, stomp, hurt, key, door, select,  short names, for rmp::audio::play()
  win, lose .ogg
  music.ogg                                    "Retro Beat", from Music Loops
  License-kenney.txt                           which pack, which version, where

    python3 tools/make_platformer_assets.py --kenney ~/Downloads/kenney

AND THE LEVEL. With --level it also writes world.ldtk: an LDtk 1.5.3 project of
three levels in a GridVania world, generated so that the game has a world from
its first commit. It is a starting point and not a source: once the project has
been opened and saved in LDtk, the file belongs to LDtk and is edited there --
which is why this refuses to overwrite it without --force. The generated
project is complete enough to be edited properly, not just loaded: the terrain
is drawn by auto-layer RULES from an IntGrid, one rule group per biome (a level
field picks it), so painting a cell `Solid` in the editor draws the right edge,
corner or top. The tiles the rules would draw are written into the file as
well, by the same rules applied here (`terrain_tile` below), so the game sees
the level exactly as LDtk will.

The sounds were chosen by their shape, measured rather than guessed: a jump is
a short rising pitch (maximize_008, 300 -> 480 Hz in a quarter of a second), the
win jingle rises to its last note (NES12) and the game-over one falls to a low
held note (NES11).

Pillow does the cutting; it is in the build image and on the runners.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import struct
import sys
import uuid
import zipfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # main() says so
    Image = None

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "examples" / "games" / "07_platformer" / "resources"

# ---------------------------------------------------------------------------
# The inputs, pinned. (file, sha256, where to get it)
# ---------------------------------------------------------------------------

PACKS = {
    "pixel-platformer": (
        "kenney_pixel-platformer.zip",
        "d01a196dbe3cc964e00d83ba3b987df62f332dc9260c9f941b4fbcc9047130f4",
        "https://kenney.nl/media/pages/assets/pixel-platformer/33bb4921eb-1696667883/"
        "kenney_pixel-platformer.zip",
    ),
    "impact-sounds": (
        "kenney_impact-sounds.zip",
        "029d734af1582474edf3a694d1b0cebc97c1c152f2f39fa34d4c2bafc5de77f8",
        "https://kenney.nl/media/pages/assets/impact-sounds/87b4ddecda-1677589768/"
        "kenney_impact-sounds.zip",
    ),
    "interface-sounds": (
        "kenney_interface-sounds.zip",
        "f2193d072726d6758a5f7871b2dcc54dcce0d5c35c6f0a62f92549b327c81232",
        "https://kenney.nl/media/pages/assets/interface-sounds/fa43c1dd4d-1677589452/"
        "kenney_interface-sounds.zip",
    ),
    "music-jingles": (
        "kenney_music-jingles.zip",
        "b729ba57959bd58793d2c5cafa348aaf2655d354f3da35ec4729e03ec77197b8",
        "https://kenney.nl/media/pages/assets/music-jingles/f37e530b9e-1677590399/"
        "kenney_music-jingles.zip",
    ),
    # Music Loops is no longer on kenney.nl; this is the same file from a mirror
    # of his sound pack, with the pack's own readme (CC0) beside it.
    "music-loop": (
        "Retro Beat.ogg",
        "09f9ea0abf097393eb57a11a76354be1011fd239cf411090c9f57b6957e331dc",
        "https://www.gamesounds.xyz/Kenney%27s%20Sound%20Pack/Music%20Loops/Retro/"
        "Retro%20Beat.ogg",
    ),
}

# What is taken from each pack, under the name the game uses.
SHEETS = {
    "tiles.png": "Tilemap/tilemap_packed.png",  # 18x18, 20 x 9
    "characters.png": "Tilemap/tilemap-characters_packed.png",  # 24x24, 9 x 3
    "backgrounds.png": "Tilemap/tilemap-backgrounds_packed.png",  # 24x24, 8 x 3
}
SOUNDS = {
    "jump.ogg": ("interface-sounds", "Audio/maximize_008.ogg"),
    "coin.ogg": ("interface-sounds", "Audio/confirmation_001.ogg"),
    "select.ogg": ("interface-sounds", "Audio/select_001.ogg"),
    "stomp.ogg": ("impact-sounds", "Audio/impactSoft_heavy_001.ogg"),
    "hurt.ogg": ("impact-sounds", "Audio/impactPunch_heavy_000.ogg"),
    "key.ogg": ("music-jingles", "Audio/8-Bit jingles/jingles_NES09.ogg"),
    "door.ogg": ("music-jingles", "Audio/8-Bit jingles/jingles_NES08.ogg"),
    "win.ogg": ("music-jingles", "Audio/8-Bit jingles/jingles_NES12.ogg"),
    "lose.ogg": ("music-jingles", "Audio/8-Bit jingles/jingles_NES11.ogg"),
}

TILE = 18  # the tiles sheet
CHAR = 24  # the characters and backgrounds sheets
TILES_W, TILES_H = 20, 9


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_inputs(folder: Path) -> dict[str, Path]:
    found, problems = {}, []
    for key, (name, digest, url) in PACKS.items():
        path = folder / name
        if not path.is_file():
            problems.append(f"missing {path}\n    download: {url}")
            continue
        got = sha256(path)
        if got != digest:
            problems.append(f"{path} is not the pinned file\n    sha256 {got}\n    wanted {digest}")
            continue
        found[key] = path
    if problems:
        sys.exit("make_platformer_assets: \n  " + "\n  ".join(problems))
    return found


def member(zip_path: Path, suffix: str) -> bytes:
    """A file of a pack by the end of its path: the zips put everything under a
    top folder whose name is not worth pinning."""
    with zipfile.ZipFile(zip_path) as z:
        names = [n for n in z.namelist() if n.endswith(suffix)]
        if len(names) != 1:
            sys.exit(f"make_platformer_assets: {zip_path.name} has {len(names)} files ending in {suffix}")
        return z.read(names[0])


# ---------------------------------------------------------------------------
# Aseprite, written from the published format
# (https://github.com/aseprite/aseprite/blob/main/docs/ase-file-specs.md):
# a header, one layer, one raw cel per frame, a tags chunk. The same subset
# tools/make_aseprite_fixture.py writes, for real pictures.
# ---------------------------------------------------------------------------

def u8(v): return struct.pack("<B", v)
def u16(v): return struct.pack("<H", v)
def u32(v): return struct.pack("<I", v)
def s16(v): return struct.pack("<h", v)


def ase_string(text: str) -> bytes:
    raw = text.encode("utf-8")
    return u16(len(raw)) + raw


def chunk(kind: int, body: bytes) -> bytes:
    return u32(len(body) + 6) + u16(kind) + body


def aseprite(width: int, height: int, frames: list, tags: list) -> bytes:
    """frames: [(milliseconds, Image RGBA)], tags: [(name, first, last, direction)]
    with direction 0 forwards, 1 backwards, 2 ping-pong."""
    layer = chunk(0x2004, u16(1) + u16(0) + u16(0) + u16(0) + u16(0) + u16(0) + u8(255)
                  + b"\x00" * 3 + ase_string("Layer 1"))
    tag_body = u16(len(tags)) + b"\x00" * 8
    for name, first, last, direction in tags:
        tag_body += u16(first) + u16(last) + u8(direction) + u16(0) + b"\x00" * 6
        tag_body += b"\x00" * 3 + u8(0) + ase_string(name)
    tag_chunk = chunk(0x2018, tag_body)

    out = b""
    for i, (ms, picture) in enumerate(frames):
        assert picture.size == (width, height), picture.size
        cel = u16(0) + s16(0) + s16(0) + u8(255) + u16(0) + s16(0) + b"\x00" * 5
        cel += u16(width) + u16(height) + picture.convert("RGBA").tobytes()
        chunks = (layer + tag_chunk if i == 0 else b"") + chunk(0x2005, cel)
        count = 3 if i == 0 else 1
        out += u32(len(chunks) + 16) + u16(0xF1FA) + u16(count) + u16(ms) + b"\x00" * 2
        out += u32(count) + chunks

    header = u32(128 + len(out)) + u16(0xA5E0) + u16(len(frames)) + u16(width) + u16(height)
    header += u16(32) + u32(1) + u16(100) + u32(0) + u32(0) + u8(0) + b"\x00" * 3 + u16(0)
    header += u8(1) + u8(1) + s16(0) + s16(0) + u16(16) + u16(16) + b"\x00" * 84
    assert len(header) == 128
    return header + out


def cell(sheet: Image.Image, index: int, size: int) -> Image.Image:
    columns = sheet.width // size
    x, y = (index % columns) * size, (index // columns) * size
    return sheet.crop((x, y, x + size, y + size))


def hurt_tint(picture: Image.Image) -> Image.Image:
    """The frame the player shows when hit: the same alien, gone red."""
    out = picture.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = px[x, y]
            px[x, y] = (min(255, r + 80), g * 2 // 5, b * 2 // 5, a)
    return out


def sprites(tiles: Image.Image, chars: Image.Image) -> dict[str, bytes]:
    c = lambda i: cell(chars, i, CHAR)  # noqa: E731
    t = lambda i: cell(tiles, i, TILE)  # noqa: E731
    return {
        # The green alien: 0 stands, 1 is mid-step.
        "player.aseprite": aseprite(CHAR, CHAR, [(150, c(0)), (150, c(1)), (100, hurt_tint(c(0)))],
                                    [("idle", 0, 0, 0), ("walk", 0, 1, 0), ("jump", 1, 1, 0),
                                     ("hurt", 2, 2, 0)]),
        # The small blue walker: two steps, and 20 is the same beetle flattened.
        "walker.aseprite": aseprite(CHAR, CHAR, [(220, c(18)), (220, c(19)), (100, c(20))],
                                    [("walk", 0, 1, 0), ("squash", 2, 2, 0)]),
        "bat.aseprite": aseprite(CHAR, CHAR, [(110, c(24)), (110, c(25)), (110, c(26))],
                                 [("fly", 0, 2, 2)]),
        "coin.aseprite": aseprite(TILE, TILE, [(260, t(151)), (180, t(152))], [("spin", 0, 1, 0)]),
        "flag.aseprite": aseprite(TILE, TILE, [(260, t(111)), (260, t(112))], [("wave", 0, 1, 0)]),
    }


# ---------------------------------------------------------------------------
# The licence file
# ---------------------------------------------------------------------------

LICENSE_HEAD = """\
The art and sound in this folder are by Kenney (https://kenney.nl), released
under Creative Commons Zero (CC0 1.0): public domain, free for any use, with no
attribution required -- credited here anyway, because it is his work.

Made from these, unmodified except for cutting sprites out of the sheets and
giving the sounds short names, by tools/make_platformer_assets.py, which also
checks every download against its sha256:

  Pixel Platformer 1.2       https://kenney.nl/assets/pixel-platformer
      tiles.png, characters.png, backgrounds.png, *.aseprite
  Impact Sounds 1.0          https://kenney.nl/assets/impact-sounds
      stomp.ogg (impactSoft_heavy_001), hurt.ogg (impactPunch_heavy_000)
  Interface Sounds 1.0       https://kenney.nl/assets/interface-sounds
      jump.ogg (maximize_008), coin.ogg (confirmation_001), select.ogg (select_001)
  Music Jingles              https://kenney.nl/assets/music-jingles
      key.ogg (NES09), door.ogg (NES08), win.ogg (NES12), lose.ogg (NES11)
  Music Loops 1.1            Kenney's "Music Loops" pack (in his All-in-1 bundle)
      music.ogg ("Retro Beat")

world.ldtk is this repository's: the level, generated by the same script and
then edited in LDtk (https://ldtk.io).

The Pixel Platformer pack's own licence file follows.

------------------------------------------------------------------------------
"""


# ---------------------------------------------------------------------------
# The world: three 40 x 16 levels of 18 px cells, left to right
# ---------------------------------------------------------------------------
#
# '#' is a solid cell (IntGrid value 1, `Solid`), '.' is air. The ground at the
# edge between two levels is at the same height on both sides, so walking
# across is seamless; LDtk's GridVania layout puts the three side by side.

W, H = 40, 16
LEVEL_PX = (W * TILE, H * TILE)  # 720 x 288, a whole number of 24 px backdrop tiles

LEVELS = [
    {
        "name": "Meadow",
        "biome": "Grass",
        "rows": [
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "................####....................",
            "..............................##########",
            "..............................##########",
            "############..##########################",
            "############..##########################",
            "############..##########################",
        ],
        # (cx, cy, tile): plants and a crate on the ground. Tile 26, the crate,
        # carries the tileset's `Solid` tag: decoration you cannot walk through.
        "decor": [(1, 12, 125), (5, 12, 124), (11, 12, 128), (15, 12, 124), (20, 12, 129),
                  (28, 12, 26), (31, 10, 126), (34, 10, 125), (38, 10, 124)],
        "clouds": [(3, 2), (14, 4), (27, 1)],
        "entities": [
            ("Player", 2, 12, {}),
            ("Sign", 4, 12, {"text": "Arrows or A/D to move", "arrow": "None"}),
            ("Sign", 9, 12, {"text": "Space or W to jump", "arrow": "Right"}),
            ("Coin", 6, 11, {}), ("Coin", 7, 11, {}), ("Coin", 8, 11, {}),
            ("Coin", 16, 9, {}), ("Coin", 17, 9, {}), ("Coin", 18, 9, {}), ("Coin", 19, 9, {}),
            ("Sign", 19, 12, {"text": "Jump on their heads!", "arrow": "None"}),
            ("Walker", 24, 12, {"speed": 35.0, "patrol": [(21, 12), (26, 12)], "stompable": True}),
            ("Coin", 23, 10, {}), ("Coin", 25, 10, {}),
            ("Coin", 33, 9, {}), ("Coin", 35, 9, {}), ("Coin", 37, 9, {"value": 5}),
        ],
    },
    {
        "name": "Desert",
        "biome": "Sand",
        "rows": [
            ".....................................#..",
            ".....................................#..",
            ".....................................#..",
            ".....................................#..",
            ".....................................#..",
            ".....................................#..",
            ".....................................#..",
            "..............................##.....#..",
            ".....................................#..",
            ".....................................#..",
            "..................................##.#..",
            "#########...............................",
            "#########...............................",
            "#########......................#########",
            "#########......................#########",
            "#########......................#########",
        ],
        "decor": [(2, 10, 127), (5, 10, 124), (33, 12, 127), (39, 12, 124)],
        "clouds": [(6, 3), (20, 1), (31, 3)],
        "entities": [
            ("Sign", 6, 10, {"text": "Ride the platform", "arrow": "Right"}),
            ("MovingPlatform", 9, 11, {"to": (29, 11), "speed": 45.0}),
            ("Coin", 12, 9, {}), ("Coin", 16, 9, {}), ("Coin", 20, 9, {}), ("Coin", 24, 9, {}),
            ("Bat", 18, 8, {"speed": 45.0, "patrol": [(13, 8), (25, 8)]}),
            ("Coin", 34, 9, {}), ("Coin", 35, 9, {}),
            ("Key", 30, 6, {"opens": "Door"}),
            ("Coin", 31, 6, {}),
            ("Door", 37, 12, {}),
        ],
    },
    {
        "name": "Snowfield",
        "biome": "Snow",
        "rows": [
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "........................................",
            "...........................######.......",
            "...........................######.......",
            "...........................######.......",
            "...........................######.......",
            "##########....##########...######..#####",
            "##########....##########...######..#####",
            "##########....##########...######..#####",
        ],
        "decor": [(1, 12, 145), (3, 12, 126), (16, 12, 144), (23, 12, 126), (28, 8, 144),
                  (35, 12, 145)],
        "clouds": [(5, 3), (19, 2), (33, 4)],
        "entities": [
            ("Spikes", 6, 12, {"width": 2}),
            ("Coin", 11, 10, {}), ("Coin", 12, 10, {}),
            ("Walker", 18, 12, {"speed": 45.0, "patrol": [(15, 12), (22, 12)], "stompable": True}),
            ("Coin", 16, 11, {}), ("Coin", 18, 11, {}), ("Coin", 20, 11, {}),
            ("MovingPlatform", 24, 13, {"to": (25, 9), "speed": 35.0}),
            ("Coin", 25, 10, {}),
            ("Walker", 30, 8, {"speed": 40.0, "patrol": [(28, 8), (32, 8)], "stompable": True}),
            ("Bat", 30, 5, {"speed": 55.0, "patrol": [(27, 5), (34, 5)]}),
            ("Coin", 28, 7, {}), ("Coin", 30, 6, {}), ("Coin", 32, 7, {}),
            ("Coin", 37, 11, {}),
            ("Goal", 38, 12, {}),
        ],
    },
]

# The terrain tiles, by biome: a one-cell-tall platform (thin) and the top row
# of a thicker block (top), each as both ends, the middle, and a lone cell.
TOPS = {
    "Grass": {"thin": (1, 2, 3, 0), "top": (21, 22, 23, 20)},
    "Sand": {"thin": (41, 42, 43, 40), "top": (61, 62, 63, 60)},
    "Snow": {"thin": (81, 82, 83, 80), "top": (101, 102, 103, 100)},
}
# Below the top, every biome is the same earth. The names say which sides have
# the dark outline: 120 and 140 have both, for a column one cell wide. (4, 5,
# 24 and 25 look like fill and are not: they carry inner-corner notches.)
EARTH = {"column": 120, "left": 121, "right": 123, "bottom_column": 140,
         "bottom_left": 141, "bottom": 142, "bottom_right": 143, "fill": (122, 104)}
# The backdrop, 24 px tiles from backgrounds.png: sky, a horizon strip, ground.
BACKDROP = {
    "Grass": {"sky": (6, 7), "horizon": (14, 15), "fill": (22, 23)},
    "Sand": {"sky": (4, 5), "horizon": (12, 13), "fill": (20, 21)},
    "Snow": {"sky": (0, 1), "horizon": (8, 9, 10, 11), "fill": (16, 17)},
}
BIOMES = ["Grass", "Sand", "Snow"]


def rules_for(biome: str) -> list[dict]:
    """The rule list of one biome, most specific first, every one breaking on
    match: exactly what terrain_tile() does, in LDtk's terms. A pattern is the
    3x3 around the cell, row by row: 1 must be Solid, -1 must not, 0 either."""
    thin, top = TOPS[biome]["thin"], TOPS[biome]["top"]
    N, W_, E, S = 1, 3, 5, 7

    def pattern(**want):
        p = [0] * 9
        p[4] = 1
        for side, value in want.items():
            p[{"n": N, "w": W_, "e": E, "s": S}[side]] = value
        return p

    return [
        (thin[3], pattern(n=-1, s=-1, w=-1, e=-1), 1),
        (thin[0], pattern(n=-1, s=-1, w=-1), 1),
        (thin[2], pattern(n=-1, s=-1, e=-1), 1),
        (thin[1], pattern(n=-1, s=-1), 1),
        (top[3], pattern(n=-1, w=-1, e=-1), 1),
        (top[0], pattern(n=-1, w=-1), 1),
        (top[2], pattern(n=-1, e=-1), 1),
        (top[1], pattern(n=-1), 1),
        (EARTH["bottom_column"], pattern(s=-1, w=-1, e=-1), 1),
        (EARTH["bottom_left"], pattern(s=-1, w=-1), 1),
        (EARTH["bottom_right"], pattern(s=-1, e=-1), 1),
        (EARTH["bottom"], pattern(s=-1), 1),
        (EARTH["column"], pattern(w=-1, e=-1), 1),
        (EARTH["left"], pattern(w=-1), 1),
        (EARTH["right"], pattern(e=-1), 1),
        (EARTH["fill"][0], pattern(), 2),  # every other column...
        (EARTH["fill"][1], pattern(), 1),  # ...and the rest, for a little variety
    ]


def solid_at(rows: list[str], x: int, y: int) -> bool:
    # Outside the level counts as solid (the rules' outOfBoundsValue is 1), so
    # ground that runs into the edge of a level has no border there: it goes on
    # in the next one.
    if x < 0 or y < 0 or x >= W or y >= H:
        return True
    return rows[y][x] == "#"


def matches(rows, x, y, pattern) -> bool:
    for i, want in enumerate(pattern):
        if want == 0:
            continue
        solid = solid_at(rows, x + i % 3 - 1, y + i // 3 - 1)
        if (want == 1) != solid:
            return False
    return True


def terrain_tile(rows, x, y, biome) -> tuple[int, int] | None:
    """(tile, index of the rule that drew it), as LDtk's rules would."""
    if not solid_at(rows, x, y):
        return None
    for index, (tile, pattern, modulo) in enumerate(rules_for(biome)):
        if x % modulo != 0:  # LDtk: (cx - xOffset) % xModulo != 0 skips the rule
            continue
        if matches(rows, x, y, pattern):
            return tile, index
    raise AssertionError("the last rule matches every solid cell")


# ---------------------------------------------------------------------------
# The LDtk project
# ---------------------------------------------------------------------------

NAMESPACE = uuid.UUID("6f3d9b52-2f0e-4c49-9a43-7d9d1c2e0b17")


def iid(*parts) -> str:
    """Stable ids, so the same script writes the same file."""
    return str(uuid.uuid5(NAMESPACE, "/".join(str(p) for p in parts)))


def src_of(tile: int, columns: int, size: int) -> list[int]:
    return [(tile % columns) * size, (tile // columns) * size]


def rect(tileset_uid: int, tile: int, columns: int, size: int, w: int = 0, h: int = 0) -> dict:
    x, y = src_of(tile, columns, size)
    return {"tilesetUid": tileset_uid, "x": x, "y": y, "w": w or size, "h": h or size}


UID_TILES, UID_CHARS, UID_BACKDROP = 1, 2, 3
UID_ENUM_TAGS, UID_ENUM_ARROW, UID_ENUM_BIOME = 10, 11, 12
UID_LAYER_ENTITIES, UID_LAYER_DECOR, UID_LAYER_COLLISIONS, UID_LAYER_BACKDROP = 20, 21, 22, 23
UID_LEVEL_FIELD_BIOME = 30
UID_GROUP_BASE, UID_RULE_BASE = 100, 1000
UID_ENTITY_BASE, UID_FIELD_BASE = 200, 300


def field_def(uid, identifier, kind, ldtk_type, *, array=False, null=False, default=None,
              display="NameAndValue", ref_entity=None, enum_uid=None):
    return {
        "identifier": identifier, "doc": None, "__type": kind, "uid": uid, "type": ldtk_type,
        "isArray": array, "canBeNull": null, "arrayMinLength": None, "arrayMaxLength": None,
        "editorDisplayMode": display, "editorDisplayScale": 1, "editorDisplayPos": "Above",
        "editorLinkStyle": "StraightArrow" if ref_entity is None else "CurvedArrow",
        "editorDisplayColor": None, "editorAlwaysShow": False, "editorShowInWorld": True,
        "editorCutLongValues": True, "editorTextSuffix": None, "editorTextPrefix": None,
        "useForSmartColor": False, "exportToToc": False, "searchable": False, "min": None,
        "max": None, "regex": None, "acceptFileTypes": None, "defaultOverride": default,
        "textLanguageMode": None, "symmetricalRef": False, "autoChainRef": True,
        "allowOutOfLevelRef": True,
        "allowedRefs": "OnlySpecificEntity" if ref_entity is not None else "OnlySame",
        "allowedRefsEntityUid": ref_entity, "allowedRefTags": [], "tilesetUid": None,
    }


# name: (width, height, pivot, editor tile (tileset, index, w, h), colour, resizable, render mode)
ENTITIES = {
    "Player": (24, 24, (0.5, 1), (UID_CHARS, 0, 24, 24), "#63C74D", False, "FitInside"),
    "Walker": (24, 24, (0.5, 1), (UID_CHARS, 18, 24, 24), "#2CE8F5", False, "FitInside"),
    "Bat": (24, 24, (0.5, 0.5), (UID_CHARS, 24, 24, 24), "#B55088", False, "FitInside"),
    "MovingPlatform": (54, 18, (0, 0), (UID_TILES, 48, 54, 18), "#C28569", False, "Stretch"),
    "Coin": (18, 18, (0.5, 0.5), (UID_TILES, 151, 18, 18), "#FEE761", False, "FitInside"),
    "Key": (18, 18, (0.5, 0.5), (UID_TILES, 27, 18, 18), "#FEAE34", False, "FitInside"),
    "Door": (18, 36, (0, 1), (UID_TILES, 28, 18, 18), "#F77622", False, "Repeat"),
    "Spikes": (18, 18, (0, 1), (UID_TILES, 68, 18, 18), "#E43B44", True, "Repeat"),
    "Sign": (18, 18, (0.5, 1), (UID_TILES, 86, 18, 18), "#C0CBDC", False, "FitInside"),
    "Goal": (18, 36, (0.5, 1), (UID_TILES, 111, 18, 18), "#3E8948", False, "FitInside"),
}
ENTITY_UIDS = {name: UID_ENTITY_BASE + i for i, name in enumerate(ENTITIES)}


def entity_fields() -> dict[str, list[dict]]:
    uid = iter(range(UID_FIELD_BASE, UID_FIELD_BASE + 100))
    v_float = lambda x: {"id": "V_Float", "params": [x]}  # noqa: E731
    return {
        "Walker": [
            field_def(next(uid), "patrol", "Array<Point>", "F_Point", array=True, display="PointPath"),
            field_def(next(uid), "speed", "Float", "F_Float", default=v_float(40), display="Hidden"),
            field_def(next(uid), "stompable", "Bool", "F_Bool", default={"id": "V_Bool", "params": [True]},
                      display="Hidden"),
        ],
        "Bat": [
            field_def(next(uid), "patrol", "Array<Point>", "F_Point", array=True, display="PointPath"),
            field_def(next(uid), "speed", "Float", "F_Float", default=v_float(50), display="Hidden"),
        ],
        "MovingPlatform": [
            field_def(next(uid), "to", "Point", "F_Point", null=True, display="PointStar"),
            field_def(next(uid), "speed", "Float", "F_Float", default=v_float(40), display="Hidden"),
        ],
        "Coin": [field_def(next(uid), "value", "Int", "F_Int", default={"id": "V_Int", "params": [1]},
                           display="Hidden")],
        "Key": [field_def(next(uid), "opens", "EntityRef", "F_EntityRef", null=True,
                          display="RefLinkBetweenCenters", ref_entity=ENTITY_UIDS["Door"])],
        "Sign": [
            field_def(next(uid), "text", "String", "F_String", default={"id": "V_String", "params": ["..."]},
                      display="ValueOnly"),
            field_def(next(uid), "arrow", "LocalEnum.Arrow", f"F_Enum({UID_ENUM_ARROW})",
                      default={"id": "V_String", "params": ["None"]}, display="Hidden"),
        ],
    }


def entity_defs(fields) -> list[dict]:
    out = []
    for name, (w, h, pivot, tile, colour, resizable, mode) in ENTITIES.items():
        tileset, index, tw, th = tile
        columns, size = (TILES_W, TILE) if tileset == UID_TILES else (9, CHAR)
        out.append({
            "identifier": name, "uid": ENTITY_UIDS[name], "tags": [], "exportToToc": False,
            "allowOutOfBounds": False, "doc": None, "width": w, "height": h,
            "resizableX": resizable, "resizableY": False, "minWidth": None, "maxWidth": None,
            "minHeight": None, "maxHeight": None, "keepAspectRatio": False, "tileOpacity": 1,
            "fillOpacity": 0.08, "lineOpacity": 0, "hollow": False, "color": colour,
            "renderMode": "Tile", "showName": name in ("Player", "Goal"), "tilesetId": tileset,
            "tileRenderMode": mode, "tileRect": rect(tileset, index, columns, size, tw, th),
            "uiTileRect": None, "nineSliceBorders": [],
            "maxCount": 1 if name in ("Player", "Goal") else 0,
            "limitScope": "PerWorld" if name in ("Player", "Goal") else "PerLevel",
            "limitBehavior": "MoveLastOne", "pivotX": pivot[0], "pivotY": pivot[1],
            "fieldDefs": fields.get(name, []),
        })
    return out


def layer_def(uid, identifier, kind, grid, tileset=None, colour="#000000", values=(), groups=()):
    return {
        "__type": kind, "identifier": identifier, "type": kind, "uid": uid, "doc": None,
        "uiColor": colour, "gridSize": grid, "guideGridWid": 0, "guideGridHei": 0,
        "displayOpacity": 1, "inactiveOpacity": 1, "hideInList": False,
        "hideFieldsWhenInactive": kind != "Entities", "canSelectWhenInactive": True,
        "renderInWorldView": True, "pxOffsetX": 0, "pxOffsetY": 0, "parallaxFactorX": 0,
        "parallaxFactorY": 0, "parallaxScaling": True, "requiredTags": [], "excludedTags": [],
        "autoTilesKilledByOtherLayerUid": None, "uiFilterTags": [], "useAsyncRender": False,
        "intGridValues": list(values), "intGridValuesGroups": [], "autoRuleGroups": list(groups),
        "autoSourceLayerDefUid": None, "tilesetDefUid": tileset, "tilePivotX": 0, "tilePivotY": 0,
        "biomeFieldUid": UID_LEVEL_FIELD_BIOME if groups else None,
    }


def rule_groups() -> list[dict]:
    groups = []
    for g, biome in enumerate(BIOMES):
        rules = []
        for r, (tile, pattern, modulo) in enumerate(rules_for(biome)):
            # A rule that looks at nothing but its own cell is 1x1 in LDtk's
            # eyes, and it rewrites a 3x3 one that way when it saves.
            alone = pattern.count(0) == 8
            rules.append({
                "uid": UID_RULE_BASE + g * 100 + r, "active": True, "size": 1 if alone else 3,
                "tileRectsIds": [[tile]], "alpha": 1, "chance": 1, "breakOnMatch": True,
                "pattern": [1] if alone else pattern, "flipX": False, "flipY": False, "xModulo": modulo,
                "yModulo": 1, "xOffset": 0, "yOffset": 0, "tileXOffset": 0, "tileYOffset": 0,
                "tileRandomXMin": 0, "tileRandomXMax": 0, "tileRandomYMin": 0,
                "tileRandomYMax": 0, "checker": "None", "tileMode": "Single", "pivotX": 0,
                "pivotY": 0, "outOfBoundsValue": 1, "invalidated": False,
                "perlinActive": False, "perlinSeed": 1000 + g * 100 + r, "perlinScale": 0.2,
                "perlinOctaves": 2,
            })
        groups.append({
            "uid": UID_GROUP_BASE + g, "name": f"terrain: {biome.lower()}", "color": None,
            "icon": rect(UID_TILES, TOPS[biome]["top"][1], TILES_W, TILE), "active": True,
            "isOptional": False, "usesWizard": False, "requiredBiomeValues": [biome],
            "biomeRequirementMode": 0, "rules": rules,
        })
    return groups


def enum(uid, identifier, values, icons=None):
    return {
        "identifier": identifier, "uid": uid,
        "values": [{"id": v, "tileRect": (icons or {}).get(v), "color": 0} for v in values],
        "iconTilesetUid": UID_TILES if icons else None, "externalRelPath": None,
        "externalFileChecksum": None, "tags": [],
    }


def tileset(uid, identifier, rel, px, size):
    return {
        "__cWid": px[0] // size, "__cHei": px[1] // size, "identifier": identifier, "uid": uid,
        "relPath": rel, "embedAtlas": None, "pxWid": px[0], "pxHei": px[1],
        "tileGridSize": size, "spacing": 0, "padding": 0, "tags": [],
        "tagsSourceEnumUid": UID_ENUM_TAGS if uid == UID_TILES else None,
        "enumTags": [{"enumValueId": "Solid", "tileIds": [26]}] if uid == UID_TILES else [],
        "customData": [], "savedSelections": [], "cachedPixelData": None,
    }


def real_value(kind: str, value):
    if kind in ("Point", "Array<Point>"):
        return {"id": "V_String", "params": [f"{value[0]},{value[1]}"]}
    if kind == "Int":
        return {"id": "V_Int", "params": [value]}
    if kind == "Float":
        return {"id": "V_Float", "params": [value]}
    if kind == "Bool":
        return {"id": "V_Bool", "params": [value]}
    return {"id": "V_String", "params": [value]}


def build_world() -> dict:
    fields = entity_fields()
    defs_by_name = {name: {f["identifier"]: f for f in fields.get(name, [])} for name in ENTITIES}
    world_iid = iid("world")
    level_iids = [iid("level", lv["name"]) for lv in LEVELS]
    layer_iids = {}

    # Pass 1: every entity's iid, so a Key can point at the Door it opens.
    entity_iids = {}
    for li, lv in enumerate(LEVELS):
        for ei, (name, cx, cy, _) in enumerate(lv["entities"]):
            entity_iids[(li, ei)] = iid("entity", lv["name"], ei, name)

    levels = []
    for li, lv in enumerate(LEVELS):
        rows = lv["rows"]
        assert len(rows) == H and all(len(r) == W for r in rows), lv["name"]
        biome = lv["biome"]
        world_x = li * LEVEL_PX[0]
        for layer in ("Entities", "Decor", "Collisions", "Background"):
            layer_iids[(li, layer)] = iid("layer", lv["name"], layer)

        # Entities, with their fields.
        instances = []
        for ei, (name, cx, cy, values) in enumerate(lv["entities"]):
            w, h, pivot, tile, colour, _, _ = ENTITIES[name]
            if name == "Spikes":
                w = TILE * values.get("width", 1)
            # The pivot goes on the grid point of its cell: a bottom pivot on
            # the cell's bottom edge, so a Player in row 12 stands on row 13.
            px = [int(cx * TILE + pivot[0] * TILE), int(cy * TILE + pivot[1] * TILE)]
            if name in ("MovingPlatform",):
                px = [cx * TILE, cy * TILE]
            field_instances = []
            for key, fdef in defs_by_name[name].items():
                kind = fdef["__type"]
                if key in values:
                    value = values[key]
                elif fdef["defaultOverride"] is not None:
                    value = fdef["defaultOverride"]["params"][0]
                else:
                    value = None
                if kind == "Array<Point>":
                    exported = [{"cx": x, "cy": y} for x, y in value or []]
                    real = [real_value(kind, p) for p in value or []]
                elif kind == "Point":
                    exported = {"cx": value[0], "cy": value[1]} if value else None
                    real = [real_value(kind, value)] if value else [None]
                elif kind == "EntityRef":
                    target = next(e for e, (n, *_rest) in enumerate(lv["entities"]) if n == value)
                    ref = entity_iids[(li, target)]
                    exported = {"entityIid": ref, "layerIid": layer_iids[(li, "Entities")],
                                "levelIid": level_iids[li], "worldIid": world_iid}
                    real = [real_value(kind, ref)]
                else:
                    exported = value
                    real = [real_value(kind, value)]
                field_instances.append({"__identifier": key, "__type": kind, "__value": exported,
                                        "__tile": None, "defUid": fdef["uid"],
                                        "realEditorValues": real})
            tileset_uid, index, tw, th = tile
            columns, size = (TILES_W, TILE) if tileset_uid == UID_TILES else (9, CHAR)
            instances.append({
                "__identifier": name,
                "__grid": [(px[0] - (1 if pivot[0] == 1 else 0)) // TILE,
                           (px[1] - (1 if pivot[1] == 1 else 0)) // TILE],
                "__pivot": [pivot[0], pivot[1]], "__tags": [],
                "__tile": rect(tileset_uid, index, columns, size, tw, th),
                "__smartColor": colour, "iid": entity_iids[(li, ei)], "width": w, "height": h,
                "defUid": ENTITY_UIDS[name], "px": px, "fieldInstances": field_instances,
                "__worldX": world_x + px[0], "__worldY": px[1],
            })

        # The terrain the rules draw, and the IntGrid it is drawn from.
        csv, auto = [], []
        rules_uid_base = UID_RULE_BASE + BIOMES.index(biome) * 100
        for y in range(H):
            for x in range(W):
                csv.append(1 if rows[y][x] == "#" else 0)
                made = terrain_tile(rows, x, y, biome)
                if made is not None:
                    tile, rule = made
                    auto.append({"px": [x * TILE, y * TILE], "src": src_of(tile, TILES_W, TILE),
                                 "f": 0, "t": tile, "d": [rules_uid_base + rule, x + y * W], "a": 1})

        decor = [{"px": [x * TILE, y * TILE], "src": src_of(t, TILES_W, TILE), "f": 0, "t": t,
                  "d": [x + y * W], "a": 1} for x, y, t in lv["decor"]]
        for x, y in lv["clouds"]:  # three tiles wide: 153 154 155
            for i, t in enumerate((153, 154, 155)):
                decor.append({"px": [(x + i) * TILE, y * TILE], "src": src_of(t, TILES_W, TILE),
                              "f": 0, "t": t, "d": [x + i + y * W], "a": 1})

        bw, bh = LEVEL_PX[0] // CHAR, LEVEL_PX[1] // CHAR  # 30 x 12
        back = BACKDROP[biome]
        backdrop = []
        for y in range(bh):
            for x in range(bw):
                strip = back["sky"] if y < 6 else (back["horizon"] if y == 6 else back["fill"])
                t = strip[x % len(strip)]
                backdrop.append({"px": [x * CHAR, y * CHAR], "src": src_of(t, 8, CHAR), "f": 0,
                                 "t": t, "d": [x + y * bw], "a": 1})

        level_uid = 50 + li  # what each layer instance names as its levelId

        def layer_instance(name, kind, grid, cw, ch, def_uid, tileset_uid=None, rel=None, **content):
            return {
                "__identifier": name, "__type": kind, "__cWid": cw, "__cHei": ch,
                "__gridSize": grid, "__opacity": 1, "__pxTotalOffsetX": 0, "__pxTotalOffsetY": 0,
                "__tilesetDefUid": tileset_uid, "__tilesetRelPath": rel,
                "iid": layer_iids[(li, name)], "levelId": level_uid, "layerDefUid": def_uid,
                "pxOffsetX": 0, "pxOffsetY": 0, "visible": True, "optionalRules": [],
                "intGridCsv": content.get("csv", []), "autoLayerTiles": content.get("auto", []),
                "seed": 4242 + li, "overrideTilesetUid": None,
                "gridTiles": content.get("tiles", []), "entityInstances": content.get("entities", []),
            }

        neighbours = []
        if li > 0:
            neighbours.append({"levelIid": level_iids[li - 1], "dir": "w"})
        if li + 1 < len(LEVELS):
            neighbours.append({"levelIid": level_iids[li + 1], "dir": "e"})
        levels.append({
            "identifier": lv["name"], "iid": level_iids[li], "uid": level_uid,
            "worldX": world_x, "worldY": 0, "worldDepth": 0,
            "pxWid": LEVEL_PX[0], "pxHei": LEVEL_PX[1], "__bgColor": "#C0E8F0", "bgColor": None,
            "useAutoIdentifier": False, "bgRelPath": None, "bgPos": None, "bgPivotX": 0.5,
            "bgPivotY": 0.5, "__smartColor": "#DCF2F7", "__bgPos": None, "externalRelPath": None,
            "fieldInstances": [{
                "__identifier": "biome", "__type": "LocalEnum.Biome", "__value": biome,
                "__tile": None, "defUid": UID_LEVEL_FIELD_BIOME,
                "realEditorValues": [{"id": "V_String", "params": [biome]}],
            }],
            "layerInstances": [  # top first, as LDtk lists them
                layer_instance("Entities", "Entities", TILE, W, H, UID_LAYER_ENTITIES,
                               entities=instances),
                layer_instance("Decor", "Tiles", TILE, W, H, UID_LAYER_DECOR, UID_TILES,
                               "tiles.png", tiles=decor),
                layer_instance("Collisions", "IntGrid", TILE, W, H, UID_LAYER_COLLISIONS,
                               UID_TILES, "tiles.png", csv=csv, auto=auto),
                layer_instance("Background", "Tiles", CHAR, bw, bh, UID_LAYER_BACKDROP,
                               UID_BACKDROP, "backgrounds.png", tiles=backdrop),
            ],
            "__neighbours": neighbours,
        })

    solid_icon = rect(UID_TILES, 104, TILES_W, TILE)
    biome_field = field_def(UID_LEVEL_FIELD_BIOME, "biome", "LocalEnum.Biome",
                            f"F_Enum({UID_ENUM_BIOME})", null=True)
    biome_field["doc"] = "Which terrain the Collisions rules draw in this level."
    biome_field["editorAlwaysShow"] = True
    return {
        "__header__": {
            "fileType": "LDtk Project JSON", "app": "LDtk", "doc": "https://ldtk.io/json",
            "schema": "https://ldtk.io/files/JSON_SCHEMA.json",
            "appAuthor": "Sebastien 'deepnight' Benard", "appVersion": "1.5.3",
            "url": "https://ldtk.io",
        },
        "iid": iid("project"), "jsonVersion": "1.5.3", "appBuildId": 473703,
        "nextUid": UID_RULE_BASE + 1000, "identifierStyle": "Capitalize", "toc": [],
        "worldLayout": "GridVania", "worldGridWidth": LEVEL_PX[0], "worldGridHeight": LEVEL_PX[1],
        "defaultLevelWidth": LEVEL_PX[0], "defaultLevelHeight": LEVEL_PX[1],
        "defaultPivotX": 0.5, "defaultPivotY": 1, "defaultGridSize": TILE,
        "defaultEntityWidth": TILE, "defaultEntityHeight": TILE, "bgColor": "#40465B",
        "defaultLevelBgColor": "#C0E8F0", "minifyJson": False, "externalLevels": False,
        "exportTiled": False, "simplifiedExport": False, "imageExportMode": "None",
        "exportLevelBg": True, "pngFilePattern": None, "backupOnSave": False, "backupLimit": 10,
        "backupRelPath": None, "levelNamePattern": "Level_%idx",
        "tutorialDesc": None, "customCommands": [],
        "flags": ["ExportOldTableOfContentData", "UseMultilinesType"],
        "defs": {
            "layers": [  # top first, as the editor's panel shows them
                layer_def(UID_LAYER_ENTITIES, "Entities", "Entities", TILE, colour="#0099DB"),
                layer_def(UID_LAYER_DECOR, "Decor", "Tiles", TILE, UID_TILES, "#7F95AB"),
                layer_def(UID_LAYER_COLLISIONS, "Collisions", "IntGrid", TILE, UID_TILES,
                          "#E4A672",
                          values=[{"value": 1, "identifier": "Solid", "color": "#8F563B",
                                   "tile": solid_icon, "groupUid": 0}],
                          groups=rule_groups()),
                layer_def(UID_LAYER_BACKDROP, "Background", "Tiles", CHAR, UID_BACKDROP, "#3D6E70"),
            ],
            "entities": entity_defs(fields),
            "tilesets": [
                tileset(UID_TILES, "Tiles", "tiles.png", (360, 162), TILE),
                tileset(UID_CHARS, "Characters", "characters.png", (216, 72), CHAR),
                tileset(UID_BACKDROP, "Backgrounds", "backgrounds.png", (192, 72), CHAR),
            ],
            "enums": [
                enum(UID_ENUM_TAGS, "TileTags", ["Solid"]),
                enum(UID_ENUM_ARROW, "Arrow", ["None", "Left", "Right"],
                     {"Left": rect(UID_TILES, 87, TILES_W, TILE),
                      "Right": rect(UID_TILES, 88, TILES_W, TILE)}),
                enum(UID_ENUM_BIOME, "Biome", BIOMES,
                     {b: rect(UID_TILES, TOPS[b]["top"][1], TILES_W, TILE) for b in BIOMES}),
            ],
            "externalEnums": [],
            "levelFields": [biome_field],
        },
        "levels": levels,
        "worlds": [],
        "dummyWorldIid": world_iid,
    }


# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--kenney", type=Path, required=True,
                        help="the folder with the downloaded packs (see PACKS)")
    parser.add_argument("--level", action="store_true", help="also write world.ldtk")
    parser.add_argument("--force", action="store_true",
                        help="overwrite world.ldtk even if it exists (LDtk owns it once saved there)")
    args = parser.parse_args()
    if Image is None:
        sys.exit("make_platformer_assets: needs Pillow (pip install pillow)")

    inputs = find_inputs(args.kenney.expanduser())
    OUT.mkdir(parents=True, exist_ok=True)
    written = []

    def write(name: str, data: bytes) -> None:
        (OUT / name).write_bytes(data)
        written.append(name)

    platformer = inputs["pixel-platformer"]
    for name, path in SHEETS.items():
        write(name, member(platformer, path))
    tiles = Image.open(io.BytesIO(member(platformer, SHEETS["tiles.png"]))).convert("RGBA")
    chars = Image.open(io.BytesIO(member(platformer, SHEETS["characters.png"]))).convert("RGBA")
    for name, data in sprites(tiles, chars).items():
        write(name, data)
    for name, (pack, path) in SOUNDS.items():
        write(name, member(inputs[pack], path))
    write("music.ogg", inputs["music-loop"].read_bytes())
    write("License-kenney.txt", (LICENSE_HEAD + member(platformer, "License.txt").decode(
        "utf-8", "replace").replace("\r\n", "\n").strip() + "\n").encode("utf-8"))

    level = OUT / "world.ldtk"
    if args.level:
        if level.exists() and not args.force:
            sys.exit(f"make_platformer_assets: {level} exists, and once it has been saved by "
                     "LDtk it is LDtk's. --force to overwrite it anyway.")
        level.write_text(json.dumps(build_world(), indent=1) + "\n", encoding="utf-8")
        written.append("world.ldtk")

    print(f"wrote {len(written)} files to {OUT.relative_to(REPO)}: {', '.join(written)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
