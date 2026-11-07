#!/usr/bin/env python3
"""The art and sounds of examples/ui/10_dialogue, examples/ui/11_localization and
examples/games/09_after_school.

Every picture, letter and sound is by Kenney (https://kenney.nl), CC0, except
the Japanese letters of 11_localization, which are Jigmo's (Kamichi Koichi,
https://kamichikoichi.github.io/jigmo/, CC0 too). OpenGameArt mirrors the
packs, and the URLs below are those mirrors. They are downloaded by hand ONCE
into one folder and not committed: this script checks each against its sha256,
refuses anything else, and makes from them what IS committed:

  portraits/<who>_<mood>.png    Toon Characters 1's HD poses: idle, cheer, hurt
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

With --world it also writes examples/games/09_after_school/resources/world.ldtk,
an LDtk 1.5.3 project of one level, the town: its paths, its trees, its school
and its café, laid out in `TOWN` below, and every person in it an entity with
the fields its Talker reads. Once LDtk has saved it the file is LDtk's, so this
refuses to overwrite it without --force.

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
MOODS = {"neutral": "idle", "happy": "cheer0", "sad": "hurt", "think": "think"}

UI = {"panel.png": "panel_beige.png", "plate.png": "buttonLong_brown.png",
      "button.png": "buttonLong_beige.png", "button_down.png": "buttonLong_beige_pressed.png"}
SOUNDS = {"tap.ogg": "Sounds/tap-a.ogg", "click.ogg": "Sounds/click-a.ogg",
          "switch.ogg": "Sounds/switch-a.ogg"}

TILE = 16
COLUMNS = 27  # RPG Urban Pack's packed sheet: 27 x 18 tiles

# RPG Urban Pack's townsfolk: three rows each (standing, two steps), four
# columns (side, down, up, the other side). The first row of each, by index.
TOWNSFOLK = {"you": 23, "mika": 428, "sora": 185, "aki": 104, "kai": 347}


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

# One character a cell. What each draws is TILES below; # marks what is solid
# besides (the walls, the pond, the trees' trunks, the hedges).
TOWN = [
    "TTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTTT",
    "T...........oooooooooooooooo..........tT",
    "T.rrrrrrr...oooooooooooooooo....f.....tT",
    "T.rrrrrrr...oooooooooooooooo..........tT",
    "T.rrrrrrr...wwwwwwwwwwwwwwww...t...t..TT",
    "T.wwwdwww...wwwwwwwDDwwwwwww..........TT",
    "T...=.......hhhhhhh..hhhhhhh....b.....tT",
    "T...=...............................t.TT",
    "T=====================================.T",
    "T=====================================.T",
    "T.t.......b.........==........~~~~~~..TT",
    "T.........f.........==.......~~~~~~~~..T",
    "T.....t.............==.......~~~~~~~~..T",
    "Tt..................==.......~~~~~~~~.tT",
    "T.......pppppppppppp==pppp....~~~~~~...T",
    "T..t....pppppppppppp==pppp.............T",
    "T.......pppbpppppppp==ppppb........t...T",
    "T..t....pppppppppppp==pppp.............T",
    "T.......pppppppppppp==pppp....t....f...T",
    "Tt...t..............==.............t..tT",
    "T...................==.................T",
    "T..f..t.......t.....==......t......t...T",
    "T...................==.................T",
    "Tt..t...t....t......==.....t....t...t..T",
    "T...................==.................T",
    "TTTTTTTTTTTTTTTTTTTT==TTTTTTTTTTTTTTTTTT",
]

GRASS = [28] * 12 + [0 + 28]
STONE = 36
PLAZA = 109
WATER = 198
ROOF_ORANGE = {"top": [125, 126, 127, 128, 129, 130], "mid": [152, 153, 154, 155, 156, 157]}
ROOF_RED = [44, 45, 46, 71, 72, 73]
WALL = [281, 336]
SCHOOL_DOOR = 284
CAFE_DOOR = 283
TREE_TOP, TREE_TRUNK = 259, 286
TREE_ORANGE_TOP, TREE_ORANGE_TRUNK = 313, 340
HEDGE = [328, 329, 330, 331]
BENCH = 381
FLOWERS = [276, 277, 303, 304]
LAMP = 162


def town_tiles(rng: random.Random):
    """Two layers -- the ground and what stands on it -- and the solid cells."""
    ground, over, solid = [], [], []
    for y, row in enumerate(TOWN):
        for x, c in enumerate(row):
            g = rng.choice(GRASS)
            o = None
            hard = False
            if c in "=":
                g = STONE
            elif c == "p":
                g = PLAZA
            elif c == "~":
                g, hard = WATER, True
            elif c == "o":
                part = "top" if y == 1 else "mid"
                o, hard = ROOF_ORANGE[part][(x - 12) % 6], True
            elif c == "r":
                o, hard = ROOF_RED[(x - 2) % 3 + (3 if y > 2 else 0)], True
            elif c == "w":
                o, hard = WALL[x % len(WALL)], True
            elif c == "D":
                o, hard = SCHOOL_DOOR, True
            elif c == "d":
                o, hard = CAFE_DOOR, True
            elif c == "h":
                o, hard = HEDGE[x % len(HEDGE)], True
            elif c in "tT":
                # Autumn: most trees have turned.
                o, hard = (TREE_TRUNK if (x * 7 + y) % 6 == 0 else TREE_ORANGE_TRUNK), True
            elif c == "b":
                o, hard = BENCH, True
            elif c == "f":
                o = FLOWERS[(x + y) % len(FLOWERS)]
            ground.append(g)
            over.append(o)
            solid.append(1 if hard else 0)
    # Every tree's crown, a cell above its trunk, drawn over whatever is there.
    crowns = []
    for i, o in enumerate(over):
        if o in (TREE_TRUNK, TREE_ORANGE_TRUNK) and i >= W:
            crowns.append((i - W, TREE_TOP if o == TREE_TRUNK else TREE_ORANGE_TOP))
    return ground, over, crowns, solid


NAMESPACE = uuid.UUID("5d0c4a2e-7a1b-4b9e-9f61-09a5c4001a5d")


def iid(*parts) -> str:
    return str(uuid.uuid5(NAMESPACE, "/".join(str(p) for p in parts)))


def rect(tile: int) -> dict:
    return {"tilesetUid": UID_TILES, "x": (tile % COLUMNS) * TILE,
            "y": (tile // COLUMNS) * TILE, "w": TILE, "h": TILE}


UID_TILES = 1
UID_ENTITIES, UID_OVER, UID_CROWNS, UID_GROUND, UID_WALLS = 20, 21, 22, 23, 24
UID_ENTITY_BASE, UID_FIELD_BASE = 200, 300

# name: (editor tile, colour)
ENTITIES = {"Player": (24, "#63C74D"), "Person": (105, "#E43B44"),
            "Flyer": (362, "#FEE761"), "Home": (STONE, "#262B44"),
            "Place": (PLAZA, "#8B9BB4")}
ENTITY_UIDS = {name: UID_ENTITY_BASE + i for i, name in enumerate(ENTITIES)}

# Who stands where, and what they say (rmp::behavior::Talker::from() reads
# dialogue, node, range and once; the game reads name and look).
CAST = [
    ("Player", 20, 21, {}),
    ("Person", 18, 7, {"name": "Mika", "look": "mika", "node": "Mika"}),
    ("Person", 28, 12, {"name": "Sora", "look": "sora", "node": "Sora"}),
    ("Person", 6, 9, {"name": "Aki", "look": "aki", "node": "Aki"}),
    ("Person", 24, 16, {"name": "Kai", "look": "kai", "node": "Kai"}),
    ("Flyer", 35, 20, {}),
    ("Home", 20, 24, {}),
    # Where a <<walk who place>> of the script goes.
    ("Place", 20, 7, {"name": "gate"}),
    ("Place", 27, 15, {"name": "pond"}),
    ("Place", 5, 7, {"name": "cafe"}),
    ("Place", 14, 16, {"name": "plaza"}),
]


def fields() -> dict[str, list[dict]]:
    uid = iter(range(UID_FIELD_BASE, UID_FIELD_BASE + 100))
    text = lambda name, v: field_def(next(uid), name, "String", "F_String",  # noqa: E731
                                     default={"id": "V_String", "params": [v]})
    num = lambda name, v: field_def(next(uid), name, "Float", "F_Float",  # noqa: E731
                                    default={"id": "V_Float", "params": [v]})
    return {"Person": [text("name", "Mika"), text("look", "mika"),
                       text("dialogue", "school.yarn"), text("node", "Mika"),
                       num("range", 26)],
            "Home": [text("dialogue", "school.yarn"), text("node", "Home"), num("range", 20)],
            "Place": [text("name", "gate")]}


def entity_defs(all_fields) -> list[dict]:
    out = []
    for name, (tile, colour) in ENTITIES.items():
        single = name in ("Player", "Home")  # one of each
        out.append({
            "identifier": name, "uid": ENTITY_UIDS[name], "tags": [], "exportToToc": False,
            "allowOutOfBounds": False, "doc": None, "width": TILE, "height": TILE,
            "resizableX": False, "resizableY": False, "minWidth": None, "maxWidth": None,
            "minHeight": None, "maxHeight": None, "keepAspectRatio": False, "tileOpacity": 1,
            "fillOpacity": 0.08, "lineOpacity": 0, "hollow": False, "color": colour,
            "renderMode": "Tile", "showName": True, "tilesetId": UID_TILES,
            "tileRenderMode": "FitInside", "tileRect": rect(tile), "uiTileRect": None,
            "nineSliceBorders": [], "maxCount": 1 if single else 0, "limitScope": "PerLevel",
            "limitBehavior": "MoveLastOne", "pivotX": 0.5, "pivotY": 0.5,
            "fieldDefs": all_fields.get(name, []),
        })
    return out


def build_world() -> dict:
    all_fields = fields()
    defs = {n: {f["identifier"]: f for f in all_fields.get(n, [])} for n in ENTITIES}
    rng = random.Random(9)
    ground, over, crowns, solid = town_tiles(rng)
    level_iid = iid("level", "Town")
    layer_iids = {k: iid("layer", k) for k in ("Entities", "Over", "Crowns", "Ground", "Walls")}

    instances = []
    for i, (name, cx, cy, values) in enumerate(CAST):
        assert solid[cy * W + cx] == 0, (name, cx, cy)
        tile, colour = ENTITIES[name]
        px = [cx * TILE + TILE // 2, cy * TILE + TILE // 2]
        field_instances = []
        for key, fdef in defs[name].items():
            value = values.get(key, fdef["defaultOverride"]["params"][0])
            field_instances.append({"__identifier": key, "__type": fdef["__type"],
                                    "__value": value, "__tile": None, "defUid": fdef["uid"],
                                    "realEditorValues": [real_value(fdef["__type"], value)]})
        instances.append({
            "__identifier": name, "__grid": [cx, cy], "__pivot": [0.5, 0.5], "__tags": [],
            "__tile": rect(tile), "__smartColor": colour, "iid": iid("entity", i, name),
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
    level = {
        "identifier": "Town", "iid": level_iid, "uid": 50, "worldX": 0, "worldY": 0,
        "worldDepth": 0, "pxWid": px_w, "pxHei": px_h, "__bgColor": "#6DBE6A",
        "bgColor": "#6DBE6A", "useAutoIdentifier": False, "bgRelPath": None, "bgPos": None,
        "bgPivotX": 0.5, "bgPivotY": 0.5, "__smartColor": "#7A9A63", "__bgPos": None,
        "externalRelPath": None, "fieldInstances": [],
        "layerInstances": [
            layer("Entities", "Entities", UID_ENTITIES, entities=instances),
            layer("Crowns", "Tiles", UID_CROWNS, UID_TILES, "tiles.png", tiles=grid(crowns)),
            layer("Over", "Tiles", UID_OVER, UID_TILES, "tiles.png",
                  tiles=grid([(i, t) for i, t in enumerate(over) if t is not None])),
            layer("Ground", "Tiles", UID_GROUND, UID_TILES, "tiles.png",
                  tiles=grid(list(enumerate(ground)))),
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
                layer_def(UID_CROWNS, "Crowns", "Tiles", UID_TILES, "#4E9A06"),
                layer_def(UID_OVER, "Over", "Tiles", UID_TILES, "#7F95AB"),
                layer_def(UID_GROUND, "Ground", "Tiles", UID_TILES, "#6DBE6A"),
                layer_def(UID_WALLS, "Walls", "IntGrid", colour="#E4A672",
                          values=[{"value": 1, "identifier": "Solid", "color": "#8F563B",
                                   "tile": rect(TREE_TRUNK), "groupUid": 0}]),
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
    parser.add_argument("--kenney", type=Path, required=True,
                        help="the folder with the downloaded packs (see PACKS)")
    parser.add_argument("--world", action="store_true", help="also write 09's world.ldtk")
    parser.add_argument("--force", action="store_true",
                        help="overwrite world.ldtk even if it exists (LDtk owns it once saved)")
    args = parser.parse_args()
    if Image is None:
        sys.exit("make_story_assets: needs Pillow (pip install pillow)")
    inputs = find_inputs(args.kenney.expanduser())
    written = []

    def write(folder: Path, name: str, data: bytes) -> None:
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
        write(folder, "License-kenney.txt",
              LICENSE.format(jigmo=JIGMO if folder == LOCAL else "").encode("utf-8"))
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

    world = SCHOOL / "world.ldtk"
    if args.world:
        if world.exists() and not args.force:
            sys.exit(f"make_story_assets: {world} exists, and once LDtk has saved it it is "
                     "LDtk's. --force to overwrite it anyway.")
        world.write_text(json.dumps(build_world(), indent=1) + "\n", encoding="utf-8")
        written.append(world.relative_to(REPO).as_posix())

    print(f"wrote {len(written)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
