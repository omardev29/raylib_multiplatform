#pragma once
// ---------------------------------------------------------------------------
// Maps for the properties of rmp::Tilemap and its two readers
// (tests/property/tilemap_prop.cpp, tests/property/ldtk_prop.cpp):
//
//   Grid      a map as plain values -- layers, each its own grid, cell size,
//             offset and solid cells, at an origin in the world -- that a
//             property can both build (to_map()) and reason about by hand
//             (solid_cell(), the model the queries are checked against). It
//             copies and prints, which a MapData does not.
//   nice_grid()     cells, offsets, origins and sizes on quarter pixels: every
//                   division a query does is exact, so the model and the map
//                   must agree to the last cell
//   hostile_grid()  the same, at origins and offsets near float's end, as the
//                   readers can hand over (they clamp to FLT_MAX)
//   Box       a Rectangle that prints, for the queries
//   number_text()   a JSON number as a file may write it: ordinary, fractional,
//                   past int's range, past float's, 1e400 (cJSON's infinity)
//
// Test code only: RapidCheck throws (RC_ASSERT), which no file under src/rmp/
// may do.
// ---------------------------------------------------------------------------

#include "gen.h"

#include "../../src/rmp/tilemap_internal.h"

#include <rmp/tilemap.h>

#include <rapidcheck.h>
#include <raylib.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace rmp::test::maps {

// A Rectangle a counterexample can show.
struct Box {
    Rectangle r{};
};

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
inline void showValue(const Box &box, std::ostream &os) {
    os << "{" << box.r.x << ", " << box.r.y << ", " << box.r.width << ", " << box.r.height
       << "}";
}

// A Vector2 a counterexample can show.
struct Point {
    Vector2 p{};
};

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
inline void showValue(const Point &point, std::ostream &os) {
    os << "{" << point.p.x << ", " << point.p.y << "}";
}

// gen.h's hostile_rect(), drawn as a Box so that a counterexample shows it.
inline rc::Gen<Box> hostile_box() {
    return rc::gen::map(rmp::test::gen::hostile_rect(),
                        [](Rectangle r) { return Box{ r }; });
}

struct GridLayer {
    int cell_w = 16;
    int cell_h = 16;
    float offset_x = 0;
    float offset_y = 0;
    int width = 0;
    int height = 0;
    std::vector<bool> solid; // width * height, row by row
    std::vector<int> gids; // width * height, row by row: tile_at()'s answers
};

struct Grid {
    float origin_x = 0;
    float origin_y = 0;
    std::vector<GridLayer> layers;
};

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
inline void showValue(const Grid &grid, std::ostream &os) {
    os << "Grid{origin " << grid.origin_x << "," << grid.origin_y;
    for (const GridLayer &layer : grid.layers) {
        os << " | " << layer.width << "x" << layer.height << " of " << layer.cell_w << "x"
           << layer.cell_h << " at " << layer.offset_x << "," << layer.offset_y << ":";
        for (std::size_t i = 0; i < layer.solid.size(); ++i) {
            if (layer.width > 0 && i % static_cast<std::size_t>(layer.width) == 0)
                os << "/";
            os << (layer.solid[i] ? '#' : '.');
        }
    }
    os << "}";
}

// Whether cell (column, row) of a layer is solid, by the grid's own word.
inline bool solid_cell(const GridLayer &layer, std::int64_t column, std::int64_t row) {
    if (column < 0 || row < 0 || column >= layer.width || row >= layer.height)
        return false;
    return layer.solid[static_cast<std::size_t>((row * layer.width) + column)];
}

// The map the grid describes, as a reader would hand it over: an IntGrid layer
// per GridLayer, its solid cells painted -- each layer at `opacity`, and, when
// `alpha` is given, one tile placed at its corner with that alpha.
inline rmp::Tilemap to_map(const Grid &grid, float opacity = 1.0f,
                           std::optional<float> alpha = std::nullopt) {
    auto data = std::make_unique<rmp::tilemap::detail::MapData>();
    data->origin = Vector2{ grid.origin_x, grid.origin_y };
    data->tile_width = 16;
    data->tile_height = 16;
    data->width = 4;
    data->height = 4;
    data->level = "grid";
    for (const GridLayer &spec : grid.layers) {
        rmp::tilemap::detail::Layer layer;
        layer.width = spec.width;
        layer.height = spec.height;
        layer.cell_width = spec.cell_w;
        layer.cell_height = spec.cell_h;
        layer.offset = Vector2{ spec.offset_x, spec.offset_y };
        layer.gids = spec.gids;
        layer.solid_cells = spec.solid;
        layer.opacity = opacity;
        if (alpha) {
            layer.placed.push_back(
                rmp::tilemap::detail::PlacedTile{ .at = Vector2{ 0, 0 },
                                                  .gid = 1,
                                                  .flip_x = true,
                                                  .flip_y = false,
                                                  .alpha = *alpha });
        }
        data->layers.push_back(std::move(layer));
    }
    rmp::Tilemap map;
    rmp::tilemap::detail::Access::adopt(
        map,
        rmp::tilemap::detail::MapPtr(data.release(), &rmp::tilemap::detail::free_map));
    return map;
}

namespace detail {

// A quarter pixel, between -limit and limit: exact in a float, and in every
// sum and division by a cell up to 64 a query makes of it.
inline rc::Gen<float> quarter(int limit) {
    return rc::gen::map(rc::gen::inRange(-4 * limit, (4 * limit) + 1),
                        [](int q) { return static_cast<float>(q) / 4.0f; });
}

inline rc::Gen<GridLayer> layer(const rc::Gen<float> &offset) {
    return rc::gen::exec([offset] {
        GridLayer out;
        // A cell of any size up to 64, and often one that is not a power of
        // two: the divisions a query makes are not exact then.
        out.cell_w = *rc::gen::element(1, 3, 5, 7, 10, 12, 16, 24, 32, 48, 64);
        out.cell_h = *rc::gen::oneOf(rc::gen::just(out.cell_w),
                                     rc::gen::element(1, 3, 8, 16, 20, 33));
        out.offset_x = *offset;
        out.offset_y = *offset;
        out.width = *rc::gen::inRange(0, 9);
        out.height = *rc::gen::inRange(0, 9);
        const auto cells =
            static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height);
        out.solid =
            *rc::gen::container<std::vector<bool>>(cells, rc::gen::arbitrary<bool>());
        out.gids = *rc::gen::container<std::vector<int>>(cells, rc::gen::inRange(0, 10));
        return out;
    });
}

} // namespace detail

// Every number on a quarter pixel: what a model computed by hand must match.
inline rc::Gen<Grid> nice_grid() {
    return rc::gen::exec([] {
        Grid out;
        out.origin_x = *detail::quarter(512);
        out.origin_y = *detail::quarter(512);
        out.layers = *rc::gen::container<std::vector<GridLayer>>(
            static_cast<std::size_t>(*rc::gen::inRange(1, 4)),
            detail::layer(detail::quarter(64)));
        return out;
    });
}

// Origins and offsets anywhere a reader can put them: up to FLT_MAX, which a
// level at worldX 1e400 is after finite_float().
inline rc::Gen<Grid> hostile_grid() {
    const auto place = rc::gen::weightedOneOf<float>({
        { 3, detail::quarter(512) },
        { 1,
          rc::gen::element(std::numeric_limits<float>::max(),
                           -std::numeric_limits<float>::max(), 1e30f, -1e30f, 2.2e9f,
                           -2.2e9f, 16777217.0f, 0.1f, -0.0f) },
    });
    return rc::gen::exec([place] {
        Grid out;
        out.origin_x = *place;
        out.origin_y = *place;
        out.layers = *rc::gen::container<std::vector<GridLayer>>(
            static_cast<std::size_t>(*rc::gen::inRange(1, 4)), detail::layer(place));
        return out;
    });
}

// A rectangle on quarter pixels, near a grid at `x`,`y`: sizes 0 included.
inline rc::Gen<Box> nice_box(float x, float y) {
    return rc::gen::exec([x, y] {
        Box out;
        out.r.x = x + *detail::quarter(160);
        out.r.y = y + *detail::quarter(160);
        out.r.width = std::fabs(*detail::quarter(96));
        out.r.height = std::fabs(*detail::quarter(96));
        return out;
    });
}

// A rectangle on quarter pixels whose edges often lie exactly on a cell
// boundary of one of the grid's layers: a box resting on a floor, flush
// against a wall -- where "covers, not touches" is decided.
inline rc::Gen<Box> touching_box(const Grid &grid) {
    return rc::gen::exec([grid] {
        Box out = *nice_box(grid.origin_x, grid.origin_y);
        if (grid.layers.empty()) return out;
        const GridLayer &layer = grid.layers[static_cast<std::size_t>(
            *rc::gen::inRange(0, static_cast<int>(grid.layers.size())))];
        const float base_x = grid.origin_x + layer.offset_x;
        const float base_y = grid.origin_y + layer.offset_y;
        // Which edges meet a boundary: none, the far one, the near one, both.
        const int x_edges = *rc::gen::inRange(0, 4);
        const int y_edges = *rc::gen::inRange(0, 4);
        const auto cell_edge = [](float base, int cell) {
            return base +
                (static_cast<float>(*rc::gen::inRange(-1, 11)) *
                 static_cast<float>(cell));
        };
        if (x_edges == 1) out.r.x = cell_edge(base_x, layer.cell_w) - out.r.width;
        if (x_edges == 2) out.r.x = cell_edge(base_x, layer.cell_w);
        if (x_edges == 3) {
            out.r.x = cell_edge(base_x, layer.cell_w);
            out.r.width = static_cast<float>(*rc::gen::inRange(0, 4) * layer.cell_w);
        }
        if (y_edges == 1) out.r.y = cell_edge(base_y, layer.cell_h) - out.r.height;
        if (y_edges == 2) out.r.y = cell_edge(base_y, layer.cell_h);
        if (y_edges == 3) {
            out.r.y = cell_edge(base_y, layer.cell_h);
            out.r.height = static_cast<float>(*rc::gen::inRange(0, 4) * layer.cell_h);
        }
        return out;
    });
}

// A point near the grid, as often as not exactly on a corner of one of its
// layers' cells -- where a point and the rectangle with no area there must
// still name the same cell.
inline rc::Gen<Point> cell_point(const Grid &grid) {
    return rc::gen::exec([grid] {
        Point out{ Vector2{ grid.origin_x + *detail::quarter(160),
                            grid.origin_y + *detail::quarter(160) } };
        if (grid.layers.empty() || *rc::gen::arbitrary<bool>()) return out;
        const GridLayer &layer = grid.layers[static_cast<std::size_t>(
            *rc::gen::inRange(0, static_cast<int>(grid.layers.size())))];
        out.p.x = grid.origin_x + layer.offset_x +
            (static_cast<float>(*rc::gen::inRange(-1, 10)) *
             static_cast<float>(layer.cell_w));
        out.p.y = grid.origin_y + layer.offset_y +
            (static_cast<float>(*rc::gen::inRange(-1, 10)) *
             static_cast<float>(layer.cell_h));
        return out;
    });
}

// A JSON number, written as a file may write it.
inline rc::Gen<std::string> number_text() {
    return rc::gen::weightedOneOf<std::string>({
        { 2,
          rc::gen::map(rc::gen::inRange(-64, 65),
                       [](int n) { return std::to_string(n * 8); }) },
        { 1,
          rc::gen::element<std::string>("0", "-0", "0.5", "10.5", "-3.25", "1e-400",
                                        "2147483647", "2147483648", "-2147483649",
                                        "4294967296", "3e38", "-3e38", "3.4028235e38",
                                        "1e39", "-1e39", "1e300", "1e400", "-1e400") },
    });
}

} // namespace rmp::test::maps
