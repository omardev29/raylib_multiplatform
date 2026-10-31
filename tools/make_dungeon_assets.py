#!/usr/bin/env python3
"""The art, the sounds and the world of examples/games/08_dungeon.

Every picture, letter and sound the dungeon shows or plays is by Kenney
(https://kenney.nl), from five of his packs, all CC0 -- public domain, so they
can sit in the repository as they are. OpenGameArt mirrors them, and the URLs
below are those mirrors. The packs are downloaded by hand ONCE and are not
committed: this script checks each one against its sha256, refuses anything
else, and makes from them what IS committed, in
examples/games/08_dungeon/resources/:

  tiles.png                     Tiny Dungeon's packed sheet, as it is: the map,
                                the items, and every icon the HUD draws
  hero / slime / bat / spider / wizard / ghost / cyclops .aseprite
                                one character of that sheet each, with `idle`,
                                `run`, `hurt` and `die` tags made from it: a
                                breath, a step, a red flash and a fall
  panel.png, button.png, button_down.png, inlay.png
                                Pixel UI Pack's "Ancient" nine-slices
  font.ttf                      Kenney Fonts' Kenpixel
  coin, hit, hurt, swing, door, chest, potion, step, click, hover .ogg
                                from RPG Sounds and UI Pack, by short names
  License-kenney.txt            which pack, which version, where

    python3 tools/make_dungeon_assets.py --kenney ~/Downloads/kenney --world

AND THE WORLD. With --world it also writes world.ldtk: an LDtk 1.5.3 project
of two levels, the Crypt and the Sanctum, generated so that the game has a
world from its first commit. The rooms are carved here (`ROOMS`), the walls go
in an IntGrid whose value `Solid` is what the framework collides with, and the
tiles are written into a Tiles layer by `wall_tile()` / `floor_tile()`. Every
enemy, pickup, trap and door is an entity with the fields its behavior reads
(rmp::behavior::Patrol::from() and the others): open it in LDtk to move them.
Once LDtk has saved it the file is LDtk's, which is why this refuses to
overwrite it without --force.

Pillow does the cutting; it is in the build image and on the runners.
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

REPO = Path(__file__).resolve().parent.parent
OUT = REPO / "examples" / "games" / "08_dungeon" / "resources"

OGA = "https://opengameart.org/sites/default/files/"
PACKS = {
    "tiny-dungeon": ("kenney_tinydungeon.zip",
                     "c109438ab06f65fd80f9b2686a4cf9c7c11dc64444b47333ec71d602f8bb5fc7",
                     "https://opengameart.org/content/tiny-dungeon"),
    "pixel-ui": ("PixelUIpack.zip",
                 "b76f2f60f2be76eb8e66511038fa4d48faec11e638316fa37d06084878cdf0c7",
                 "https://opengameart.org/content/pixel-ui-pack-750-assets"),
    "fonts": ("ken_fonts.zip",
              "dbbfbe27de7682ba9ce8f4642b7cc6a88bc3e2c998618c82ef922ae228e66505",
              "https://opengameart.org/content/kenney-fonts"),
    "rpg-sounds": ("RPGsounds_Kenney.zip",
                   "3ae398ad63e293f9c450bda22d5d81c3af69c74df66fc1400f33c012c0bbc231",
                   "https://opengameart.org/content/50-rpg-sound-effects"),
    "ui-pack": ("kenney_ui-pack.zip",
                "a8a14a234911eb648c062622915c93e79e94e97cb7f9f375a70f6617f1174318",
                "https://opengameart.org/content/ui-pack"),
}

SOUNDS = {
    "coin.ogg": ("rpg-sounds", "OGG/handleCoins.ogg"),
    "hit.ogg": ("rpg-sounds", "OGG/chop.ogg"),
    "hurt.ogg": ("rpg-sounds", "OGG/knifeSlice2.ogg"),
    "swing.ogg": ("rpg-sounds", "OGG/drawKnife2.ogg"),
    "door.ogg": ("rpg-sounds", "OGG/doorOpen_1.ogg"),
    "chest.ogg": ("rpg-sounds", "OGG/creak1.ogg"),
    "potion.ogg": ("rpg-sounds", "OGG/bookFlip2.ogg"),
    "rune.ogg": ("rpg-sounds", "OGG/metalLatch.ogg"),
    "click.ogg": ("ui-pack", "Sounds/click-a.ogg"),
    "hover.ogg": ("ui-pack", "Sounds/tap-a.ogg"),
}

UI = {
    "panel.png": "9-Slice/Ancient/brown.png",
    "button.png": "9-Slice/Ancient/tan.png",
    "button_down.png": "9-Slice/Ancient/tan_pressed.png",
    "inlay.png": "9-Slice/Ancient/brown_inlay.png",
}

TILE = 16
COLUMNS = 12  # tiles.png is 12 x 11 tiles, packed

# The characters of tiles.png, by index.
CHARACTERS = {
    "hero.aseprite": 97,  # the knight
    "slime.aseprite": 108,
    "bat.aseprite": 120,
    "ghost.aseprite": 121,
    "spider.aseprite": 122,
    "wizard.aseprite": 111,
    "cyclops.aseprite": 109,
}


def find_inputs(folder: Path) -> dict[str, Path]:
    found = {}
    for key, (name, digest, url) in PACKS.items():
        path = folder / name
        if not path.is_file():
            sys.exit(f"make_dungeon_assets: {path} is missing. Download it from {url}")
        if sha256(path) != digest:
            sys.exit(f"make_dungeon_assets: {path} is not the pack this was made from "
                     f"(sha256 {sha256(path)}, expected {digest})")
        found[key] = path
    return found


def member(zip_path: Path, suffix: str) -> bytes:
    with zipfile.ZipFile(zip_path) as z:
        names = [n for n in z.namelist() if n.endswith(suffix)]
        if len(names) != 1:
            sys.exit(f"make_dungeon_assets: {zip_path.name} has {len(names)} files ending in {suffix}")
        return z.read(names[0])


# ---------------------------------------------------------------------------
# The characters: one tile each, four tags made from it
# ---------------------------------------------------------------------------

def tile_of(sheet: Image.Image, index: int) -> Image.Image:
    x, y = (index % COLUMNS) * TILE, (index // COLUMNS) * TILE
    return sheet.crop((x, y, x + TILE, y + TILE))


def on_canvas(picture: Image.Image, dy: int = 0, squash: int = 0) -> Image.Image:
    """The tile on a canvas two pixels taller, `dy` lower, `squash` rows shorter."""
    canvas = Image.new("RGBA", (TILE, TILE + 2), (0, 0, 0, 0))
    if squash:
        picture = picture.resize((TILE, TILE - squash), Image.NEAREST)
    canvas.alpha_composite(picture, (0, 1 + dy + squash))
    return canvas


def tinted(picture: Image.Image, rgb: tuple[int, int, int], amount: float) -> Image.Image:
    out = picture.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = px[x, y]
            if a:
                px[x, y] = (int(r + (rgb[0] - r) * amount), int(g + (rgb[1] - g) * amount),
                            int(b + (rgb[2] - b) * amount), a)
    return out


def faded(picture: Image.Image, keep: float) -> Image.Image:
    out = picture.copy()
    alpha = out.getchannel("A").point(lambda v: int(v * keep))
    out.putalpha(alpha)
    return out


def character(sheet: Image.Image, index: int) -> bytes:
    base = tile_of(sheet, index)
    frames = [
        (420, on_canvas(base)),  # idle: a breath
        (420, on_canvas(base, squash=1)),
        (120, on_canvas(base, dy=-1)),  # run: a step up, a step down
        (120, on_canvas(base, dy=1)),
        (90, on_canvas(tinted(base, (255, 70, 60), 0.7))),  # hurt: a red flash
        (90, on_canvas(tinted(base, (255, 255, 255), 0.85))),
        (110, on_canvas(base, squash=4)),  # die: down, and gone
        (110, on_canvas(faded(base, 0.6), squash=8)),
        (400, on_canvas(faded(base, 0.0), squash=8)),
    ]
    tags = [("idle", 0, 1, 0), ("run", 2, 3, 0), ("hurt", 4, 5, 0), ("die", 6, 8, 0)]
    return aseprite(TILE, TILE + 2, frames, tags)


# ---------------------------------------------------------------------------
# The world
# ---------------------------------------------------------------------------

W, H = 40, 24  # cells of a level
LEVEL_PX = (W * TILE, H * TILE)

# Floors and walls, by index into tiles.png.
FLOOR = [48] * 30 + [49] * 8 + [42]
WALL_FACE = [40, 40, 40, 57, 58]
WALL_TOP = [0] * 14 + [12, 24]

# Each level: its rooms (x, y, w, h, inclusive of the floor only), the
# corridors that join them (rectangles of floor too), and solid blocks inside.
LEVELS = [
    {
        "name": "Crypt",
        "title": "The Crypt",
        "rooms": [(2, 2, 9, 7), (14, 2, 12, 8), (29, 2, 9, 8), (2, 13, 13, 9),
                  (18, 14, 9, 7), (30, 14, 8, 8)],
        "corridors": [(11, 4, 3, 3), (26, 4, 3, 3), (5, 9, 3, 4), (15, 16, 3, 3),
                      (27, 16, 3, 3), (33, 10, 1, 4)],
        "blocks": [(19, 5, 2, 2), (5, 16, 2, 2), (10, 16, 2, 2)],
        "entities": [
            ("Player", 5, 5, {}),
            ("Brazier", 2, 2, {}), ("Brazier", 10, 2, {}), ("Brazier", 14, 2, {}),
            ("Brazier", 25, 2, {}), ("Brazier", 29, 2, {}), ("Brazier", 37, 2, {}),
            ("Brazier", 2, 13, {}), ("Brazier", 14, 13, {}), ("Brazier", 30, 14, {}),
            ("Brazier", 37, 14, {}),
            ("Gem", 7, 7, {}), ("Gem", 16, 8, {}), ("Gem", 23, 3, {}), ("Gem", 24, 8, {}),
            ("Slime", 22, 6, {"speed": 46.0, "sight": 110.0}),
            ("Slime", 17, 4, {"speed": 40.0, "sight": 100.0}),
            ("Bat", 15, 3, {"path": [(24, 3), (24, 8), (15, 8)], "speed": 58.0}),
            ("Chest", 34, 3, {"gems": 6}),
            ("Gem", 30, 4, {}), ("Gem", 31, 8, {}), ("Gem", 36, 7, {}),
            ("Lever", 37, 9, {"opens": "Gate"}),
            ("Gate", 33, 11, {}),
            ("Spider", 4, 19, {"path": [(12, 19), (12, 14), (4, 14)], "speed": 34.0,
                               "sight": 120.0, "fov": 110.0}),
            ("Wizard", 13, 20, {"every": 1.8, "speed": 120.0, "count": 3, "spread": 36.0,
                                "range": 130.0}),
            ("Rune", 3, 21, {}), ("Rune", 13, 14, {}),
            ("Altar", 8, 19, {}),
            ("Spikes", 19, 15, {"damage": 1, "knockback": 220.0}),
            ("Spikes", 21, 17, {"damage": 1, "knockback": 220.0}),
            ("Spikes", 23, 15, {"damage": 1, "knockback": 220.0}),
            ("Spikes", 25, 19, {"damage": 1, "knockback": 220.0}),
            ("Spikes", 19, 19, {"damage": 1, "knockback": 220.0}),
            ("Blades", 22, 17, {"radius": 30.0, "speed": 120.0, "angle": 0.0, "blades": 3}),
            ("Potion", 26, 20, {}), ("Gem", 18, 20, {}), ("Gem", 26, 14, {}),
            ("Stairs", 34, 19, {"to": "Sanctum"}),
            ("Gem", 31, 15, {}), ("Gem", 36, 20, {}),
        ],
    },
    {
        "name": "Sanctum",
        "title": "The Sanctum",
        "rooms": [(4, 3, 32, 18)],
        "corridors": [(18, 21, 4, 2)],
        "blocks": [(9, 7, 2, 2), (29, 7, 2, 2), (9, 15, 2, 2), (29, 15, 2, 2)],
        "entities": [
            ("Player", 20, 21, {}),
            ("Brazier", 4, 3, {}), ("Brazier", 35, 3, {}), ("Brazier", 4, 20, {}),
            ("Brazier", 35, 20, {}), ("Brazier", 18, 3, {}), ("Brazier", 21, 3, {}),
            ("Cyclops", 20, 8, {"hp": 14}),
            ("Ghost", 20, 8, {"radius": 34.0, "speed": 90.0, "angle": 0.0}),
            ("Ghost", 20, 8, {"radius": 34.0, "speed": 90.0, "angle": 180.0}),
            ("Potion", 6, 5, {}), ("Potion", 33, 18, {}),
            ("Gem", 6, 18, {}), ("Gem", 33, 5, {}), ("Gem", 14, 12, {}), ("Gem", 26, 12, {}),
            ("Spikes", 15, 5, {"damage": 1, "knockback": 220.0}),
            ("Spikes", 24, 5, {"damage": 1, "knockback": 220.0}),
            ("Exit", 20, 4, {}),
        ],
    },
]

NAMESPACE = uuid.UUID("0b8f4f1e-56a2-4f77-9e2b-08d0f0e0b0a8")


def iid(*parts) -> str:
    return str(uuid.uuid5(NAMESPACE, "/".join(str(p) for p in parts)))


def carve(level: dict) -> list[str]:
    grid = [["#"] * W for _ in range(H)]
    for x, y, w, h in level["rooms"] + level["corridors"]:
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                grid[yy][xx] = "."
    for x, y, w, h in level["blocks"]:
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                grid[yy][xx] = "#"
    return ["".join(r) for r in grid]


def wall_tile(rows: list[str], x: int, y: int, rng: random.Random) -> int:
    below = rows[y + 1][x] if y + 1 < H else "#"
    if below == ".":
        return rng.choice(WALL_FACE)
    return rng.choice(WALL_TOP)


def floor_tile(rng: random.Random) -> int:
    return rng.choice(FLOOR)


UID_TILES = 1
UID_LAYER_ENTITIES, UID_LAYER_TILES, UID_LAYER_WALLS = 20, 21, 22
UID_ENTITY_BASE, UID_FIELD_BASE = 200, 300

# name: (editor tile, colour, pivot)
ENTITIES = {
    "Player": (97, "#63C74D"), "Slime": (108, "#3E8948"), "Bat": (120, "#B55088"),
    "Spider": (122, "#5A6988"), "Wizard": (111, "#68386C"), "Cyclops": (109, "#E43B44"),
    "Ghost": (121, "#C0CBDC"), "Gem": (102, "#2CE8F5"), "Potion": (115, "#E43B44"),
    "Chest": (89, "#C28569"), "Lever": (80, "#FEAE34"), "Gate": (45, "#8F563B"),
    "Rune": (101, "#FEE761"), "Altar": (64, "#8B9BB4"), "Spikes": (41, "#E43B44"),
    "Blades": (65, "#C0CBDC"), "Brazier": (29, "#F77622"), "Stairs": (56, "#262B44"),
    "Exit": (60, "#FEE761"),
}
ENTITY_UIDS = {name: UID_ENTITY_BASE + i for i, name in enumerate(ENTITIES)}


def rect(tile: int) -> dict:
    return {"tilesetUid": UID_TILES, "x": (tile % COLUMNS) * TILE, "y": (tile // COLUMNS) * TILE,
            "w": TILE, "h": TILE}


def fields() -> dict[str, list[dict]]:
    uid = iter(range(UID_FIELD_BASE, UID_FIELD_BASE + 200))
    f = lambda v: {"id": "V_Float", "params": [v]}  # noqa: E731
    i = lambda v: {"id": "V_Int", "params": [v]}  # noqa: E731
    path = lambda: field_def(next(uid), "path", "Array<Point>", "F_Point", array=True,  # noqa: E731
                             display="PointPath")
    num = lambda name, v: field_def(next(uid), name, "Float", "F_Float", default=f(v))  # noqa: E731
    whole = lambda name, v: field_def(next(uid), name, "Int", "F_Int", default=i(v))  # noqa: E731
    return {
        "Slime": [num("speed", 40), num("sight", 100)],
        "Bat": [path(), num("speed", 50)],
        "Spider": [path(), num("speed", 30), num("sight", 120), num("fov", 110)],
        "Wizard": [num("every", 1.8), num("speed", 120), whole("count", 3), num("spread", 36),
                   num("range", 130)],
        "Cyclops": [whole("hp", 14)],
        "Ghost": [num("radius", 34), num("speed", 90), num("angle", 0)],
        "Chest": [whole("gems", 5)],
        "Lever": [field_def(next(uid), "opens", "EntityRef", "F_EntityRef", null=True,
                            display="RefLinkBetweenCenters", ref_entity=ENTITY_UIDS["Gate"])],
        "Spikes": [whole("damage", 1), num("knockback", 200)],
        "Blades": [num("radius", 30), num("speed", 120), num("angle", 0), whole("blades", 3)],
        "Stairs": [field_def(next(uid), "to", "String", "F_String",
                             default={"id": "V_String", "params": ["Sanctum"]})],
    }


def entity_defs(all_fields) -> list[dict]:
    out = []
    for name, (tile, colour) in ENTITIES.items():
        single = name in ("Player", "Cyclops", "Exit")
        out.append({
            "identifier": name, "uid": ENTITY_UIDS[name], "tags": [], "exportToToc": False,
            "allowOutOfBounds": False, "doc": None, "width": TILE, "height": TILE,
            "resizableX": False, "resizableY": False, "minWidth": None, "maxWidth": None,
            "minHeight": None, "maxHeight": None, "keepAspectRatio": False, "tileOpacity": 1,
            "fillOpacity": 0.08, "lineOpacity": 0, "hollow": False, "color": colour,
            "renderMode": "Tile", "showName": single, "tilesetId": UID_TILES,
            "tileRenderMode": "FitInside", "tileRect": rect(tile), "uiTileRect": None,
            "nineSliceBorders": [], "maxCount": 1 if single else 0,
            "limitScope": "PerLevel", "limitBehavior": "MoveLastOne", "pivotX": 0.5,
            "pivotY": 0.5, "fieldDefs": all_fields.get(name, []),
        })
    return out


def layer_def(uid, identifier, kind, tileset=None, colour="#000000", values=()):
    return {
        "__type": kind, "identifier": identifier, "type": kind, "uid": uid, "doc": None,
        "uiColor": colour, "gridSize": TILE, "guideGridWid": 0, "guideGridHei": 0,
        "displayOpacity": 1, "inactiveOpacity": 1, "hideInList": False,
        "hideFieldsWhenInactive": kind != "Entities", "canSelectWhenInactive": True,
        "renderInWorldView": True, "pxOffsetX": 0, "pxOffsetY": 0, "parallaxFactorX": 0,
        "parallaxFactorY": 0, "parallaxScaling": True, "requiredTags": [], "excludedTags": [],
        "autoTilesKilledByOtherLayerUid": None, "uiFilterTags": [], "useAsyncRender": False,
        "intGridValues": list(values), "intGridValuesGroups": [], "autoRuleGroups": [],
        "autoSourceLayerDefUid": None, "tilesetDefUid": tileset, "tilePivotX": 0, "tilePivotY": 0,
        "biomeFieldUid": None,
    }


def build_world() -> dict:
    all_fields = fields()
    defs_by_name = {n: {f["identifier"]: f for f in all_fields.get(n, [])} for n in ENTITIES}
    world_iid = iid("world")
    levels = []
    for li, lv in enumerate(LEVELS):
        rows = carve(lv)
        rng = random.Random(1000 + li)
        level_iid = iid("level", lv["name"])
        layer_iids = {k: iid("layer", lv["name"], k) for k in ("Entities", "Tiles", "Walls")}
        level_uid = 50 + li
        entity_iids = [iid("entity", lv["name"], ei, e[0]) for ei, e in enumerate(lv["entities"])]

        instances = []
        for ei, (name, cx, cy, values) in enumerate(lv["entities"]):
            assert rows[cy][cx] == "." or name in ("Gate", "Brazier"), (lv["name"], name, cx, cy)
            tile, colour = ENTITIES[name]
            px = [cx * TILE + TILE // 2, cy * TILE + TILE // 2]
            field_instances = []
            for key, fdef in defs_by_name[name].items():
                kind = fdef["__type"]
                value = values.get(key)
                if value is None and fdef["defaultOverride"] is not None:
                    value = fdef["defaultOverride"]["params"][0]
                if kind == "Array<Point>":
                    exported = [{"cx": x, "cy": y} for x, y in value or []]
                    real = [real_value(kind, p) for p in value or []]
                elif kind == "EntityRef":
                    target = next(e for e, (n, *_r) in enumerate(lv["entities"]) if n == value)
                    exported = {"entityIid": entity_iids[target],
                                "layerIid": layer_iids["Entities"], "levelIid": level_iid,
                                "worldIid": world_iid}
                    real = [real_value(kind, entity_iids[target])]
                else:
                    exported = value
                    real = [real_value(kind, value)]
                field_instances.append({"__identifier": key, "__type": kind, "__value": exported,
                                        "__tile": None, "defUid": fdef["uid"],
                                        "realEditorValues": real})
            instances.append({
                "__identifier": name, "__grid": [cx, cy], "__pivot": [0.5, 0.5], "__tags": [],
                "__tile": rect(tile), "__smartColor": colour, "iid": entity_iids[ei],
                "width": TILE, "height": TILE, "defUid": ENTITY_UIDS[name], "px": px,
                "fieldInstances": field_instances,
                "__worldX": li * (LEVEL_PX[0] + 4 * TILE) + px[0], "__worldY": px[1],
            })

        csv, tiles = [], []
        for y in range(H):
            for x in range(W):
                solid = rows[y][x] == "#"
                csv.append(1 if solid else 0)
                t = wall_tile(rows, x, y, rng) if solid else floor_tile(rng)
                tiles.append({"px": [x * TILE, y * TILE],
                              "src": [(t % COLUMNS) * TILE, (t // COLUMNS) * TILE],
                              "f": 0, "t": t, "d": [x + y * W], "a": 1})

        def layer(name, kind, def_uid, tileset_uid=None, rel=None, **content):
            return {
                "__identifier": name, "__type": kind, "__cWid": W, "__cHei": H,
                "__gridSize": TILE, "__opacity": 1, "__pxTotalOffsetX": 0, "__pxTotalOffsetY": 0,
                "__tilesetDefUid": tileset_uid, "__tilesetRelPath": rel,
                "iid": layer_iids[name], "levelId": level_uid, "layerDefUid": def_uid,
                "pxOffsetX": 0, "pxOffsetY": 0, "visible": True, "optionalRules": [],
                "intGridCsv": content.get("csv", []), "autoLayerTiles": [],
                "seed": 77 + li, "overrideTilesetUid": None,
                "gridTiles": content.get("tiles", []),
                "entityInstances": content.get("entities", []),
            }

        levels.append({
            "identifier": lv["name"], "iid": level_iid, "uid": level_uid,
            "worldX": li * (LEVEL_PX[0] + 4 * TILE), "worldY": 0, "worldDepth": 0,
            "pxWid": LEVEL_PX[0], "pxHei": LEVEL_PX[1], "__bgColor": "#25131A",
            "bgColor": "#25131A", "useAutoIdentifier": False, "bgRelPath": None, "bgPos": None,
            "bgPivotX": 0.5, "bgPivotY": 0.5, "__smartColor": "#7A5A63", "__bgPos": None,
            "externalRelPath": None, "fieldInstances": [],
            "layerInstances": [
                layer("Entities", "Entities", UID_LAYER_ENTITIES, entities=instances),
                layer("Tiles", "Tiles", UID_LAYER_TILES, UID_TILES, "tiles.png", tiles=tiles),
                layer("Walls", "IntGrid", UID_LAYER_WALLS, csv=csv),
            ],
            "__neighbours": [],
        })

    return {
        "__header__": {
            "fileType": "LDtk Project JSON", "app": "LDtk", "doc": "https://ldtk.io/json",
            "schema": "https://ldtk.io/files/JSON_SCHEMA.json",
            "appAuthor": "Sebastien 'deepnight' Benard", "appVersion": "1.5.3",
            "url": "https://ldtk.io",
        },
        "iid": iid("project"), "jsonVersion": "1.5.3", "appBuildId": 473703,
        "nextUid": UID_FIELD_BASE + 300, "identifierStyle": "Capitalize", "toc": [],
        "worldLayout": "Free", "worldGridWidth": LEVEL_PX[0], "worldGridHeight": LEVEL_PX[1],
        "defaultLevelWidth": LEVEL_PX[0], "defaultLevelHeight": LEVEL_PX[1],
        "defaultPivotX": 0.5, "defaultPivotY": 0.5, "defaultGridSize": TILE,
        "defaultEntityWidth": TILE, "defaultEntityHeight": TILE, "bgColor": "#25131A",
        "defaultLevelBgColor": "#25131A", "minifyJson": False, "externalLevels": False,
        "exportTiled": False, "simplifiedExport": False, "imageExportMode": "None",
        "exportLevelBg": True, "pngFilePattern": None, "backupOnSave": False, "backupLimit": 10,
        "backupRelPath": None, "levelNamePattern": "Level_%idx", "tutorialDesc": None,
        "customCommands": [], "flags": ["ExportOldTableOfContentData", "UseMultilinesType"],
        "defs": {
            "layers": [
                layer_def(UID_LAYER_ENTITIES, "Entities", "Entities", colour="#0099DB"),
                layer_def(UID_LAYER_TILES, "Tiles", "Tiles", UID_TILES, "#7F95AB"),
                layer_def(UID_LAYER_WALLS, "Walls", "IntGrid", colour="#E4A672",
                          values=[{"value": 1, "identifier": "Solid", "color": "#8F563B",
                                   "tile": rect(0), "groupUid": 0}]),
            ],
            "entities": entity_defs(all_fields),
            "tilesets": [{
                "__cWid": COLUMNS, "__cHei": 11, "identifier": "Tiles", "uid": UID_TILES,
                "relPath": "tiles.png", "embedAtlas": None, "pxWid": COLUMNS * TILE,
                "pxHei": 11 * TILE, "tileGridSize": TILE, "spacing": 0, "padding": 0,
                "tags": [], "tagsSourceEnumUid": None, "enumTags": [], "customData": [],
                "savedSelections": [], "cachedPixelData": None,
            }],
            "enums": [], "externalEnums": [], "levelFields": [],
        },
        "levels": levels,
        "worlds": [],
        "dummyWorldIid": world_iid,
    }


# ---------------------------------------------------------------------------

LICENSE_HEAD = """\
The art, letters and sounds in this folder are by Kenney (https://kenney.nl),
released under Creative Commons Zero (CC0 1.0): public domain, free for any
use, with no attribution required -- credited here anyway, because it is his
work.

Made from these, unmodified except for cutting characters and nine-slices out
of the sheets, giving the characters a few frames made from their own pixels,
and giving the files short names, by tools/make_dungeon_assets.py, which also
checks every download against its sha256:

  Tiny Dungeon 1.0       https://kenney.nl/assets/tiny-dungeon
      tiles.png, *.aseprite
  Pixel UI Pack          https://kenney.nl/assets/pixel-ui-pack
      panel.png, button.png, button_down.png, inlay.png
  Kenney Fonts           https://kenney.nl/assets/kenney-fonts
      font.ttf (Kenpixel)
  RPG Audio              https://kenney.nl/assets/rpg-audio
      coin, hit, hurt, swing, door, chest, potion, rune .ogg
  UI Pack 2.0            https://kenney.nl/assets/ui-pack
      click.ogg, hover.ogg

world.ldtk is this repository's: the two levels, generated by the same script,
to be edited in LDtk (https://ldtk.io).

Tiny Dungeon's own licence file follows.

------------------------------------------------------------------------------
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--kenney", type=Path, required=True,
                        help="the folder with the downloaded packs (see PACKS)")
    parser.add_argument("--world", action="store_true", help="also write world.ldtk")
    parser.add_argument("--force", action="store_true",
                        help="overwrite world.ldtk even if it exists (LDtk owns it once saved there)")
    args = parser.parse_args()
    if Image is None:
        sys.exit("make_dungeon_assets: needs Pillow (pip install pillow)")

    inputs = find_inputs(args.kenney.expanduser())
    OUT.mkdir(parents=True, exist_ok=True)
    written = []

    def write(name: str, data: bytes) -> None:
        (OUT / name).write_bytes(data)
        written.append(name)

    tiles_png = member(inputs["tiny-dungeon"], "Tilemap/tilemap_packed.png")
    write("tiles.png", tiles_png)
    sheet = Image.open(io.BytesIO(tiles_png)).convert("RGBA")
    for name, index in CHARACTERS.items():
        write(name, character(sheet, index))
    for name, path in UI.items():
        write(name, member(inputs["pixel-ui"], path))
    write("font.ttf", member(inputs["fonts"], "kenpixel.ttf"))
    for name, (pack, path) in SOUNDS.items():
        write(name, member(inputs[pack], path))
    write("License-kenney.txt", (LICENSE_HEAD + member(inputs["tiny-dungeon"], "License.txt")
                                 .decode("utf-8", "replace").replace("\r\n", "\n").strip()
                                 + "\n").encode("utf-8"))

    world = OUT / "world.ldtk"
    if args.world:
        if world.exists() and not args.force:
            sys.exit(f"make_dungeon_assets: {world} exists, and once LDtk has saved it it is "
                     "LDtk's. --force to overwrite it anyway.")
        world.write_text(json.dumps(build_world(), indent=1) + "\n", encoding="utf-8")
        written.append("world.ldtk")

    print(f"wrote {len(written)} files to {OUT.relative_to(REPO)}: {', '.join(written)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
