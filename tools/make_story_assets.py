#!/usr/bin/env python3
"""The art and sounds of examples/ui/10_dialogue, examples/ui/11_localization and
examples/games/09_after_school.

Every picture, letter and sound is by Kenney (https://kenney.nl), CC0, except
the Japanese letters of 11_localization, which are Jigmo's (Kamichi Koichi,
https://kamichikoichi.github.io/jigmo/, CC0 too). OpenGameArt mirrors the
packs, and the URLs below are those mirrors. They are downloaded by hand ONCE
into one folder and not committed: this script checks each against its sha256,
refuses anything else, and makes from them what IS committed:

  portraits/<who>_<mood>.png    Toon Characters 1's HD poses: idle, cheer, duck
                                and think as neutral, happy, sad and think
  <who>.aseprite                (09) RPG Urban Pack's townsfolk: idle_* and
                                walk_*, facing e, n, w and s
  tiles.png                     (09, 10) RPG Urban Pack's packed sheet
  panel.png, plate.png, button.png, button_down.png
                                UI Pack RPG's frames, for nine-slices
  font.ttf                      Kenney Fonts' Kenpixel High
  jp.ttf                        (11) Jigmo, cut down to the letters lang.tsv's
                                Japanese column uses (fontTools)
  tap.ogg, click.ogg, switch.ogg
                                UI Pack's: a voice's blip, a click, a page
  License-kenney.txt            which pack, which version, where

    python3 tools/make_story_assets.py --kenney ~/Downloads/kenney [--world]
    python3 tools/make_story_assets.py --world [--force]     the town alone

09_after_school keeps these in folders by kind -- maps/, sprites/, portraits/,
ui/, audio/, fonts/ -- and the two UI examples flat.

With --world it also writes
examples/games/09_after_school/resources/maps/world.ldtk, an LDtk 1.5.3 project
of one level, the town: its streets, its school and its café, the forest
around it, laid out in `TOWN` below; every person in it an entity with the
fields its Talker reads, and every tree and street lamp one too, so the game
can draw it over whoever walks behind. Once LDtk has saved it the file is
LDtk's, so this refuses to overwrite it without --force.

Pillow does the cutting and fontTools the subsetting.
"""
from __future__ import annotations

import argparse
import io
import json
import random
import sys
import uuid
import zipfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # main() says so
    Image = None

sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_platformer_assets import aseprite, field_def, real_value, sha256  # noqa: E402
from make_dungeon_assets import layer_def  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
DIALOGUE = REPO / "examples" / "ui" / "10_dialogue" / "resources"
LOCAL = REPO / "examples" / "ui" / "11_localization" / "resources"
SCHOOL = REPO / "examples" / "games" / "09_after_school" / "resources"

PACKS = {
    "toon": ("kenney_toonCharacters1.zip",
             "54e585338e05bb4662da78a1b19283a3dda1f90dbe527353b65a4bb88b5f6297",
             "https://opengameart.org/content/toon-characters-1"),
    "urban": ("kenney_RPGurbanPack.zip",
              "4541d89d639fc7d1e905dd925e55b1c4977a41d983516228db1d57173bb9afaf",
              "https://opengameart.org/content/rpg-urban-pack"),
    "ui-rpg": ("UIpack_RPG.zip",
               "c69c30c09d74df542842e4ec811735b6d260cd6c9e2ee261d7b894d259a6adb4", "https://opengameart.org/content/ui-pack-rpg-extension"),
    "fonts": ("ken_fonts.zip",
              "dbbfbe27de7682ba9ce8f4642b7cc6a88bc3e2c998618c82ef922ae228e66505",
              "https://opengameart.org/content/kenney-fonts"),
    "ui-pack": ("kenney_ui-pack.zip",
                "a8a14a234911eb648c062622915c93e79e94e97cb7f9f375a70f6617f1174318",
                "https://opengameart.org/content/ui-pack"),
    "jigmo": ("Jigmo-20250912.zip",
              "5744c7386d129475d87607ca66d043c8793c65448adeaedc921b6931890e5d0b", "https://kamichikoichi.github.io/jigmo/"),
}

# Who, out of Toon Characters 1, and which of their poses is which mood.
PEOPLE = {"mika": "Female person", "sora": "Male person", "aki": "Female adventurer",
          "kai": "Male adventurer"}
MOODS = {"neutral": "idle", "happy": "cheer0", "sad": "duck", "think": "think"}

UI = {"panel.png": "panel_beige.png", "plate.png": "buttonLong_brown.png",
      "button.png": "buttonLong_beige.png", "button_down.png": "buttonLong_beige_pressed.png"}
SOUNDS = {"tap.ogg": "Sounds/tap-a.ogg", "click.ogg": "Sounds/click-a.ogg",
          "switch.ogg": "Sounds/switch-a.ogg"}

TILE = 16
COLUMNS = 27  # RPG Urban Pack's packed sheet: 27 x 18 tiles

# RPG Urban Pack's townsfolk: three rows each (standing, two steps), four
# columns (side, down, up, the other side). The first row of each, by index.
TOWNSFOLK = {"you": 23, "mika": 428, "sora": 185, "aki": 104, "kai": 347}


def in_folders(name: str) -> str:
    """Where a file of 09_after_school lives below its resources/: a folder
    for each kind (the map's tileset next to the map, as LDtk reads it)."""
    if "/" in name or name.startswith("License"):
        return name
    kind = {".ogg": "audio", ".ttf": "fonts", ".aseprite": "sprites", ".ldtk": "maps"}
    folder = "maps" if name == "tiles.png" else kind.get(Path(name).suffix, "ui")
    return f"{folder}/{name}"


def find_inputs(folder: Path) -> dict[str, Path]:
    found = {}
    for key, (name, digest, url) in PACKS.items():
        path = folder / name
        if not path.is_file():
            sys.exit(f"make_story_assets: {path} is missing. Download it from {url}")
        if sha256(path) != digest:
            sys.exit(f"make_story_assets: {path} is not the pack this was made from "
                     f"(sha256 {sha256(path)}, expected {digest})")
        found[key] = path
    return found


def member(zip_path: Path, suffix: str) -> bytes:
    with zipfile.ZipFile(zip_path) as z:
        names = [n for n in z.namelist() if n.endswith(suffix)]
        if len(names) != 1:
            sys.exit(f"make_story_assets: {zip_path.name} has {len(names)} files ending in "
                     f"{suffix}")
        return z.read(names[0])


def portrait(inputs, who: str, pose: str) -> bytes:
    folder = PEOPLE[who]
    stem = "character_" + folder.split()[0].lower() + folder.split()[1].capitalize()
    return member(inputs["toon"], f"{folder}/PNG/Poses HD/{stem}_{pose}.png")


def townsfolk(sheet, first: int) -> bytes:
    def cell(index):
        x, y = (index % COLUMNS) * TILE, (index // COLUMNS) * TILE
        return sheet.crop((x, y, x + TILE, y + TILE))

    rows = [first, first + COLUMNS, first + 2 * COLUMNS]
    frames, tags = [], []
    # rmp::behavior::TopDown's suffixes: east, north, west, south.
    for name, col in (("w", 0), ("s", 1), ("n", 2), ("e", 3)):
        start = len(frames)
        frames.append((400, cell(rows[0] + col)))
        tags.append((f"idle_{name}", start, start, 0))
        start = len(frames)
        for row in (1, 0, 2, 0):
            frames.append((140, cell(rows[row] + col)))
        tags.append((f"walk_{name}", start, start + 3, 0))
    return aseprite(TILE, TILE, frames, tags)


def japanese_subset(inputs, text: str) -> bytes:
    from fontTools import subset
    from fontTools.ttLib import TTFont
    font = TTFont(io.BytesIO(member(inputs["jigmo"], "Jigmo.ttf")))
    options = subset.Options()
    options.name_IDs = ["*"]
    options.notdef_outline = True
    s = subset.Subsetter(options)
    s.populate(text="".join(sorted(set(text))))
    s.subset(font)
    out = io.BytesIO()
    font.save(out)
    return out.getvalue()


# ---------------------------------------------------------------------------
# The town of 09_after_school
# ---------------------------------------------------------------------------

W, H = 40, 26

# One character a cell: what stands there. The ground under it, the tiles it
# draws and whether it is solid are worked out below.
#
#   F  the forest around the town      .  grass
#   =  street    p  plaza    ~  the pond
#   o  the school's roof   w  its wall   D  its doors   h  its hedge
#   r  the cafe's roof     c  its front  d  its door    m  its crates
#   b  a bench on the grass            B  a bench on the plaza
#   t  a tree, l  a street lamp: entities, drawn over whoever walks behind
TOWN = [
    "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
    "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
    "F............oooooooooooooooo..........F",
    "F.rrrrrrr....oooooooooooooooo....t.....F",
    "F.rrrrrrr....oooooooooooooooo..........F",
    "F.rrrrrrr....oooooooooooooooo......t...F",
    "F.cccdccc....wwwwwwwwwwwwwwww..........F",
    "F.mm.==....l.wwwwwwwDDwwwwwww...t......F",
    "F....==......hhhhhh.==.hhhhhh..........F",
    "F====================================..F",
    "F====================================..F",
    "F..t.....l..........==......~~~~~~~~...F",
    "F...................==......~~~~~~~~.t.F",
    "F.t....pppppppppppp.==......~~~~~~~~...F",
    "F......pppppppppppp.==..b...~~~~~~~~...F",
    "F......ppppBppppppp.==......~~~~~~~~...F",
    "F..t...pppppppppppp.==...............t.F",
    "F......pppppppppppp.==....t.......t....F",
    "F...................==.................F",
    "F.t...t.....t.......==.......t...t.t...F",
    "F...................==.................F",
    "F....t....t.....l...==..l....t....t.t..F",
    "F...................==.................F",
    "F..t.....t....t.....==.....t....t......F",
    "FFFFFFFFFFFFFFFFFFFF==FFFFFFFFFFFFFFFFFF",
    "FFFFFFFFFFFFFFFFFFFF==FFFFFFFFFFFFFFFFFF",
]
assert len(TOWN) == H and all(len(row) == W for row in TOWN), "TOWN is 40 x 26"

GRASS = 28
# Three by three, with their edges: the corner and side tiles of each ground,
# as the sheet lays them out (top-left first).
STREET = [8, 9, 10, 35, 36, 37, 62, 63, 64]
PLAZA = [81, 82, 83, 108, 109, 110, 135, 136, 137]
WATER = [170, 171, 172, 197, 198, 199, 224, 225, 226]
GROUNDS = {"=": STREET, "p": PLAZA, "B": PLAZA, "~": WATER}
# A roof, by row: its left, middle and right tile.
ROOF_SCHOOL = [(125, 126, 127), (152, 153, 154), (179, 180, 181), (206, 207, 208)]
ROOF_CAFE = [(17, 18, 19), (44, 45, 46), (98, 99, 100)]
WALL_SCHOOL = [(89, 90, 91), (143, 144, 145)]  # two rows, framed all round
WINDOW_ARCH, WINDOW_SQUARE, SCHOOL_DOOR = 336, 363, 284  # drawn over the wall
CAFE_FRONT = {"c": (281, 282), "d": (283,)}  # shop windows and shutters; the door
HEDGE = (328, 329, 330)
BENCH = 381
CRATES = (276, 277)
# A tree is two tiles, crown over trunk: which column of the sheet, and its top row.
TREES = {"green": (16, 8), "orange": (16, 11)}
LAMP = (7, 6)  # the green street lamp: its head over its post
SOLID = set("Forwc" "dDhbBm~tl")


def autumn(x: int, y: int) -> str:
    """Most trees have turned."""
    return "green" if (x * 7 + y // 2 * 3) % 3 == 0 else "orange"


def tall(kind: tuple[int, int]) -> tuple[int, int]:
    """The two tiles of something two cells high, top and bottom."""
    col, row = kind
    return row * COLUMNS + col, (row + 1) * COLUMNS + col


def edged(block: list[int], same) -> int:
    """The tile of a three-by-three ground for a cell, from which of its four
    neighbours are the same ground (same(dx, dy))."""
    col = 0 if not same(-1, 0) else 2 if not same(1, 0) else 1
    row = 0 if not same(0, -1) else 2 if not same(0, 1) else 1
    return block[row * 3 + col]


def run(row: str, x: int, chars: str) -> tuple[int, int]:
    """Where the run of `chars` that holds column x starts, and how long it is."""
    first = x
    while first > 0 and row[first - 1] in chars:
        first -= 1
    last = x
    while last + 1 < W and row[last + 1] in chars:
        last += 1
    return first, last - first + 1


def of_three(tiles: tuple, index: int, length: int) -> int:
    """Left, middle or right of a run."""
    return tiles[0] if index == 0 else tiles[2] if index == length - 1 else tiles[1]


def town_tiles():
    """The four tile layers, bottom first -- grass, the ground on it, what is
    built on that, and what is drawn over a wall -- then the solid cells and
    the trees and lamps, which are entities."""
    def at(x, y):
        # Off the map the street goes on, so it has no edge there.
        return TOWN[y][x] if 0 <= x < W and 0 <= y < H else "="

    grass = [(i, GRASS) for i in range(W * H)]
    ground, built, details, solid, standing = [], [], [], [], []
    top_of = {c: next(y for y, row in enumerate(TOWN) if c in row) for c in "orw"}
    for y, row in enumerate(TOWN):
        for x, c in enumerate(row):
            i = y * W + x
            solid.append(1 if c in SOLID else 0)
            if c in GROUNDS:
                block = GROUNDS[c]
                ground.append((i, edged(block, lambda dx, dy: at(x + dx, y + dy) in
                                        [k for k, v in GROUNDS.items() if v is block])))
            if c == "F":
                # A row of trees along the top and the bottom, a column down each side.
                top, bottom = tall(TREES[autumn(x, y)])
                built.append((i, top if y % 2 == 0 else bottom))
            elif c in "or":
                rows = ROOF_SCHOOL if c == "o" else ROOF_CAFE
                first, length = run(row, x, c)
                built.append((i, of_three(rows[y - top_of[c]], x - first, length)))
            elif c in "wD":
                first, length = run(row, x, "wD")
                built.append((i, of_three(WALL_SCHOOL[y - top_of["w"]], x - first, length)))
                if c == "D":
                    details.append((i, SCHOOL_DOOR))
                elif 0 < x - first < length - 1 and (x - first) % 2 == 1:
                    details.append((i, WINDOW_ARCH if y == top_of["w"] else WINDOW_SQUARE))
            elif c in CAFE_FRONT:
                first, _ = run(row, x, "cd")
                front = CAFE_FRONT[c]
                built.append((i, front[(x - first) % len(front)]))
            elif c == "h":
                first, length = run(row, x, "h")
                built.append((i, of_three(HEDGE, x - first, length)))
            elif c in "bB":
                built.append((i, BENCH))
            elif c == "m":
                first, _ = run(row, x, "m")
                built.append((i, CRATES[(x - first) % len(CRATES)]))
            elif c == "t":
                standing.append(("Tree", x, y, {"kind": autumn(x, y)}))
            elif c == "l":
                standing.append(("Lamp", x, y, {}))
    return [grass, ground, built, details], solid, standing


NAMESPACE = uuid.UUID("5d0c4a2e-7a1b-4b9e-9f61-09a5c4001a5d")


def iid(*parts) -> str:
    return str(uuid.uuid5(NAMESPACE, "/".join(str(p) for p in parts)))


def rect(tile: int, tall_by: int = 1) -> dict:
    return {"tilesetUid": UID_TILES, "x": (tile % COLUMNS) * TILE,
            "y": (tile // COLUMNS) * TILE, "w": TILE, "h": TILE * tall_by}


UID_TILES = 1
UID_ENTITIES, UID_WALLS = 20, 24
# The tile layers, as LDtk lists them: the top one first.
TILE_LAYERS = [("Details", 21, "#FEE761"), ("Built", 22, "#7F95AB"),
               ("Ground", 23, "#8B9BB4"), ("Grass", 25, "#6DBE6A")]
UID_ENTITY_BASE, UID_FIELD_BASE = 200, 300

# name: (editor tile, how many tiles high, colour)
ENTITIES = {"Player": (24, 1, "#63C74D"), "Person": (105, 1, "#E43B44"),
            "Flyer": (362, 1, "#FEE761"), "Home": (STREET[4], 1, "#262B44"),
            "Place": (PLAZA[4], 1, "#8B9BB4"),
            "Tree": (tall(TREES["orange"])[0], 2, "#D77643"),
            "Lamp": (tall(LAMP)[0], 2, "#3E8948")}
ENTITY_UIDS = {name: UID_ENTITY_BASE + i for i, name in enumerate(ENTITIES)}

# Who stands where, and what they say (rmp::behavior::Talker::from() reads
# dialogue, node, range and once; the game reads name and look).
CAST = [
    ("Player", 20, 21, {}),
    ("Person", 19, 8, {"name": "Mika", "look": "mika", "node": "Mika"}),
    ("Person", 26, 12, {"name": "Sora", "look": "sora", "node": "Sora"}),
    ("Person", 7, 7, {"name": "Aki", "look": "aki", "node": "Aki"}),
    ("Person", 16, 15, {"name": "Kai", "look": "kai", "node": "Kai"}),
    ("Flyer", 35, 20, {}),
    ("Home", 20, 24, {}),
    # Where a <<walk who place>> of the script goes.
    ("Place", 22, 8, {"name": "gate"}),
    ("Place", 26, 14, {"name": "pond"}),
    ("Place", 4, 8, {"name": "cafe"}),
    ("Place", 9, 16, {"name": "plaza"}),
]

DIALOGUE_FILE = "dialogue/school.yarn"  # below resources/, as every loader takes it


def fields() -> dict[str, list[dict]]:
    uid = iter(range(UID_FIELD_BASE, UID_FIELD_BASE + 100))
    text = lambda name, v: field_def(next(uid), name, "String", "F_String",  # noqa: E731
                                     default={"id": "V_String", "params": [v]})
    num = lambda name, v: field_def(next(uid), name, "Float", "F_Float",  # noqa: E731
                                    default={"id": "V_Float", "params": [v]})
    return {"Person": [text("name", "Mika"), text("look", "mika"),
                       text("dialogue", DIALOGUE_FILE), text("node", "Mika"),
                       num("range", 26)],
            "Home": [text("dialogue", DIALOGUE_FILE), text("node", "Home"), num("range", 20)],
            "Place": [text("name", "gate")],
            "Tree": [text("kind", "orange")]}


def entity_defs(all_fields) -> list[dict]:
    out = []
    for name, (tile, tall_by, colour) in ENTITIES.items():
        single = name in ("Player", "Home")  # one of each
        out.append({
            "identifier": name, "uid": ENTITY_UIDS[name], "tags": [], "exportToToc": False,
            "allowOutOfBounds": False, "doc": None, "width": TILE, "height": TILE,
            "resizableX": False, "resizableY": False, "minWidth": None, "maxWidth": None,
            "minHeight": None, "maxHeight": None, "keepAspectRatio": False, "tileOpacity": 1,
            "fillOpacity": 0.08, "lineOpacity": 0, "hollow": False, "color": colour,
            "renderMode": "Tile", "showName": name not in ("Tree", "Lamp"),
            "tilesetId": UID_TILES,
            "tileRenderMode": "FitInside", "tileRect": rect(tile, tall_by), "uiTileRect": None,
            "nineSliceBorders": [], "maxCount": 1 if single else 0, "limitScope": "PerLevel",
            "limitBehavior": "MoveLastOne", "pivotX": 0.5, "pivotY": 0.5,
            "fieldDefs": all_fields.get(name, []),
        })
    return out


def build_world() -> dict:
    all_fields = fields()
    defs = {n: {f["identifier"]: f for f in all_fields.get(n, [])} for n in ENTITIES}
    layers, solid, standing = town_tiles()
    level_iid = iid("level", "Town")
    names = ["Entities", *(name for name, _, _ in TILE_LAYERS), "Walls"]
    layer_iids = {k: iid("layer", k) for k in names}

    instances = []
    for i, (name, cx, cy, values) in enumerate(CAST + standing):
        if name in ("Tree", "Lamp"):
            assert cy > 0 and TOWN[cy - 1][cx] in ".=p", (name, cx, cy, "its top needs room")
        else:
            assert solid[cy * W + cx] == 0, (name, cx, cy)
        tile, tall_by, colour = ENTITIES[name]
        px = [cx * TILE + TILE // 2, cy * TILE + TILE // 2]
        field_instances = []
        for key, fdef in defs[name].items():
            value = values.get(key, fdef["defaultOverride"]["params"][0])
            field_instances.append({"__identifier": key, "__type": fdef["__type"],
                                    "__value": value, "__tile": None, "defUid": fdef["uid"],
                                    "realEditorValues": [real_value(fdef["__type"], value)]})
        instances.append({
            "__identifier": name, "__grid": [cx, cy], "__pivot": [0.5, 0.5], "__tags": [],
            "__tile": rect(tile, tall_by), "__smartColor": colour,
            "iid": iid("entity", i, name),
            "width": TILE, "height": TILE, "defUid": ENTITY_UIDS[name], "px": px,
            "fieldInstances": field_instances, "__worldX": px[0], "__worldY": px[1],
        })

    def grid(tiles):
        out = []
        for i, t in tiles:
            x, y = i % W, i // W
            out.append({"px": [x * TILE, y * TILE],
                        "src": [(t % COLUMNS) * TILE, (t // COLUMNS) * TILE],
                        "f": 0, "t": t, "d": [i], "a": 1})
        return out

    def layer(name, kind, def_uid, tileset_uid=None, rel=None, **content):
        return {
            "__identifier": name, "__type": kind, "__cWid": W, "__cHei": H, "__gridSize": TILE,
            "__opacity": 1, "__pxTotalOffsetX": 0, "__pxTotalOffsetY": 0,
            "__tilesetDefUid": tileset_uid, "__tilesetRelPath": rel, "iid": layer_iids[name],
            "levelId": 50, "layerDefUid": def_uid, "pxOffsetX": 0, "pxOffsetY": 0,
            "visible": True, "optionalRules": [], "intGridCsv": content.get("csv", []),
            "autoLayerTiles": [], "seed": 9, "overrideTilesetUid": None,
            "gridTiles": content.get("tiles", []), "entityInstances": content.get("entities", []),
        }

    px_w, px_h = W * TILE, H * TILE
    # town_tiles() gives the tile layers bottom first; LDtk lists the top first.
    tile_layers = [layer(name, "Tiles", uid, UID_TILES, "tiles.png", tiles=grid(tiles))
                   for (name, uid, _), tiles in zip(TILE_LAYERS, reversed(layers))]
    level = {
        "identifier": "Town", "iid": level_iid, "uid": 50, "worldX": 0, "worldY": 0,
        "worldDepth": 0, "pxWid": px_w, "pxHei": px_h, "__bgColor": "#6DBE6A",
        "bgColor": "#6DBE6A", "useAutoIdentifier": False, "bgRelPath": None, "bgPos": None,
        "bgPivotX": 0.5, "bgPivotY": 0.5, "__smartColor": "#7A9A63", "__bgPos": None,
        "externalRelPath": None, "fieldInstances": [],
        "layerInstances": [
            layer("Entities", "Entities", UID_ENTITIES, entities=instances),
            *tile_layers,
            layer("Walls", "IntGrid", UID_WALLS, csv=solid),
        ],
        "__neighbours": [],
    }
    return {
        "__header__": {
            "fileType": "LDtk Project JSON", "app": "LDtk", "doc": "https://ldtk.io/json",
            "schema": "https://ldtk.io/files/JSON_SCHEMA.json",
            "appAuthor": "Sebastien 'deepnight' Benard", "appVersion": "1.5.3",
            "url": "https://ldtk.io",
        },
        "iid": iid("project"), "jsonVersion": "1.5.3", "appBuildId": 473703,
        "nextUid": UID_FIELD_BASE + 200, "identifierStyle": "Capitalize", "toc": [],
        "worldLayout": "Free", "worldGridWidth": px_w, "worldGridHeight": px_h,
        "defaultLevelWidth": px_w, "defaultLevelHeight": px_h, "defaultPivotX": 0.5,
        "defaultPivotY": 0.5, "defaultGridSize": TILE, "defaultEntityWidth": TILE,
        "defaultEntityHeight": TILE, "bgColor": "#6DBE6A", "defaultLevelBgColor": "#6DBE6A",
        "minifyJson": False, "externalLevels": False, "exportTiled": False,
        "simplifiedExport": False, "imageExportMode": "None", "exportLevelBg": True,
        "pngFilePattern": None, "backupOnSave": False, "backupLimit": 10, "backupRelPath": None,
        "levelNamePattern": "Level_%idx", "tutorialDesc": None, "customCommands": [],
        "flags": ["ExportOldTableOfContentData", "UseMultilinesType"],
        "defs": {
            "layers": [
                layer_def(UID_ENTITIES, "Entities", "Entities", colour="#0099DB"),
                *(layer_def(uid, name, "Tiles", UID_TILES, colour)
                  for name, uid, colour in TILE_LAYERS),
                layer_def(UID_WALLS, "Walls", "IntGrid", colour="#E4A672",
                          values=[{"value": 1, "identifier": "Solid", "color": "#8F563B",
                                   "tile": rect(tall(TREES["green"])[1]), "groupUid": 0}]),
            ],
            "entities": entity_defs(all_fields),
            "tilesets": [{
                "__cWid": COLUMNS, "__cHei": 18, "identifier": "Tiles", "uid": UID_TILES,
                "relPath": "tiles.png", "embedAtlas": None, "pxWid": COLUMNS * TILE,
                "pxHei": 18 * TILE, "tileGridSize": TILE, "spacing": 0, "padding": 0,
                "tags": [], "tagsSourceEnumUid": None, "enumTags": [], "customData": [],
                "savedSelections": [], "cachedPixelData": None,
            }],
            "enums": [], "externalEnums": [], "levelFields": [],
        },
        "levels": [level],
        "worlds": [],
        "dummyWorldIid": iid("world"),
    }


# ---------------------------------------------------------------------------

LICENSE = """\
The art, letters and sounds in this folder are by Kenney (https://kenney.nl),
released under Creative Commons Zero (CC0 1.0): public domain, free for any
use, with no attribution required -- credited here anyway, because it is his
work.{jigmo}

Made from these, unmodified except for cutting poses, townsfolk and frames out
of the sheets and giving the files short names, by tools/make_story_assets.py,
which also checks every download against its sha256:

  Toon Characters 1      https://kenney.nl/assets/toon-characters-1
      portraits/*.png
  RPG Urban Pack         https://kenney.nl/assets/rpg-urban-pack
      tiles.png, *.aseprite
  UI Pack RPG Expansion  https://kenney.nl/assets/ui-pack-rpg-expansion
      panel.png, plate.png, button.png, button_down.png
  Kenney Fonts           https://kenney.nl/assets/kenney-fonts
      font.ttf (Kenpixel High)
  UI Pack                https://kenney.nl/assets/ui-pack
      tap.ogg, click.ogg, switch.ogg
"""

JIGMO = """

jp.ttf is Jigmo (https://kamichikoichi.github.io/jigmo/), by Kamichi Koichi,
also CC0 1.0, cut down to the letters this example's Japanese uses."""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--kenney", type=Path,
                        help="the folder with the downloaded packs (see PACKS); without it, "
                             "--world alone writes the town and nothing else")
    parser.add_argument("--world", action="store_true", help="also write 09's world.ldtk")
    parser.add_argument("--force", action="store_true",
                        help="overwrite world.ldtk even if it exists (LDtk owns it once saved)")
    args = parser.parse_args()
    written = []
    world = SCHOOL / "maps" / "world.ldtk"
    if args.world:
        if world.exists() and not args.force:
            sys.exit(f"make_story_assets: {world} exists, and once LDtk has saved it it is "
                     "LDtk's. --force to overwrite it anyway.")
        world.parent.mkdir(parents=True, exist_ok=True)
        world.write_text(json.dumps(build_world(), indent=1) + "\n", encoding="utf-8")
        written.append(world.relative_to(REPO).as_posix())
    if args.kenney is None:
        if not args.world:
            parser.error("--kenney FOLDER makes the art; --world alone makes the town")
        print(f"wrote {len(written)} files")
        return 0
    if Image is None:
        sys.exit("make_story_assets: needs Pillow (pip install pillow)")
    inputs = find_inputs(args.kenney.expanduser())

    def write(folder: Path, name: str, data: bytes) -> None:
        # 09_after_school keeps its resources in folders by kind; the two UI
        # examples keep theirs flat.
        if folder == SCHOOL:
            name = in_folders(name)
        path = folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        written.append(path.relative_to(REPO).as_posix())

    tiles_png = member(inputs["urban"], "Tilemap/tilemap_packed.png")
    sheet = Image.open(io.BytesIO(tiles_png)).convert("RGBA")
    font = member(inputs["fonts"], "kenpixel_high.ttf")
    for folder, people in ((DIALOGUE, ("mika", "sora")), (SCHOOL, tuple(PEOPLE))):
        for who in people:
            for mood, pose in MOODS.items():
                write(folder, f"portraits/{who}_{mood}.png", portrait(inputs, who, pose))
        write(folder, "tiles.png", tiles_png)
    for folder in (DIALOGUE, LOCAL, SCHOOL):
        for name, path in UI.items():
            write(folder, name, member(inputs["ui-rpg"], path))
        for name, path in SOUNDS.items():
            write(folder, name, member(inputs["ui-pack"], path))
        write(folder, "font.ttf", font)
        text = LICENSE.format(jigmo=JIGMO if folder == LOCAL else "")
        if folder == SCHOOL:
            text = text.replace("tiles.png, *.aseprite", "maps/tiles.png, sprites/*.aseprite") \
                       .replace("panel.png, plate.png, button.png, button_down.png",
                                "ui/*.png") \
                       .replace("font.ttf (Kenpixel High)", "fonts/font.ttf (Kenpixel High)") \
                       .replace("tap.ogg, click.ogg, switch.ogg", "audio/*.ogg")
        write(folder, "License-kenney.txt", text.encode("utf-8"))
    for who, first in TOWNSFOLK.items():
        write(SCHOOL, f"{who}.aseprite", townsfolk(sheet, first))

    # The Japanese letters the tables of 11_localization use, and the digits.
    table = (LOCAL / "lang.tsv").read_text(encoding="utf-8")
    header = table.splitlines()[0].split("\t")
    ja = header.index("ja") if "ja" in header else -1
    # A name typed in, the numbers, and the other languages' names in the menu.
    letters = "".join(chr(c) for c in range(32, 127)) + "".join(chr(c) for c in range(160, 256))
    for row in table.splitlines()[1:]:
        cells = row.split("\t")
        if ja >= 0 and len(cells) > ja:
            letters += cells[ja]
    write(LOCAL, "jp.ttf", japanese_subset(inputs, letters))

    print(f"wrote {len(written)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
