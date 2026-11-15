// ===========================================================================
// What a map does once it is read, to stay cheap at any size: the table of
// solid cells index_map() folds every source into, and drawing only what the
// camera shows (Tilemap::draw(area)) -- LDtk's placed tiles through their
// chunks, a Tiled layer through its range of cells. No GPU: the textures are
// not there, so what is held is which tiles a draw LOOKS AT
// (tiles_considered()); tests/ui_render_test.cpp holds the pixels.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/tilemap/tilemap_internal.h"

#include <rmp/tilemap.h>

#include <raylib.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using rmp::tilemap::detail::Layer;
using rmp::tilemap::detail::MapData;

std::string read(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

struct Parsed {
    rmp::Tilemap map;
    explicit Parsed(const std::string &file, std::string_view level = {}) {
        const std::string text = read(std::string(RMP_TEST_FIXTURES) + file);
        REQUIRE_FALSE(text.empty());
        rmp::tilemap::detail::Access::adopt(
            map,
            rmp::tilemap::detail::parse_map(text.data(), static_cast<int>(text.size()),
                                            file.c_str(), level));
        REQUIRE(rmp::tilemap::detail::Access::data(map) != nullptr);
    }
    // What the map read.
    [[nodiscard]] const MapData &data() const {
        return *rmp::tilemap::detail::Access::data(map);
    }
};

int placed_total(const MapData &data) {
    int n = 0;
    for (const Layer &layer : data.layers) {
        if (layer.visible && layer.opacity > 0)
            n += static_cast<int>(layer.placed.size());
    }
    return n;
}

int drawn(const rmp::Tilemap &map, Rectangle area) {
    map.draw(area);
    return rmp::tilemap::detail::tiles_considered();
}

int drawn_all(const rmp::Tilemap &map) {
    map.draw();
    return rmp::tilemap::detail::tiles_considered();
}

} // namespace

TEST_SUITE("tilemap: culling") {
    TEST_CASE(
        "an LDtk level draws every tile with no area, and one screen of it with one") {
        const Parsed p("ldtk/Typical_2D_platformer_example.ldtk");
        const int total = placed_total(p.data());
        REQUIRE(total > 500);
        CHECK(drawn_all(p.map) == total);
        // The whole level in view: every tile, none twice.
        const Rectangle b = p.map.bounds();
        CHECK(drawn(p.map, b) == total);
        CHECK(drawn(p.map,
                    Rectangle{ b.x - 1000, b.y - 1000, b.width + 2000,
                               b.height + 2000 }) == total);
        // A corner of it: a fraction, and never nothing.
        const int corner = drawn(p.map, Rectangle{ b.x, b.y, 160, 90 });
        CHECK(corner > 0);
        CHECK(corner * 4 < total);
    }

    TEST_CASE("every tile a view overlaps is among those it draws, at every view") {
        // Brute force against the chunks: whatever overlaps the area -- its
        // source's size from where it was put -- has to be looked at.
        const Parsed p("ldtk/Typical_TopDown_example.ldtk");
        const Rectangle b = p.map.bounds();
        const int columns = static_cast<int>((b.width + 64) / 97) + 1;
        const int rows = static_cast<int>((b.height + 64) / 71) + 1;
        for (int i = 0; i < columns; ++i) {
            for (int j = 0; j < rows; ++j) {
                const float x = b.x - 64 + (97 * static_cast<float>(i));
                const float y = b.y - 64 + (71 * static_cast<float>(j));
                const Rectangle area{ x, y, 120, 80 };
                int expected = 0;
                for (const Layer &layer : p.data().layers) {
                    if (!layer.visible || layer.opacity <= 0) continue;
                    const Vector2 base{ p.data().origin.x + layer.offset.x,
                                        p.data().origin.y + layer.offset.y };
                    const auto cell = static_cast<float>(
                        layer.cell_width > 0 ? layer.cell_width : p.data().tile_width);
                    for (const auto &tile : layer.placed) {
                        const Rectangle r{ base.x + tile.at.x, base.y + tile.at.y, cell,
                                           cell };
                        if (CheckCollisionRecs(r, area)) ++expected;
                    }
                }
                CAPTURE(x);
                CAPTURE(y);
                CHECK(drawn(p.map, area) >= expected);
            }
        }
    }

    TEST_CASE("a view far from the level draws next to nothing") {
        const Parsed p("ldtk/Typical_2D_platformer_example.ldtk");
        const Rectangle b = p.map.bounds();
        // Past its far corner the edge chunk is asked, and only that.
        CHECK(drawn(p.map,
                    Rectangle{ b.x + b.width + 5000, b.y + b.height + 5000, 10, 10 }) <
              placed_total(p.data()) / 4);
        // A rectangle that is not one draws nothing.
        CHECK(drawn(p.map, Rectangle{ NAN, 0, 10, 10 }) == 0);
        CHECK(drawn(p.map, Rectangle{ 0, 0, -5, 10 }) == 0);
    }

    TEST_CASE(
        "a Tiled layer draws its cells in view, and a tile taller than its cell from "
        "below") {
        const Parsed p("map_tall_tiles.json");
        const int all = drawn_all(p.map);
        REQUIRE(all > 0);
        const Rectangle b = p.map.bounds();
        CHECK(drawn(p.map, b) == all);
        // Only the strip above the map's first row of cells: the tall tiles
        // standing in that row reach up into it, so they are looked at.
        const int above = drawn(p.map, Rectangle{ b.x, b.y - 8, b.width, 8 });
        CHECK(above > 0);
        CHECK(above <= all);
        CHECK(drawn(p.map, Rectangle{ b.x - 10000, b.y, 10, 10 }) == 0);
    }

    TEST_CASE("the solid table is every source of solidity, folded") {
        // The platformer's own world paints its walls in an IntGrid; the
        // samples use their IntGrids for other things; Tiled marks tiles.
        for (const std::string file :
             { "../../examples/games/07_platformer/resources/world.ldtk",
               "ldtk/Typical_2D_platformer_example.ldtk",
               "ldtk/Typical_TopDown_example.ldtk", "map_minimal.json" }) {
            CAPTURE(file);
            const Parsed p(file);
            int solid = 0;
            for (const Layer &layer : p.data().layers) {
                REQUIRE(layer.solid.size() >= layer.solid_cells.size());
                for (std::size_t at = 0; at < layer.solid.size(); ++at) {
                    const bool intgrid =
                        at < layer.solid_cells.size() && layer.solid_cells[at];
                    const int gid = at < layer.gids.size() ? layer.gids[at] : 0;
                    bool tile = false;
                    for (const auto &set : p.data().tilesets) {
                        if (gid >= set.first_gid && gid <= set.last_gid) {
                            const auto local =
                                static_cast<std::size_t>(gid - set.first_gid);
                            tile = local < set.solid.size() && set.solid[local];
                            break;
                        }
                    }
                    const bool expected =
                        intgrid || (gid != 0 && (layer.solid_layer || tile));
                    CHECK((layer.solid[at] != 0) == expected);
                    solid += layer.solid[at];
                }
            }
            MESSAGE(file << ": " << solid << " solid cells");
            if (file.find("07_platformer") != std::string::npos) CHECK(solid > 100);
        }
    }
}

// ---------------------------------------------------------------------------
// Maps made by hand, so that what a view draws is a number known in advance:
// which chunks a placed tile lands in, the 16x16-cell chunks themselves, the
// cells of a Tiled layer a view reaches, and the camera's view as a rectangle.
// ---------------------------------------------------------------------------

namespace {

using rmp::tilemap::detail::PlacedTile;
using rmp::tilemap::detail::Tileset;

constexpr int CELL = 16;
constexpr float CHUNK = CELL * rmp::tilemap::detail::CHUNK_CELLS; // 256 px

struct Handmade {
    rmp::Tilemap map;
    explicit Handmade(std::unique_ptr<MapData> data) {
        rmp::tilemap::detail::Access::adopt(
            map,
            rmp::tilemap::detail::MapPtr(data.release(),
                                         &rmp::tilemap::detail::free_map));
    }
    [[nodiscard]] const MapData &data() const {
        return *rmp::tilemap::detail::Access::data(map);
    }
};

std::unique_ptr<MapData> map_of_cells(Vector2 origin = {}) {
    auto data = std::make_unique<MapData>();
    data->origin = origin;
    data->tile_width = CELL;
    data->tile_height = CELL;
    Tileset set; // no texture: a draw looks at each tile and puts nothing down
    set.first_gid = 1;
    set.last_gid = 4;
    set.columns = 2;
    set.tile_width = CELL;
    set.tile_height = CELL;
    data->tilesets.push_back(std::move(set));
    return data;
}

// An LDtk-style layer of ten by ten cells and tiles put down by hand:
//   0 (0,0)       chunk (0,0)
//   1 (250,0)     reaches 266: chunks (0,0) and (1,0)
//   2 (300,0)     chunk (1,0)
//   3 (0,300)     chunk (0,1)
//   4 (600,600)   chunk (2,2): the chunks reach past the layer's 160 px
//   5 (-40,-40)   before the layer's corner: chunk (0,0)
//   6 (100,100)   a gid of no tileset, a cell wide: chunk (0,0)
Layer placed_layer() {
    Layer layer;
    layer.width = 10;
    layer.height = 10;
    layer.gids.assign(100, 0);
    for (const Vector2 at :
         { Vector2{ 0, 0 }, Vector2{ 250, 0 }, Vector2{ 300, 0 }, Vector2{ 0, 300 },
           Vector2{ 600, 600 }, Vector2{ -40, -40 } }) {
        layer.placed.push_back(PlacedTile{ .at = at, .gid = 1 });
    }
    layer.placed.push_back(PlacedTile{ .at = { 100, 100 }, .gid = 99 });
    return layer;
}

int drawn_in(const rmp::Tilemap &map, float x, float y, float w = 1, float h = 1) {
    return drawn(map, Rectangle{ x, y, w, h });
}

bool near(Rectangle a, Rectangle b) {
    constexpr float CLOSE = 0.01f;
    return std::fabs(a.x - b.x) < CLOSE && std::fabs(a.y - b.y) < CLOSE &&
        std::fabs(a.width - b.width) < CLOSE && std::fabs(a.height - b.height) < CLOSE;
}

} // namespace

TEST_SUITE("tilemap: culling, by hand") {
    TEST_CASE(
        "placed tiles are put in every chunk they reach, and the chunks cover them") {
        auto data = map_of_cells();
        data->layers.push_back(placed_layer());
        const Handmade m(std::move(data));
        const Layer &layer = m.data().layers[0];
        CHECK(layer.chunk_width == CHUNK);
        CHECK(layer.chunk_height == CHUNK);
        // 616 px to the right and down: three chunks each way, past the layer.
        CHECK(layer.chunk_columns == 3);
        CHECK(layer.chunk_rows == 3);
        REQUIRE(layer.chunks.size() == 9);
        using List = std::vector<std::uint32_t>;
        CHECK(layer.chunks[0] == List{ 0, 1, 5, 6 });
        CHECK(layer.chunks[1] == List{ 1, 2 });
        CHECK(layer.chunks[2].empty());
        CHECK(layer.chunks[3] == List{ 3 });
        CHECK(layer.chunks[4].empty());
        CHECK(layer.chunks[8] == List{ 4 });
    }

    TEST_CASE("a view draws exactly the tiles of the chunks it reaches, each once") {
        auto data = map_of_cells();
        data->layers.push_back(placed_layer());
        const Handmade m(std::move(data));
        CHECK(drawn_all(m.map) == 7);
        CHECK(drawn_in(m.map, 0, 0, 10, 10) == 4); // chunk (0,0)
        CHECK(drawn_in(m.map, 260, 0, 10, 10) == 2); // chunk (1,0)
        CHECK(drawn_in(m.map, 200, 0, 100, 10) == 5); // both: tile 1 once
        CHECK(drawn_in(m.map, 600, 600) == 1); // chunk (2,2)
        CHECK(drawn_in(m.map, 0, 300) == 1); // chunk (0,1)
        CHECK(drawn_in(m.map, 520, 0) == 0); // chunk (2,0), empty
        CHECK(drawn_in(m.map, 5000, 5000) == 1); // past the edge: the edge chunk
        CHECK(drawn_in(m.map, -5000, -5000) == 4); // before it: the first one
        CHECK(drawn_in(m.map, 0, 0, 1000, 1000) == 7);
        // A view whose far edge is the next chunk's first pixel reaches it.
        CHECK(drawn_in(m.map, 0, 0, CHUNK, 1) == 5);
        CHECK(drawn_in(m.map, 0, 0, CHUNK - 1, 1) == 4);
    }

    TEST_CASE("the map's origin and the layer's offset move the chunks with the tiles") {
        auto data = map_of_cells({ 100, 50 });
        Layer layer = placed_layer();
        layer.offset = { 10, 20 }; // the layer's corner at (110, 70)
        data->layers.push_back(std::move(layer));
        const Handmade m(std::move(data));
        CHECK(drawn_in(m.map, 110, 70) == 4);
        CHECK(drawn_in(m.map, 110 + 300, 70) == 2); // chunk (1,0)
        CHECK(drawn_in(m.map, 110, 70 + 200) == 4); // still chunk (0,0)
        CHECK(drawn_in(m.map, 110, 70 + 300) == 1); // chunk (0,1)
        CHECK(drawn_in(m.map, 110 + 600, 70 + 600) == 1);
    }

    TEST_CASE("the chunks cover the whole layer, and a tile up to its far edge") {
        // A layer of 40 x 40 cells is 640 px: three chunks each way, whatever
        // its tiles cover.
        auto big = map_of_cells();
        Layer wide;
        wide.width = 40;
        wide.height = 40;
        wide.placed.push_back(PlacedTile{ .at = { 0, 0 }, .gid = 1 });
        big->layers.push_back(std::move(wide));
        const Handmade m(std::move(big));
        CHECK(m.data().layers[0].chunk_columns == 3);
        CHECK(m.data().layers[0].chunk_rows == 3);
        // A small layer and a tile at (500, 500): its far edge, 516, is in the
        // third chunk, where its near one is in the second.
        auto small = map_of_cells();
        Layer one;
        one.width = 2;
        one.height = 2;
        one.placed.push_back(PlacedTile{ .at = { 500, 500 }, .gid = 1 });
        small->layers.push_back(std::move(one));
        const Handmade n(std::move(small));
        CHECK(n.data().layers[0].chunk_columns == 3);
        CHECK(n.data().layers[0].chunk_rows == 3);
    }

    TEST_CASE("a placed tile is as big as its tileset's tiles, or a cell without one") {
        auto data = map_of_cells();
        Tileset large; // 32 px tiles over 16 px cells
        large.first_gid = 10;
        large.last_gid = 13;
        large.columns = 2;
        large.tile_width = 32;
        large.tile_height = 32;
        data->tilesets.push_back(std::move(large));
        Layer layer;
        layer.width = 10;
        layer.height = 10;
        // 230 + 32 reaches the second chunk; 230 + 16 does not.
        layer.placed.push_back(PlacedTile{ .at = { 230, 230 }, .gid = 10 });
        // No tileset: a cell, 16 px, so 230 + 16 stays in the first chunk.
        layer.placed.push_back(PlacedTile{ .at = { 230, 0 }, .gid = 99 });
        data->layers.push_back(std::move(layer));
        const Handmade m(std::move(data));
        const Layer &got = m.data().layers[0];
        REQUIRE(got.chunk_columns == 2);
        REQUIRE(got.chunk_rows == 2);
        using List = std::vector<std::uint32_t>;
        CHECK(got.chunks[0] == List{ 0, 1 });
        CHECK(got.chunks[1] == List{ 0 });
        CHECK(got.chunks[2] == List{ 0 });
        CHECK(got.chunks[3] == List{ 0 });
    }

    TEST_CASE("a hidden layer and a layer faded to nothing draw nothing") {
        for (const bool hidden : { true, false }) {
            CAPTURE(hidden);
            auto data = map_of_cells();
            Layer layer = placed_layer();
            if (hidden) {
                layer.visible = false;
            } else {
                layer.opacity = 0;
            }
            data->layers.push_back(std::move(layer));
            const Handmade m(std::move(data));
            CHECK(drawn_all(m.map) == 0);
            CHECK(drawn_in(m.map, 0, 0, 1000, 1000) == 0);
        }
    }

    TEST_CASE("tiles that claim too many chunks are drawn whole, unindexed") {
        // 256 x 256 chunks is the most a layer is cut into; one pixel more and
        // the layer is drawn whole, whatever the view.
        for (const float corner : { 65536.0f - CELL, 65536.0f - CELL + 1 }) {
            CAPTURE(corner);
            auto data = map_of_cells();
            Layer layer = placed_layer();
            layer.placed.push_back(PlacedTile{ .at = { corner, corner }, .gid = 1 });
            data->layers.push_back(std::move(layer));
            const Handmade m(std::move(data));
            const Layer &got = m.data().layers[0];
            const bool indexed = corner == 65536.0f - CELL;
            CHECK((got.chunk_columns == 256) == indexed);
            CHECK((got.chunk_rows == 256) == indexed);
            CHECK(got.chunks.empty() == !indexed);
            CHECK(drawn_in(m.map, 0, 0, 10, 10) == (indexed ? 4 : 8));
        }
    }

    TEST_CASE("a Tiled layer draws the cells around a view, as far as its biggest tile") {
        auto data = map_of_cells();
        Layer layer;
        layer.width = 8;
        layer.height = 6;
        layer.gids.assign(48, 1);
        layer.gids[0] = 0; // an empty cell is not looked at
        data->layers.push_back(std::move(layer));
        const Handmade m(std::move(data));
        CHECK(drawn_all(m.map) == 47);
        // One cell's view: a cell more each way, clamped at the corner.
        CHECK(drawn_in(m.map, 0, 0, CELL, CELL) == 8); // 3 x 3, less the empty one
        CHECK(drawn_in(m.map, 32, 32) == 9); // columns and rows 1..3
        CHECK(drawn_in(m.map, 33, 33) == 9);
        // The edges: a view a cell before the layer reaches its first column.
        CHECK(drawn_in(m.map, -16, 32) == 3); // column 0, rows 1..3
        CHECK(drawn_in(m.map, -33, 32) == 0);
        CHECK(drawn_in(m.map, -32, 32) == 0);
        CHECK(drawn_in(m.map, 143, 32) == 3); // column 7
        CHECK(drawn_in(m.map, 144, 32) == 0);
        CHECK(drawn_in(m.map, 32, 111) == 3); // row 5
        CHECK(drawn_in(m.map, 32, 112) == 0);
        CHECK(drawn_in(m.map, 32, -16) == 3); // row 0
        CHECK(drawn_in(m.map, 32, -32) == 0);
        CHECK(drawn_in(m.map, NAN, 0) == 0);
    }

    TEST_CASE(
        "a Tiled layer's cells follow the origin, and a tall tileset reaches further") {
        auto data = map_of_cells({ 100, 50 });
        data->tilesets[0].tile_height = 48; // a tree drawn up from its cell
        Layer layer;
        layer.width = 8;
        layer.height = 6;
        layer.offset = { 4, 6 }; // the grid's corner at (104, 56)
        layer.gids.assign(48, 1);
        data->layers.push_back(std::move(layer));
        const Handmade m(std::move(data));
        // Rows reach 48 px up and down from the view: rows 0..5 for one at row 2.
        CHECK(drawn_in(m.map, 104 + 32, 56 + 32) == 18); // 3 columns x 6 rows
        CHECK(drawn_in(m.map, 104 - 16, 56) == 4); // column 0, rows 0..3
        CHECK(drawn_in(m.map, 104 - 32, 56) == 0);
        CHECK(drawn_in(m.map, 104 + 143, 56 + 32) == 6); // column 7, rows 0..5
    }

    TEST_CASE("the camera's view is the screen's corners in the world") {
        using rmp::tilemap::detail::visible_area;
        const Rectangle screen{ 0, 0, 800, 450 };
        Camera2D camera{
            .offset = { 0, 0 }, .target = { 0, 0 }, .rotation = 0, .zoom = 1
        };
        CHECK(near(visible_area(camera, screen), screen));
        CHECK(near(visible_area(camera, Rectangle{ 10, 20, 100, 50 }),
                   Rectangle{ 10, 20, 100, 50 }));
        camera.zoom = 2;
        CHECK(near(visible_area(camera, screen), Rectangle{ 0, 0, 400, 225 }));
        camera = Camera2D{
            .offset = { 400, 225 }, .target = { 100, 50 }, .rotation = 0, .zoom = 1
        };
        CHECK(near(visible_area(camera, screen), Rectangle{ -300, -175, 800, 450 }));
        // A world wholly left of and above the origin: every corner negative.
        camera = Camera2D{
            .offset = { 0, 0 }, .target = { -2000, -1000 }, .rotation = 0, .zoom = 1
        };
        CHECK(near(visible_area(camera, screen), Rectangle{ -2000, -1000, 800, 450 }));
        camera = Camera2D{
            .offset = { 400, 225 }, .target = { 100, 50 }, .rotation = 0, .zoom = 1
        };
        // Turned a quarter: the view is as tall as the screen is wide, around
        // the target.
        camera.rotation = 90;
        CHECK(near(visible_area(camera, screen), Rectangle{ -125, -350, 450, 800 }));
    }
}
