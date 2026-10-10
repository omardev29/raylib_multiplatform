#pragma once

#include <rmp/assets.h> // rmp::Texture, held by a Tileset
#include <rmp/scene.h> // rmp::Scene, for the factories' Callback
#include <rmp/tilemap.h> // MapData, MapPtr, MapObject

#include <raylib.h> // Rectangle, Vector2

#include <memory>
#include <string>
#include <utility>
#include <vector>
// ---------------------------------------------------------------------------
// Private to src/rmp/. The parsed map, as BOTH readers fill it.
//
// There are two readers -- src/rmp/tilemap.cpp reads Tiled JSON through
// cute_tiled, src/rmp/ldtk.cpp reads LDtk projects through cJSON -- and one
// map: whatever they read lands in the structures below, and everything after
// parsing (drawing, solids, objects, levels) knows nothing about which editor
// made it. That is what keeps rmp::Tilemap one class with one API.
//
// tests/tilemap_test.cpp and tests/ldtk_test.cpp include this directly:
// parsing is arithmetic, so dimensions, tiles, solids, object positions and
// properties, the factories and the levels all run without a window. Only the
// tileset texture and draw() need one.
// ---------------------------------------------------------------------------

namespace rmp::tilemap::detail {

// Tiled's three flip bits, masked off every gid it writes.
inline constexpr unsigned FLIP_MASK = 0xE0000000U;

// A tileset, with its tiles numbered first_gid..last_gid in the MAP's
// numbering. Tiled hands those numbers out itself; LDtk does not number tiles
// across tilesets, so its reader gives each tileset a range in turn.
struct Tileset {
    int first_gid = 0;
    int last_gid = 0;
    int columns = 0;
    int tile_width = 0;
    int tile_height = 0;
    int margin = 0; // border between the image edge and the first tile
    int spacing = 0; // gutter between one tile and the next
    rmp::Texture texture;
    std::vector<bool> solid; // by local tile id
};

// One tile put down at a pixel position, which is how LDtk stores them: a
// cell can hold several (its auto-layers stack them), and each can be flipped
// and faded on its own. `at` is relative to the layer.
struct PlacedTile {
    Vector2 at{};
    int gid = 0;
    bool flip_x = false;
    bool flip_y = false;
    float alpha = 1.0f;
};

struct Layer {
    std::string name;
    bool solid_layer = false; // every non-empty cell is solid
    bool visible = true; // false: a collision layer only, never drawn
    int width = 0; // in cells
    int height = 0;
    int cell_width = 0; // 0 = the map's tile size
    int cell_height = 0;
    Vector2 offset{}; // the layer's own shift inside the map, in pixels
    float opacity = 1.0f;
    // One per cell. Tiled's whole layer; for LDtk the TOP tile of each cell,
    // which is what tile_at() and solid tiles look at.
    std::vector<int> gids;
    // LDtk: every tile, bottom to top. Drawn instead of `gids` when not empty.
    std::vector<PlacedTile> placed;
    // LDtk's IntGrid, one per cell: painted with the value called `solid`.
    std::vector<bool> solid_cells;
};

// A custom property of an object, whichever editor wrote it. Kept here as
// owned values, so nothing after parsing points into a parser's own tree.
struct Property {
    enum class Kind { BOOL, INT, FLOAT, STRING, POINT };
    std::string key;
    Kind kind = Kind::STRING;
    bool boolean = false;
    int integer = 0;
    float floating = 0;
    std::string text;
    std::vector<Vector2> points; // POINT: one, or a list (LDtk's Array<Point>)
};

// What a MapObject's names view, and what its property lookups read. Owned
// through a unique_ptr so the strings never move.
struct ObjectInfo {
    std::string name;
    std::string type;
    std::string iid;
    std::vector<Property> properties;
    // Whether an area means a wall when no factory claims the object. True
    // for Tiled, where a rectangle drawn on an object layer is a shape; false
    // for an LDtk entity, whose box is only its size in the editor.
    bool solid_area = true;
};

// A level of the project the map came from, where it sits in the world.
struct LevelInfo {
    std::string name;
    Rectangle world{};
};

} // namespace rmp::tilemap::detail

// The parsed map. Forward-declared in the public header so that the readers
// and <vector> stay out of it; owned by the Tilemap through a unique_ptr.
struct rmp::tilemap::detail::MapData {
    int width = 0; // in tiles
    int height = 0;
    int tile_width = 0;
    int tile_height = 0;
    Vector2 size_px{}; // {0,0} = width * tile_width by height * tile_height
    Vector2 origin{}; // where the map's (0,0) sits in the world; LDtk's worldX/Y
    std::vector<Layer> layers; // bottom to top
    std::vector<Tileset> tilesets;
    std::vector<MapObject> objects;
    std::vector<std::unique_ptr<ObjectInfo>> infos;
    std::vector<std::pair<std::string, Callback<Scene &, const MapObject &>>> factories;
    std::string source; // the file it came from
    std::string level; // the LDtk level loaded; "" for Tiled
    std::vector<LevelInfo> levels; // the levels of its world, for neighbour_at()

    // Hands back a MapObject whose strings and properties live in `info`.
    MapObject &add_object(std::unique_ptr<ObjectInfo> info);
};

namespace rmp::tilemap::detail {

// What a Tilemap keeps private, reached by the loader that fills it, by the
// engine that collides with it and by the tests that read it back.
struct Access {
    // Owned by the map from here, and whatever was there goes; null empties it.
    static void adopt(Tilemap &map, MapPtr data) { map.adopt(std::move(data)); }
    // The parsed map, or null for an empty one.
    static const MapData *data(const Tilemap &map) { return map.data(); }
};

// The bytes of a map file into a MapData: an LDtk project when `name` ends in
// .ldtk, a Tiled JSON map otherwise. `level` picks the LDtk level by name, and
// empty means the first; Tiled has no levels and ignores it. Returns nullptr
// and says why. TAKES no ownership of `bytes`; the caller owns the result and
// frees it with free_map.
MapPtr parse_map(const void *bytes, int size, const char *name, const char *level = "");

// The Tiled half and the LDtk half of parse_map, for the tests that want one.
MapPtr parse_tiled(const void *bytes, int size, const char *name);
MapPtr parse_ldtk(const void *bytes, int size, const char *name, const char *level);

// The object layers, flattened, for a test that wants to look before spawning.
int object_count(const MapData *data);
const MapObject *object_at(const MapData *data, int index);

// The smallest cell of any layer, 0 for a map with none: how far a moving
// object may step before the collision pass looks at the map again.
float smallest_cell(const MapData *data);

// Where one gid is in its tileset image, with the tileset's margin and spacing
// already in it. {0,0,0,0} when no tileset in the map holds that gid. This is
// the arithmetic draw() does, split out because it is the half a headless test
// can check -- and margin and spacing are exactly the kind of thing that looks
// right until somebody opens the game.
Rectangle tile_source(const MapData *data, int gid);

// Where that tile's top-left corner goes in world units, which is NOT simply
// the cell's own corner: Tiled anchors a tile layer by the BOTTOM-left, so a
// tileset whose tiles are taller than the map's grid reaches UPWARDS out of
// the cell. The cell's corner when no tileset in the map holds that gid --
// nothing to be taller than the grid.
Vector2 tile_origin(const MapData *data, int gid, int column, int row);

// The file name of a path: the asset layer works in names, and both editors
// write paths relative to the map.
const char *file_name_of(const char *path);

// A tileset image that did not become a texture, said as what it is: not in
// resources/, or there and not loadable. `map` names an LDtk project in the
// line; "" for a Tiled map. Both readers call it, so they say the same thing.
void report_tileset_image(const char *image, const char *map);

} // namespace rmp::tilemap::detail
