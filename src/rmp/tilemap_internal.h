#pragma once

#include <rmp/tilemap.h> // MapData, MapPtr

#include <memory>
// ---------------------------------------------------------------------------
// Private to src/rmp/. The half of the tilemap layer that needs no GPU.
//
// tests/tilemap_test.cpp includes this directly: parsing a Tiled map is
// arithmetic, so everything the tests care about -- dimensions, GIDs, solid
// tiles, object positions and properties, the factories -- runs without a
// window. Only the tileset texture and draw() need one.
// ---------------------------------------------------------------------------

#include <raylib.h> // Rectangle

namespace rmp {
struct MapObject; // namespace tilemap::detail
} // namespace rmp

namespace rmp::tilemap::detail {

// The bytes of a Tiled JSON map into a MapData. Returns nullptr and says why
// -- and says it usefully for the one failure people actually hit, which is
// layers saved as base64 rather than CSV. TAKES no ownership of `bytes`; the
// caller owns the result and frees it with free_map.
MapPtr parse_map(const void *bytes, int size, const char *name);

// The object layers, flattened, for a test that wants to look before spawning.
int object_count(const MapData *data);
const MapObject *object_at(const MapData *data, int index);

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

} // namespace rmp::tilemap::detail
