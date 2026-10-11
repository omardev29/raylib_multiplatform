// ---------------------------------------------------------------------------
// The two map readers -- LDtk (src/rmp/ldtk.cpp) and Tiled (tilemap.cpp over
// cute_tiled) -- held to the campaign's rules for any project a file can
// hold, and rmp::assets::load_map() to reading a file the way the parser
// reads its bytes:
//
//   - neighbour_at() never names the level the map is, and otherwise is the
//     first other level of its world whose rectangle holds the point;
//   - the level a project is loaded at is the one asked for, byte for byte,
//     or there is no map;
//   - every number a parsed map hands out is finite, every size not
//     negative, whatever number the file wrote (past int's range, past
//     float's, cJSON's 1e400, cute_tiled's "nan");
//   - a tile's source is its place in its tileset's grid, never negative;
//   - load_map(name) and load_map(name, level) are parse_map() of the file.
//
// The queries over a map, whichever reader made it, are
// tests/property/tilemap_prop.cpp's.
//
// covers: rmp::Tilemap::neighbour_at(Vector2) const
// covers: rmp::assets::load_map(std::string_view)
// covers: rmp::assets::load_map(std::string_view, std::string_view)
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"
#include "map_gen.h"

#include "../../src/rmp/internal.h"
#include "../../src/rmp/tilemap_internal.h"

#include <rmp/assets.h>
#include <rmp/tilemap.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using rmp::test::maps::number_text;
using rmp::test::maps::Point;

// A string as JSON writes it: every byte that needs one escaped, NUL as
// \u0000 (which cJSON reads back as the end of the string), the rest as is
// -- bytes that are not UTF-8 included, which cJSON takes.
std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (byte < 0x20) {
            constexpr std::string_view HEX = "0123456789abcdef";
            out += "\\u00";
            out += HEX[byte >> 4U];
            out += HEX[byte & 0xFU];
        } else {
            out += c;
        }
    }
    return out + "\"";
}

// What cJSON makes of json_string(text): the text up to its first NUL.
std::string as_read(std::string_view text) {
    return std::string(text.substr(0, text.find('\0')));
}

// ---- worlds of levels -----------------------------------------------------

struct LevelSpec {
    std::string name;
    std::string x = "0";
    std::string y = "0";
    std::string width = "64";
    std::string height = "64";
};

struct WorldSpec {
    std::string layout = "Free";
    std::vector<LevelSpec> levels;
};

std::string to_json(const WorldSpec &world) {
    std::string out = R"({"jsonVersion":"1.5.3","worldLayout":")" + world.layout +
        R"(","defs":{"layers":[],"tilesets":[]},"levels":[)";
    for (std::size_t i = 0; i < world.levels.size(); ++i) {
        const LevelSpec &level = world.levels[i];
        if (i > 0) out += ",";
        out += R"({"identifier":)" + json_string(level.name) + R"(,"worldX":)" + level.x +
            R"(,"worldY":)" + level.y + R"(,"pxWid":)" + level.width + R"(,"pxHei":)" +
            level.height + R"(,"layerInstances":[]})";
    }
    return out + "]}";
}

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const WorldSpec &world, std::ostream &os) { os << to_json(world); }

// A world of up to six levels: names from a few (so two levels share one),
// or anything a string can hold; numbers as `numbers` writes them.
rc::Gen<WorldSpec> world_of(const rc::Gen<std::string> &numbers,
                            const rc::Gen<std::string> &sizes) {
    return rc::gen::exec([numbers, sizes] {
        WorldSpec out;
        out.layout =
            *rc::gen::element<std::string>("Free", "GridVania", "LinearHorizontal");
        const int count = *rc::gen::inRange(1, 7);
        for (int i = 0; i < count; ++i) {
            LevelSpec level;
            level.name = *rc::gen::weightedOneOf<std::string>(
                { { 4, rc::gen::element<std::string>("A", "B", "C", "Level_0") },
                  { 1, rmp::test::gen::hostile_string() } });
            level.x = *numbers;
            level.y = *numbers;
            level.width = *sizes;
            level.height = *sizes;
            out.levels.push_back(std::move(level));
        }
        return out;
    });
}

// Numbers on an 8 px grid: a level's place and size exact, no fraction for
// bounds() to cut.
rc::Gen<std::string> grid_number() {
    return rc::gen::map(rc::gen::inRange(-24, 25),
                        [](int n) { return std::to_string(n * 8); });
}
rc::Gen<std::string> grid_size() {
    return rc::gen::map(rc::gen::inRange(1, 13),
                        [](int n) { return std::to_string(n * 8); });
}

struct Parsed {
    rmp::Tilemap map;
    explicit Parsed(const std::string &text, std::string_view level = {}) {
        rmp::tilemap::detail::Access::adopt(
            map,
            rmp::tilemap::detail::parse_map(text.data(), static_cast<int>(text.size()),
                                            "world.ldtk", level));
    }
};

bool holds(Rectangle r, Vector2 p) {
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.width && p.y < r.y + r.height;
}

// Points worth asking about: each level's corners, middle and just inside its
// far corner, and points no level has.
std::vector<Vector2> points_of(const WorldSpec &world) {
    std::vector<Vector2> out;
    for (const LevelSpec &level : world.levels) {
        const auto x = static_cast<float>(std::strtod(level.x.c_str(), nullptr));
        const auto y = static_cast<float>(std::strtod(level.y.c_str(), nullptr));
        const auto w = static_cast<float>(std::strtod(level.width.c_str(), nullptr));
        const auto h = static_cast<float>(std::strtod(level.height.c_str(), nullptr));
        for (const float fx : { 0.0f, 0.5f, 0.99f, 1.0f }) {
            for (const float fy : { 0.0f, 0.5f, 0.99f, 1.0f }) {
                out.push_back(
                    Vector2{ rmp::tilemap::detail::finite_float(x + (w * fx)),
                             rmp::tilemap::detail::finite_float(y + (h * fy)) });
            }
        }
    }
    return out;
}

// ---- entities, fields and tiles ----------------------------------------

struct ThingSpec {
    std::string px_x, px_y, width, height, pivot_x, pivot_y;
    std::string world_x, world_y, offset_x, offset_y;
    std::string cx, cy, floating, integer, tile_x, tile_y, alpha, opacity;
    int grid = 16;
};

std::string to_json(const ThingSpec &t) {
    return R"({"jsonVersion":"1.5.3","worldLayout":"Free","defs":{"layers":[],)"
           R"("tilesets":[{"uid":1,"__cWid":4,"__cHei":4,"tileGridSize":16}]},)"
           R"("levels":[{"identifier":"L","worldX":)" +
        t.world_x + R"(,"worldY":)" + t.world_y +
        R"(,"pxWid":64,"pxHei":64,"layerInstances":[)"
        R"({"__type":"Entities","__gridSize":)" +
        std::to_string(t.grid) + R"(,"__pxTotalOffsetX":)" + t.offset_x +
        R"(,"__pxTotalOffsetY":)" + t.offset_y +
        R"(,"entityInstances":[{"__identifier":"Thing","iid":"t","px":[)" + t.px_x + "," +
        t.px_y + R"(],"width":)" + t.width + R"(,"height":)" + t.height +
        R"(,"__pivot":[)" + t.pivot_x + "," + t.pivot_y +
        R"(],"fieldInstances":[)"
        R"({"__identifier":"to","__type":"Point","__value":{"cx":)" +
        t.cx + R"(,"cy":)" + t.cy +
        R"(}},{"__identifier":"path","__type":"Array<Point>","__value":[{"cx":)" + t.cy +
        R"(,"cy":)" + t.cx + R"(}]},{"__identifier":"f","__type":"Float","__value":)" +
        t.floating + R"(},{"__identifier":"i","__type":"Int","__value":)" + t.integer +
        R"(}]}]},{"__type":"Tiles","__gridSize":)" + std::to_string(t.grid) +
        R"(,"__cWid":4,"__cHei":4,"__opacity":)" + t.opacity + R"(,"__pxTotalOffsetX":)" +
        t.offset_x +
        R"(,"__tilesetDefUid":1,"gridTiles":[)"
        R"({"t":5,"px":[)" +
        t.tile_x + "," + t.tile_y + R"(],"a":)" + t.alpha + "}]}]}]}";
}

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const ThingSpec &t, std::ostream &os) { os << to_json(t); }

rc::Gen<ThingSpec> thing() {
    return rc::gen::exec([] {
        ThingSpec t;
        t.px_x = *number_text();
        t.px_y = *number_text();
        t.width = *number_text();
        t.height = *number_text();
        t.pivot_x = *number_text();
        t.pivot_y = *number_text();
        t.world_x = *number_text();
        t.world_y = *number_text();
        t.offset_x = *number_text();
        t.offset_y = *number_text();
        t.cx = *number_text();
        t.cy = *number_text();
        t.floating = *number_text();
        t.integer = *number_text();
        t.tile_x = *number_text();
        t.tile_y = *number_text();
        t.alpha = *number_text();
        t.opacity = *number_text();
        t.grid = *rc::gen::element(1, 8, 16, 2147483647);
        return t;
    });
}

// ---- tilesets ------------------------------------------------------------

struct TilesetSpec {
    std::string columns, rows, size, padding, spacing;
    std::vector<int> tiles;
};

std::string to_json(const TilesetSpec &t) {
    std::string tiles;
    for (const int id : t.tiles) {
        if (!tiles.empty()) tiles += ",";
        tiles += R"({"px":[0,0],"t":)" + std::to_string(id) + "}";
    }
    return R"({"jsonVersion":"1.5.3","defs":{"layers":[],"tilesets":[{"uid":7,"__cWid":)" +
        t.columns + R"(,"__cHei":)" + t.rows + R"(,"tileGridSize":)" + t.size +
        R"(,"padding":)" + t.padding + R"(,"spacing":)" + t.spacing +
        R"(}]},"levels":[{"identifier":"L","pxWid":16,"pxHei":16,"layerInstances":[)"
        R"({"__type":"Tiles","__gridSize":16,"__cWid":1,"__cHei":1,"__tilesetDefUid":7,)"
        R"("gridTiles":[)" +
        tiles + "]}]}]}";
}

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const TilesetSpec &t, std::ostream &os) { os << to_json(t); }

rc::Gen<TilesetSpec> tileset_of(const rc::Gen<std::string> &numbers) {
    return rc::gen::exec([numbers] {
        TilesetSpec t;
        t.columns = *numbers;
        t.rows = *numbers;
        t.size = *numbers;
        t.padding = *numbers;
        t.spacing = *numbers;
        t.tiles = *rc::gen::container<std::vector<int>>(
            static_cast<std::size_t>(*rc::gen::inRange(1, 6)),
            rc::gen::oneOf(rc::gen::inRange(-1, 40), rc::gen::arbitrary<int>()));
        return t;
    });
}

rc::Gen<std::string> small_number() {
    return rc::gen::map(rc::gen::inRange(0, 9), [](int n) { return std::to_string(n); });
}

// ---- Tiled ---------------------------------------------------------------

struct TiledSpec {
    std::string x, y, width, height, rotation, floating;
    std::string map_width, tile_width, firstgid, tilecount, margin, spacing;
};

std::string to_json(const TiledSpec &t) {
    return R"({"width":)" + t.map_width + R"(,"height":2,"tilewidth":)" + t.tile_width +
        R"(,"tileheight":16,"layers":[{"type":"tilelayer","width":2,"height":1,"data":[1,2]},)"
        R"({"type":"objectgroup","objects":[{"x":)" +
        t.x + R"(,"y":)" + t.y + R"(,"width":)" + t.width + R"(,"height":)" + t.height +
        R"(,"rotation":)" + t.rotation +
        R"(,"properties":[{"name":"f","type":"float","value":)" + t.floating +
        R"(}]}]}],"tilesets":[{"firstgid":)" + t.firstgid +
        R"(,"columns":2,"tilecount":)" + t.tilecount +
        R"(,"tilewidth":16,"tileheight":16,)"
        R"("margin":)" +
        t.margin + R"(,"spacing":)" + t.spacing + "}]}";
}

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const TiledSpec &t, std::ostream &os) { os << to_json(t); }

rc::Gen<TiledSpec> tiled_map() {
    // cute_tiled reads a number with strtod, which also takes these.
    const auto any_number = rc::gen::weightedOneOf<std::string>(
        { { 4, number_text() },
          { 1,
            rc::gen::element<std::string>("nan", "-nan", "inf", "-inf", "infinity",
                                          "0x1p130") } });
    // An int field takes what strtoll reads, or the map is refused whole --
    // and a refused map would make this property hold by looking at nothing.
    const auto any_int = rc::gen::element<std::string>(
        "0", "1", "-1", "16", "2", "-16", "2147483647", "-2147483648", "4294967297",
        "9223372036854775807", "100000");
    return rc::gen::exec([any_number, any_int] {
        TiledSpec t;
        t.x = *any_number;
        t.y = *any_number;
        t.width = *any_number;
        t.height = *any_number;
        t.rotation = *any_number;
        t.floating = *any_number;
        t.map_width = *any_int;
        t.tile_width = *any_int;
        t.firstgid = *any_int;
        t.tilecount = *any_int;
        t.margin = *any_int;
        t.spacing = *any_int;
        return t;
    });
}

// Every number a map hands out, finite; every size not negative.
void assert_finite(const rmp::Tilemap &map) {
    const auto finite = [](Vector2 v) {
        return std::isfinite(v.x) && std::isfinite(v.y);
    };
    const Rectangle b = map.bounds();
    RC_ASSERT(std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.width) &&
              std::isfinite(b.height));
    RC_ASSERT(b.width >= 0 && b.height >= 0);
    RC_ASSERT(finite(map.tile_size()));
    if (!map.valid()) return;
    const rmp::tilemap::detail::MapData &data = *rmp::tilemap::detail::Access::data(map);
    for (const rmp::MapObject &object : data.objects) {
        RC_ASSERT(finite(object.position));
        RC_ASSERT(finite(object.size));
        RC_ASSERT(object.size.x >= 0 && object.size.y >= 0);
        RC_ASSERT(std::isfinite(object.rotation));
        for (const std::string_view key : { "f", "i", "to", "path" }) {
            RC_ASSERT(std::isfinite(object.property_float(key)));
            static_cast<void>(object.property_int(key)); // UBSan watches the cast
            for (int i = 0; i < object.property_count(key); ++i)
                RC_ASSERT(finite(object.property_point(key, i)));
        }
    }
    for (const rmp::tilemap::detail::Layer &layer : data.layers) {
        RC_ASSERT(finite(layer.offset));
        RC_ASSERT(std::isfinite(layer.opacity));
        for (const rmp::tilemap::detail::PlacedTile &tile : layer.placed) {
            RC_ASSERT(finite(tile.at));
            RC_ASSERT(std::isfinite(tile.alpha));
        }
        for (const int gid : layer.gids) {
            if (gid == 0) continue;
            const Rectangle source = rmp::tilemap::detail::tile_source(&data, gid);
            RC_ASSERT(std::isfinite(source.x) && std::isfinite(source.y));
            RC_ASSERT(source.x >= 0 && source.y >= 0 && source.width >= 0 &&
                      source.height >= 0);
        }
    }
}

// How many resource folders this process has made: each case's is its own,
// and the clock's count in the name keeps two processes' apart.
struct {
    int made = 0;
} folders;

// A folder of its own for load_map(), as the resources root for one case.
struct Resources {
    std::filesystem::path folder;
    std::string previous;
    Resources() : previous(rmp::assets::detail::resources_root()) {
        std::error_code error;
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        folder = std::filesystem::temp_directory_path(error) /
            ("rmp-ldtk-prop-" + std::to_string(now) + "-" +
             std::to_string(++folders.made));
        std::filesystem::create_directories(folder, error);
        const std::string root = (folder / "").string();
        rmp::assets::detail::set_resources_root(root.c_str());
    }
    ~Resources() {
        rmp::assets::detail::set_resources_root(previous.c_str());
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }
    Resources(const Resources &) = delete;
    Resources &operator=(const Resources &) = delete;
    void write(const std::string &name, const std::string &text) const {
        std::ofstream(folder / name, std::ios::binary) << text;
    }
};

bool same_map(const rmp::Tilemap &a, const rmp::Tilemap &b) {
    const Rectangle ra = a.bounds();
    const Rectangle rb = b.bounds();
    return a.valid() == b.valid() && a.level() == b.level() && ra.x == rb.x &&
        ra.y == rb.y && ra.width == rb.width && ra.height == rb.height &&
        a.layer_count() == b.layer_count() && a.object_count() == b.object_count();
}

} // namespace

TEST_SUITE("property: ldtk") {
    TEST_CASE("neighbour_at never names the level the map is") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "neighbour_at never names its own level, whatever the world", [] {
                const WorldSpec world = *world_of(number_text(), number_text());
                const std::string text = to_json(world);
                const Point extra = { Vector2{ *rmp::test::gen::hostile_float(),
                                               *rmp::test::gen::hostile_float() } };
                std::vector<Vector2> points = points_of(world);
                points.push_back(extra.p);
                for (const LevelSpec &level : world.levels) {
                    const Parsed p(text, as_read(level.name));
                    if (!p.map.valid()) continue;
                    for (const Vector2 at : points) {
                        const std::string_view next = p.map.neighbour_at(at);
                        RC_ASSERT(next.empty() || next != p.map.level());
                    }
                }
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("neighbour_at is the first other level that holds the point") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "neighbour_at is \"\" inside the level, else the first other one there", [] {
                WorldSpec world = *world_of(grid_number(), grid_size());
                world.layout = *rc::gen::element<std::string>("Free", "GridVania");
                const std::string text = to_json(world);
                const LevelSpec &asked = world.levels[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(world.levels.size())))];
                const Parsed p(text, as_read(asked.name));
                RC_ASSERT(p.map.valid());
                const Rectangle here = p.map.bounds();
                const auto rect_of = [](const LevelSpec &l) {
                    return Rectangle{ std::stof(l.x), std::stof(l.y), std::stof(l.width),
                                      std::stof(l.height) };
                };
                for (const Vector2 at : points_of(world)) {
                    std::string want;
                    if (!holds(here, at)) {
                        for (const LevelSpec &level : world.levels) {
                            if (as_read(level.name) == p.map.level()) continue;
                            if (holds(rect_of(level), at)) {
                                want = as_read(level.name);
                                break;
                            }
                        }
                    }
                    RC_ASSERT(std::string(p.map.neighbour_at(at)) == want);
                }
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("the level a project is loaded at is the one asked for, or none") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "a project loads the level asked for byte for byte, or no map at all", [] {
                const WorldSpec world = *world_of(grid_number(), grid_size());
                const std::string text = to_json(world);
                std::vector<std::string> names;
                names.reserve(world.levels.size());
                for (const LevelSpec &level : world.levels)
                    names.push_back(as_read(level.name));
                // A name it has, one with something after a NUL, or anything.
                const std::string asked = *rc::gen::oneOf(
                    rc::gen::elementOf(names),
                    rc::gen::map(rc::gen::elementOf(names),
                                 [](const std::string &n) {
                                     return n + std::string(1, '\0') + "x";
                                 }),
                    rmp::test::gen::hostile_string());
                const Parsed p(text, asked);
                if (asked.empty()) {
                    RC_ASSERT(p.map.valid());
                    RC_ASSERT(p.map.level() == names.front());
                } else if (std::ranges::find(names, asked) == names.end()) {
                    RC_ASSERT(!p.map.valid());
                } else {
                    RC_ASSERT(p.map.valid());
                    RC_ASSERT(p.map.level() == asked);
                }
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("every number a project hands out is finite") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "an LDtk project's positions, sizes and fields are finite, whatever "
            "numbers it wrote",
            [] {
                const ThingSpec spec = *thing();
                const Parsed p(to_json(spec));
                RC_ASSERT(p.map.valid());
                RC_ASSERT(p.map.object_count() == 1);
                assert_finite(p.map);
            });
        rmp::test::check(
            "a Tiled map's positions, sizes and properties are finite, whatever "
            "numbers it wrote",
            [] {
                const TiledSpec spec = *tiled_map();
                const std::string text = to_json(spec);
                rmp::Tilemap map;
                rmp::tilemap::detail::Access::adopt(
                    map,
                    rmp::tilemap::detail::parse_map(
                        text.data(), static_cast<int>(text.size()), "t.json"));
                RC_ASSERT(map.valid());
                RC_ASSERT(map.object_count() == 1);
                assert_finite(map);
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("a tile's source is its place in its tileset's grid") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check("every placed tile's source is finite and never negative", [] {
            const TilesetSpec spec = *tileset_of(number_text());
            const Parsed p(to_json(spec));
            RC_ASSERT(p.map.valid());
            assert_finite(p.map);
            const rmp::tilemap::detail::MapData &data =
                *rmp::tilemap::detail::Access::data(p.map);
            for (const rmp::tilemap::detail::Layer &layer : data.layers) {
                for (const rmp::tilemap::detail::PlacedTile &tile : layer.placed) {
                    const Rectangle source =
                        rmp::tilemap::detail::tile_source(&data, tile.gid);
                    RC_ASSERT(std::isfinite(source.x) && std::isfinite(source.y));
                    RC_ASSERT(source.x >= 0 && source.y >= 0 && source.width >= 0);
                }
            }
        });
        rmp::test::check(
            "a tile's source is padding plus its column and row of size and "
            "spacing",
            [] {
                const TilesetSpec spec = *tileset_of(small_number());
                const Parsed p(to_json(spec));
                RC_ASSERT(p.map.valid());
                const int columns = std::stoi(spec.columns);
                const int count = columns * std::stoi(spec.rows);
                const int size = std::stoi(spec.size);
                const int step = size + std::stoi(spec.spacing);
                const int padding = std::stoi(spec.padding);
                const rmp::tilemap::detail::MapData &data =
                    *rmp::tilemap::detail::Access::data(p.map);
                RC_ASSERT(data.layers.size() == 1U);
                std::size_t placed = 0;
                for (const int id : spec.tiles) {
                    if (id < 0 || id >= count) continue;
                    RC_ASSERT(placed < data.layers[0].placed.size());
                    const int gid = data.layers[0].placed[placed++].gid;
                    const Rectangle source =
                        rmp::tilemap::detail::tile_source(&data, gid);
                    RC_ASSERT(source.x ==
                              static_cast<float>(padding + ((id % columns) * step)));
                    const int row = id / columns; // the tileset's row, whole
                    RC_ASSERT(source.y == static_cast<float>(padding + (row * step)));
                    RC_ASSERT(source.width == static_cast<float>(size));
                }
                RC_ASSERT(placed == data.layers[0].placed.size());
            });
        SetTraceLogLevel(LOG_INFO);
    }

    TEST_CASE("load_map reads a file as parse_map reads its bytes") {
        SetTraceLogLevel(LOG_NONE);
        rmp::test::check(
            "load_map of a file is parse_map of its bytes, at any level", [] {
                const WorldSpec world = *world_of(grid_number(), grid_size());
                const std::string text = to_json(world);
                const Resources resources;
                resources.write("world.ldtk", text);
                const std::string level = as_read(world.levels.back().name);
                RC_ASSERT(
                    same_map(rmp::assets::load_map("world.ldtk"), Parsed(text).map));
                RC_ASSERT(same_map(rmp::assets::load_map("world.ldtk", level),
                                   Parsed(text, level).map));
                RC_ASSERT(!rmp::assets::load_map("missing.ldtk").valid());
                RC_ASSERT(!rmp::assets::load_map("missing.ldtk", level).valid());
            });
        SetTraceLogLevel(LOG_INFO);
    }
}
