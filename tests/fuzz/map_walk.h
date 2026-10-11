#pragma once
// ---------------------------------------------------------------------------
// What rmp/tilemap.h promises about ANY map a reader accepted, asked of every
// map the two map fuzz targets (tests/fuzz/ldtk.cpp, tests/fuzz/tiled.cpp)
// parse. One walk for both readers, because nothing after parsing knows which
// editor made the map:
//
//   - every number it hands out is finite: bounds(), tile_size(), each
//     object's position, size and rotation, every Float and Point property,
//     smallest_cell(), and where each tile it holds is drawn from and to --
//     sizes never negative, and no int arithmetic on the way that overflows
//     (UBSan says so);
//   - property_int() of any property returns, whatever the number was;
//   - solid_at(p) is solid_in() of the rectangle with no area at p, the
//     promise the header makes, for points a game can hand it by mistake
//     (NaN, the infinities, past int's range) and for every object's centre;
//   - solid_in() takes any rectangle, NaN, infinite and negative included;
//   - neighbour_at() never names the level the map is, and says "" inside it;
//   - tile_at() takes any layer, column and row, and an empty map answers zero
//     to everything;
//   - a map moves: the one moved from is empty, the one moved to the same map;
//   - its layers hold no more cells, all together, than MAX_CELLS: draw() and
//     solid_in() walk a layer cell by cell, and a layer is two numbers in the
//     file.
//
// Bounded per input -- at most WALKED cells a layer, WALKED objects, and the
// queries over the whole map only on maps of at most SCANNED cells -- so a
// level of sixteen million cells, which the readers accept, does not turn one
// input into minutes of work: solid_in() of a rectangle the size of such a
// level asks every cell of it, ten times over, and libFuzzer's -timeout would
// call the walk's own work a hang (regressions/ldtk/sixteen_million_cells).
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../../src/rmp/tilemap_internal.h"

#include <rmp/tilemap.h>

#include <raylib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rmp::test::fuzz::maps {

using rmp::tilemap::detail::Access;
using rmp::tilemap::detail::Layer;
using rmp::tilemap::detail::MapData;

// How much of one map the walk asks about.
inline constexpr int WALKED = 512;
// The most cells, over all its layers, a map may have for the walk to ask
// solid_in() about rectangles as big as the map: under the sanitizers a cell
// costs about a microsecond, and the walk asks ten such rectangles.
inline constexpr std::int64_t SCANNED = 4096;

// Every cell of every layer, as the layers say.
inline std::int64_t cells_of(const MapData &data) {
    std::int64_t out = 0;
    for (const Layer &layer : data.layers)
        out += static_cast<std::int64_t>(layer.width) * layer.height;
    return out;
}

inline bool finite(float v) { return std::isfinite(v); }
inline bool finite(Vector2 v) { return finite(v.x) && finite(v.y); }
inline bool finite(Rectangle r) {
    return finite(r.x) && finite(r.y) && finite(r.width) && finite(r.height);
}

// What a game can hand the map by mistake, as one coordinate.
inline constexpr std::array HOSTILE{
    std::numeric_limits<float>::quiet_NaN(),
    std::numeric_limits<float>::infinity(),
    -std::numeric_limits<float>::infinity(),
    std::numeric_limits<float>::max(),
    -std::numeric_limits<float>::max(),
    std::numeric_limits<float>::denorm_min(),
    2.2e9f,
    -2.2e9f,
    0.0f,
    -0.0f,
};

// The empty map's answers: zero, empty or false to everything.
inline void walk_empty(const rmp::Tilemap &map) {
    require(!map.valid() && !static_cast<bool>(map), "an empty map says it is empty");
    const Rectangle b = map.bounds();
    require(b.x == 0 && b.y == 0 && b.width == 0 && b.height == 0,
            "an empty map's bounds() are zero");
    require(map.layer_count() == 0 && map.object_count() == 0,
            "an empty map has no layers and no objects");
    require(map.level().empty() && map.neighbour_at(Vector2{}).empty(),
            "an empty map is no level and has no neighbours");
    require(!map.solid_at(Vector2{}) && !map.solid_in(Rectangle{ 0, 0, 1, 1 }),
            "an empty map is solid nowhere");
    require(map.tile_at(0, 0, 0) == 0, "an empty map holds no tile");
}

// One point: solid_at and the rectangle with no area there agree, and the
// neighbour there is never this level.
inline void ask_at(const rmp::Tilemap &map, Vector2 p) {
    const bool at = map.solid_at(p);
    const bool in = map.solid_in(Rectangle{ p.x, p.y, 0, 0 });
    require(at == in, "solid_at(p) is solid_in() of the rectangle with no area at p");
    const std::string_view next = map.neighbour_at(p);
    require(next.empty() || next != map.level(),
            "neighbour_at() never names its own level");
    const Rectangle b = map.bounds();
    if (p.x >= b.x && p.y >= b.y && p.x < b.x + b.width && p.y < b.y + b.height) {
        require(next.empty(), "neighbour_at() is \"\" inside the level");
    }
}

inline void walk_tiles(const MapData &data, const rmp::Tilemap &map) {
    for (std::size_t index = 0; index < data.layers.size(); ++index) {
        const Layer &layer = data.layers[index];
        const int layer_index = static_cast<int>(index);
        int asked = 0;
        for (int row = 0; row < layer.height && asked < WALKED; ++row) {
            for (int column = 0; column < layer.width && asked < WALKED;
                 ++column, ++asked) {
                const int gid = map.tile_at(layer_index, column, row);
                if (gid == 0) continue;
                const Rectangle source = rmp::tilemap::detail::tile_source(&data, gid);
                require(finite(source) && source.x >= 0 && source.y >= 0 &&
                            source.width >= 0 && source.height >= 0,
                        "a tile's place in its tileset is finite and not negative");
                require(
                    finite(rmp::tilemap::detail::tile_origin(&data, gid, column, row)),
                    "where a tile is drawn is finite");
            }
        }
        const std::size_t placed = std::min<std::size_t>(layer.placed.size(), WALKED);
        for (std::size_t i = 0; i < placed; ++i) {
            const rmp::tilemap::detail::PlacedTile &tile = layer.placed[i];
            require(finite(tile.at) && std::isfinite(tile.alpha),
                    "a placed tile's place and alpha are finite");
            const Rectangle source = rmp::tilemap::detail::tile_source(&data, tile.gid);
            require(finite(source) && source.x >= 0 && source.y >= 0 &&
                        source.width >= 0 && source.height >= 0,
                    "a tile's place in its tileset is finite and not negative");
        }
        require(finite(layer.offset) && std::isfinite(layer.opacity),
                "a layer's offset and opacity are finite");
        // Off the layer, any way: nothing, and no UB on the way there.
        for (const int off :
             { -1, std::numeric_limits<int>::min(), std::numeric_limits<int>::max() }) {
            require(map.tile_at(layer_index, off, 0) == 0 &&
                        map.tile_at(layer_index, 0, off) == 0,
                    "tile_at() off the layer is 0");
        }
        require(map.tile_at(layer_index, layer.width, 0) == 0 &&
                    map.tile_at(layer_index, 0, layer.height) == 0,
                "tile_at() off the layer is 0");
    }
    for (const int layer :
         { -1, static_cast<int>(data.layers.size()), std::numeric_limits<int>::min(),
           std::numeric_limits<int>::max() }) {
        require(map.tile_at(layer, 0, 0) == 0,
                "tile_at() of a layer that is not there is 0");
    }
}

inline void walk_objects(const MapData &data, const rmp::Tilemap &map) {
    // Every key any object has, asked of every object: a key it has and keys
    // it does not, with every kind of fallback.
    std::vector<std::string> keys{ "", "no such key" };
    for (const auto &info : data.infos) {
        for (const auto &property : info->properties) {
            if (keys.size() < 64) keys.push_back(property.key);
        }
    }
    const int count = std::min(map.object_count(), WALKED);
    for (int i = 0; i < count; ++i) {
        const rmp::MapObject &object = data.objects[static_cast<std::size_t>(i)];
        require(rmp::tilemap::detail::object_at(&data, i) == &object,
                "object_at() of every index below object_count()");
        require(finite(object.position) && finite(object.size) &&
                    std::isfinite(object.rotation),
                "an object's position, size and rotation are finite");
        require(object.size.x >= 0 && object.size.y >= 0,
                "an object's size is not negative");
        require(object.gid >= 0, "an object's gid has the flip bits off");
        ask_at(map, object.position);
        for (const std::string &key : keys) {
            static_cast<void>(object.property_int(key, -7));
            static_cast<void>(object.property_bool(key, true));
            static_cast<void>(object.property_string(key, "fallback"));
            require(std::isfinite(object.property_float(key, 0.5f)),
                    "property_float() is finite");
            const int points = object.property_count(key);
            require(points >= 0, "property_count() is not negative");
            for (int at = -1; at <= std::min(points, 8); ++at) {
                require(finite(object.property_point(key, at, Vector2{ 1, 2 })),
                        "property_point() is finite");
            }
        }
    }
    require(rmp::tilemap::detail::object_at(&data, map.object_count()) == nullptr &&
                rmp::tilemap::detail::object_at(&data, -1) == nullptr,
            "object_at() past either end is null");
}

// The whole walk, on a map a reader accepted or refused.
inline void walk(rmp::Tilemap &map) {
    if (!map.valid()) {
        walk_empty(map);
        return;
    }
    const MapData &data = *Access::data(map);
    const Rectangle b = map.bounds();
    require(finite(b) && b.width >= 0 && b.height >= 0,
            "bounds() are finite and not negative");
    const Vector2 tile = map.tile_size();
    require(finite(tile) && tile.x >= 0 && tile.y >= 0,
            "tile_size() is finite and not negative");
    require(map.layer_count() == static_cast<int>(data.layers.size()),
            "layer_count() counts the layers");
    const float cell = rmp::tilemap::detail::smallest_cell(&data);
    require(std::isfinite(cell) && cell >= 0,
            "smallest_cell() is finite and not negative");
    const std::int64_t cells = cells_of(data);
    require(cells <= rmp::tilemap::detail::MAX_CELLS,
            "a map's layers hold no more cells than MAX_CELLS");

    walk_tiles(data, map);
    walk_objects(data, map);

    // Points a game can hand it by mistake, and the level's own corners.
    for (const float x : HOSTILE) {
        for (const float y : HOSTILE) ask_at(map, Vector2{ x, y });
    }
    ask_at(map, Vector2{ b.x, b.y });
    ask_at(map, Vector2{ b.x + b.width, b.y + b.height });
    ask_at(map, Vector2{ b.x + (b.width / 2), b.y + (b.height / 2) });
    for (const tilemap::detail::LevelInfo &level : data.levels) {
        ask_at(map, Vector2{ level.world.x, level.world.y });
        ask_at(map,
               Vector2{ level.world.x + (level.world.width / 2),
                        level.world.y + (level.world.height / 2) });
        // Just inside its far corner: where a level's own rectangle and the
        // bounds() of the map made from it can disagree.
        ask_at(map,
               Vector2{ level.world.x + (level.world.width * 0.99f),
                        level.world.y + (level.world.height * 0.99f) });
    }
    // Any rectangle at all: no UB, whatever it answers. As big as the map,
    // and bigger, only on a map small enough to ask that of; on a bigger one
    // the HOSTILE points above have asked the same arithmetic of one cell.
    if (cells <= SCANNED) {
        for (const float v : HOSTILE) {
            static_cast<void>(map.solid_in(Rectangle{ v, b.y, b.width, b.height }));
            static_cast<void>(map.solid_in(Rectangle{ b.x, v, b.width, b.height }));
            static_cast<void>(map.solid_in(Rectangle{ b.x, b.y, v, b.height }));
            static_cast<void>(map.solid_in(Rectangle{ b.x, b.y, b.width, v }));
        }
    }

    // It moves, and moves back: the same map, and the one moved from empty.
    const std::string level(map.level());
    const int objects = map.object_count();
    rmp::Tilemap moved(std::move(map));
    walk_empty(map); // NOLINT(bugprone-use-after-move): moved from is empty, by promise
    require(moved.valid() && moved.level() == level && moved.object_count() == objects,
            "a map moved is the same map");
    map = std::move(moved);
    require(map.valid() && map.level() == level, "a map moved back is the same map");
}

} // namespace rmp::test::fuzz::maps
