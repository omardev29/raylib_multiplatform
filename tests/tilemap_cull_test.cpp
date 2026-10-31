// ===========================================================================
// What a map does once it is read, to stay cheap at any size: the table of
// solid cells index_map() folds every source into, and drawing only what the
// camera shows (Tilemap::draw(area)) -- LDtk's placed tiles through their
// chunks, a Tiled layer through its range of cells. No GPU: the textures are
// not there, so what is held is which tiles a draw LOOKS AT
// (tiles_considered()); tests/ui_render_test.cpp holds the pixels.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/tilemap_internal.h"

#include <rmp/tilemap.h>

#include <raylib.h>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

using rmp::tilemap::detail::Layer;
using rmp::tilemap::detail::MapData;

std::string read(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

struct Parsed {
    rmp::Tilemap map;
    const MapData *data = nullptr;
    explicit Parsed(const std::string &file, std::string_view level = {}) {
        const std::string text = read(std::string(RMP_TEST_FIXTURES) + file);
        REQUIRE_FALSE(text.empty());
        rmp::tilemap::detail::Access::adopt(
            map, rmp::tilemap::detail::parse_map(text.data(), static_cast<int>(text.size()),
                                                 file.c_str(), level));
        data = rmp::tilemap::detail::Access::data(map);
        REQUIRE(data != nullptr);
    }
};

int placed_total(const MapData &data) {
    int n = 0;
    for (const Layer &layer : data.layers) {
        if (layer.visible && layer.opacity > 0) n += static_cast<int>(layer.placed.size());
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
    TEST_CASE("an LDtk level draws every tile with no area, and one screen of it with one") {
        const Parsed p("ldtk/Typical_2D_platformer_example.ldtk");
        const int total = placed_total(*p.data);
        REQUIRE(total > 500);
        CHECK(drawn_all(p.map) == total);
        // The whole level in view: every tile, none twice.
        const Rectangle b = p.map.bounds();
        CHECK(drawn(p.map, b) == total);
        CHECK(drawn(p.map, Rectangle{ b.x - 1000, b.y - 1000, b.width + 2000,
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
        for (float x = b.x - 64; x < b.x + b.width; x += 97) {
            for (float y = b.y - 64; y < b.y + b.height; y += 71) {
                const Rectangle area{ x, y, 120, 80 };
                int expected = 0;
                for (const Layer &layer : p.data->layers) {
                    if (!layer.visible || layer.opacity <= 0) continue;
                    const Vector2 base{ p.data->origin.x + layer.offset.x,
                                        p.data->origin.y + layer.offset.y };
                    const auto cell = static_cast<float>(
                        layer.cell_width > 0 ? layer.cell_width : p.data->tile_width);
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
        CHECK(drawn(p.map, Rectangle{ b.x + b.width + 5000, b.y + b.height + 5000, 10, 10 }) <
              placed_total(*p.data) / 4);
        // A rectangle that is not one draws nothing.
        CHECK(drawn(p.map, Rectangle{ NAN, 0, 10, 10 }) == 0);
        CHECK(drawn(p.map, Rectangle{ 0, 0, -5, 10 }) == 0);
    }

    TEST_CASE("a Tiled layer draws its cells in view, and a tile taller than its cell from "
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
               "ldtk/Typical_2D_platformer_example.ldtk", "ldtk/Typical_TopDown_example.ldtk",
               "map_minimal.json" }) {
            CAPTURE(file);
            const Parsed p(file);
            int solid = 0;
            for (const Layer &layer : p.data->layers) {
                REQUIRE(layer.solid.size() >= layer.solid_cells.size());
                for (std::size_t at = 0; at < layer.solid.size(); ++at) {
                    const bool intgrid = at < layer.solid_cells.size() && layer.solid_cells[at];
                    const int gid = at < layer.gids.size() ? layer.gids[at] : 0;
                    bool tile = false;
                    for (const auto &set : p.data->tilesets) {
                        if (gid >= set.first_gid && gid <= set.last_gid) {
                            const auto local = static_cast<std::size_t>(gid - set.first_gid);
                            tile = local < set.solid.size() && set.solid[local];
                            break;
                        }
                    }
                    const bool expected = intgrid || (gid != 0 && (layer.solid_layer || tile));
                    CHECK((layer.solid[at] != 0) == expected);
                    solid += layer.solid[at];
                }
            }
            MESSAGE(file << ": " << solid << " solid cells");
            if (file.find("07_platformer") != std::string::npos) CHECK(solid > 100);
        }
    }
}
