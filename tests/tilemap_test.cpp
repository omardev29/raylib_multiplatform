// ===========================================================================
// Tiled maps: parsing, solid tiles, object layers and factories.
//
// All of it without a GPU, because parsing is arithmetic. Three fixtures, and
// the middle one is the important one:
//
//   map_minimal.json  a small map with both solid conventions, an object layer
//                     and properties of all four types.
//   map_modern.json   saved by Tiled 1.11, full of keys cute_tiled's 1.5 schema
//                     has never heard of. THE UNPATCHED PARSER REJECTS IT. It
//                     is what makes the patch in thirdparty/cute_tiled a tested
//                     thing rather than an assertion.
//   map_base64.json   layers saved compressed. It has to fail with the message
//                     that names the setting, not with a parse error.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/internal.h"
#include "../src/rmp/object_internal.h"
#include "../src/rmp/tilemap_internal.h"

#include <rmp/assets.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/tilemap.h>

#include <array>
#include <clocale>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

struct Fixture {
    Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    ~Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
    }
};

std::vector<unsigned char> bytes_of(const char *file) {
    const std::string path = std::string(RMP_TEST_FIXTURES) + file;
    std::ifstream in(path, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "missing " << path);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

// An external tileset is fetched through rmp::assets, which looks in the
// resources root -- so for the length of one test the root IS tests/fixtures/.
// Put back afterwards, because it is process-wide and every other test in this
// binary shares it.
struct AtFixtures {
    std::string previous;
    AtFixtures() : previous(rmp::assets::detail::resources_root()) {
        rmp::assets::detail::set_resources_root(RMP_TEST_FIXTURES);
    }
    ~AtFixtures() { rmp::assets::detail::set_resources_root(previous.c_str()); }
    AtFixtures(const AtFixtures &) = delete;
    AtFixtures &operator=(const AtFixtures &) = delete;
};

// A parsed map that frees itself. load_map goes through rmp::assets and would
// want resources/; the parser underneath is what these tests are about.
//
// `data` is the same pointer the Tilemap owns, kept so that the detail entry
// points -- which take the parsed map and not the class -- can be called on it.
// It is an alias and not a second owner: the Tilemap frees it.
struct Parsed {
    rmp::Tilemap map;
    const rmp::tilemap::detail::MapData *data = nullptr;
    explicit Parsed(const char *file) : Parsed(bytes_of(file), file) {}
    Parsed(const std::vector<unsigned char> &raw, const char *name) {
        rmp::tilemap::detail::Access::adopt(
            map,
            rmp::tilemap::detail::parse_map(raw.data(), static_cast<int>(raw.size()),
                                            name));
        data = rmp::tilemap::detail::Access::data(map);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// A minimal map
// ---------------------------------------------------------------------------

TEST_CASE("the dimensions, the tile size and the layers come out of the file") {
    const Parsed loaded("map_minimal.json");
    REQUIRE(loaded.map.valid());

    CHECK(loaded.map.tile_size().x == doctest::Approx(16));
    CHECK(loaded.map.tile_size().y == doctest::Approx(16));
    // 4x4 tiles of 16 = 64x64 world units.
    CHECK(loaded.map.bounds().width == doctest::Approx(64));
    CHECK(loaded.map.bounds().height == doctest::Approx(64));
    // Two tile layers. The object layer is not one of them.
    CHECK(loaded.map.layer_count() == 2);
}

TEST_CASE("the first row's GIDs are exactly what the file says") {
    const Parsed loaded("map_minimal.json");
    REQUIRE(loaded.map.valid());
    CHECK(loaded.map.tile_at(0, 0, 0) == 1);
    CHECK(loaded.map.tile_at(0, 1, 0) == 2);
    CHECK(loaded.map.tile_at(0, 2, 0) == 3);
    CHECK(loaded.map.tile_at(0, 3, 0) == 4);
    // The second row of that layer is empty.
    CHECK(loaded.map.tile_at(0, 0, 1) == 0);

    SUBCASE("and asking outside the map is 0, not a crash") {
        CHECK(loaded.map.tile_at(0, -1, 0) == 0);
        CHECK(loaded.map.tile_at(0, 99, 0) == 0);
        CHECK(loaded.map.tile_at(0, 0, 99) == 0);
        CHECK(loaded.map.tile_at(99, 0, 0) == 0);
        CHECK(loaded.map.tile_at(-1, 0, 0) == 0);
    }
}

// ---------------------------------------------------------------------------
// Solid tiles, by both conventions
// ---------------------------------------------------------------------------

TEST_CASE("a layer whose class is `solid` makes all of its tiles solid") {
    const Parsed loaded("map_minimal.json");
    REQUIRE(loaded.map.valid());
    // Layer 1 is the one with "class":"solid"; it has tiles at (1,1) and (2,1).
    CHECK(loaded.map.solid_at({ 24, 24 })); // inside column 1, row 1
    CHECK(loaded.map.solid_at({ 40, 24 })); // column 2, row 1
    CHECK_FALSE(loaded.map.solid_at({ 8, 24 })); // column 0, row 1: empty there
    // Row 3 of that layer is gid 7, which the tileset does NOT mark: solid only
    // because of the layer's class. Rows 1 and 2 above hold gid 6, which is
    // marked, so they could not tell the two conventions apart.
    CHECK(loaded.map.solid_at({ 8, 56 }));
}

TEST_CASE("a tile with a boolean `solid` property is solid wherever it is") {
    const Parsed loaded("map_minimal.json");
    REQUIRE(loaded.map.valid());
    // Tile id 5 in the tileset is marked solid, which is gid 6. The ground
    // layer's last row is gid 5 -- NOT solid -- and the solid layer's last row
    // is gid 7. Row 1 of the solid layer holds gid 6.
    CHECK(loaded.map.solid_at({ 24, 24 }));
    // The ground layer's first row is gids 1..4, none of them marked.
    CHECK_FALSE(loaded.map.solid_at({ 8, 8 }));
}

TEST_CASE("solid_in checks every tile it crosses, not just the corners") {
    const Parsed loaded("map_minimal.json");
    REQUIRE(loaded.map.valid());

    SUBCASE("a rectangle straddling a solid tile with its corners in empty space") {
        // The bug everybody writes the first time: a rectangle wider than a
        // tile can have all four corners in the clear and a wall through it.
        // Columns 0 and 2 of row 1 are empty; column 1 is solid.
        const Rectangle across{ 4, 20, 40, 8 };
        CHECK(loaded.map.solid_in(across));
    }
    SUBCASE("and one entirely in empty space is not solid") {
        CHECK_FALSE(loaded.map.solid_in({ 2, 18, 8, 8 }));
    }
    SUBCASE("outside the map is not solid either") {
        CHECK_FALSE(loaded.map.solid_at({ -10, -10 }));
        CHECK_FALSE(loaded.map.solid_at({ 9999, 9999 }));
    }
}

// ---------------------------------------------------------------------------
// Object layers
// ---------------------------------------------------------------------------

TEST_CASE("the object layer's contents, with the centre and the four property types") {
    const std::vector<unsigned char> raw = bytes_of("map_minimal.json");
    auto data = rmp::tilemap::detail::parse_map(raw.data(), static_cast<int>(raw.size()),
                                                "map_minimal.json");
    REQUIRE(data != nullptr);
    REQUIRE(rmp::tilemap::detail::object_count(data.get()) == 2);

    const rmp::MapObject *goblin = rmp::tilemap::detail::object_at(data.get(), 0);
    REQUIRE(goblin != nullptr);
    CHECK(std::string(goblin->name) == "goblin");
    CHECK(std::string(goblin->type) == "enemy");
    // Tiled says x=32 y=48 w=16 h=24, so the centre is 40, 60.
    CHECK(goblin->position.x == doctest::Approx(40));
    CHECK(goblin->position.y == doctest::Approx(60));
    CHECK(goblin->size.x == doctest::Approx(16));

    CHECK(goblin->property_int("hp") == 20);
    CHECK(goblin->property_float("speed") == doctest::Approx(1.5));
    CHECK(goblin->property_bool("boss"));
    CHECK(std::string(goblin->property_string("drop")) == "coin");

    SUBCASE("and a property that is not there is the default, whatever the type") {
        CHECK(goblin->property_int("missing", 7) == 7);
        CHECK(goblin->property_float("missing", 1.25f) == doctest::Approx(1.25));
        CHECK(goblin->property_bool("missing", true));
        CHECK(std::string(goblin->property_string("missing", "none")) == "none");
    }
    SUBCASE("an object with no properties at all is all defaults") {
        const rmp::MapObject *start = rmp::tilemap::detail::object_at(data.get(), 1);
        REQUIRE(start != nullptr);
        CHECK(std::string(start->type) == "checkpoint");
        CHECK(start->property_int("hp", 99) == 99);
    }
}

TEST_CASE_FIXTURE(Fixture, "a registered class produces the user's type") {
    World world;
    Parsed loaded("map_minimal.json");
    REQUIRE(loaded.map.valid());

    int made = 0;
    Vector2 where{};
    loaded.map.on_object("enemy", [&](rmp::Scene &scene, const rmp::MapObject &object) {
        ++made;
        where = object.position;
        scene.spawn({ .position = object.position, .size = { 8, 8 } });
    });
    loaded.map.spawn_objects(world);

    CHECK(made == 1);
    CHECK(where.x == doctest::Approx(40));
    // The registered one plus the plain object the unregistered class became.
    CHECK(world.object_count() == 2);
}

TEST_CASE_FIXTURE(Fixture, "an unregistered class becomes a plain solid object") {
    // The right default for a wall or a trigger drawn in the editor, which is
    // most of what an unregistered class is.
    World world;
    Parsed loaded("map_minimal.json");
    loaded.map.spawn_objects(world);
    REQUIRE(world.object_count() == 2);

    const std::vector<rmp::Object *> all = rmp::objects::detail::live_objects(world);
    for (const rmp::Object *object : all) {
        CHECK(object->solid);
        CHECK(object->immovable);
        CHECK(object->world_collider().width > 0);
    }
}

TEST_CASE_FIXTURE(Fixture,
                  "registering the same class twice replaces rather than stacks") {
    World world;
    Parsed loaded("map_minimal.json");
    int first = 0;
    int second = 0;
    loaded.map.on_object("enemy", [&](rmp::Scene &, const rmp::MapObject &) { ++first; });
    loaded.map.on_object("enemy",
                         [&](rmp::Scene &, const rmp::MapObject &) { ++second; });
    loaded.map.spawn_objects(world);
    CHECK(first == 0);
    CHECK(second == 1);
}

// ---------------------------------------------------------------------------
// The patch, and the failure that has to be readable
// ---------------------------------------------------------------------------

TEST_CASE("a map saved by a modern Tiled loads, which is what the patch is for") {
    // WITHOUT the patch in thirdparty/cute_tiled this fails with "Unknown
    // identifier found": the vendored parser is verified against the Tiled 1.5
    // schema and this fixture is Tiled 1.11, full of keys it has never seen --
    // parallaxx, tintcolor, repeatx, class, fillmode, tilerendersize.
    //
    // Checked by running both: the unpatched parser rejects this exact file.
    const Parsed loaded("map_modern.json");
    REQUIRE(loaded.map.valid());
    CHECK(loaded.map.layer_count() == 1);
    CHECK(loaded.map.bounds().width == doctest::Approx(32));
    CHECK(loaded.map.tile_at(0, 0, 0) == 9);
}

TEST_CASE("base64 fails, and the message names the setting to change") {
    // The parser's own words are "Compression is not yet supported", which does
    // not say what to DO -- and what to do is one setting in the editor. The
    // map comes back invalid rather than half-read.
    const Parsed loaded("map_base64.json");
    CHECK_FALSE(loaded.map.valid());
    CHECK(loaded.map.layer_count() == 0);
    CHECK(loaded.map.object_count() == 0);
}

TEST_CASE("a map reads the same under a locale with a decimal comma") {
    // cute_tiled reads every number with strtod, which follows LC_NUMERIC: a
    // game that sets a German locale for its interface read "1.5" as 1 and
    // stopped at the '.', and since every map says "version":"1.5" and
    // cute_tiled reads that as a number too, NO map loaded at all. The LDtk
    // reader had the guard (tests/ldtk_test.cpp); this reader did not.
    const std::array names{ "rmp_comma", "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8",
                            "es_ES.UTF-8" };
    const std::string was = std::setlocale(LC_NUMERIC, nullptr);
    const char *found = nullptr;
    for (const char *name : names) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr) {
            found = name;
            break;
        }
    }
    if (found == nullptr) {
        const char *must = std::getenv("RMP_REQUIRE_TEST_LOCALES");
        if (must != nullptr && std::string(must) == "1") {
            FAIL_CHECK("no locale with a decimal comma: run tools/test_locales.sh and "
                       "set LOCPATH");
        } else {
            MESSAGE("no locale with a decimal comma here: skipped");
        }
        return;
    }
    std::array<char, 16> raw{};
    std::snprintf(raw.data(), raw.size(), "%g", 0.5);
    const bool comma = std::string(raw.data()) != "0.5"; // or this tests nothing
    const Parsed p("map_minimal.json");
    const std::string during = std::setlocale(LC_NUMERIC, nullptr);
    std::setlocale(LC_NUMERIC, was.c_str());
    CHECK(comma);
    REQUIRE(p.map.valid());
    const rmp::MapObject *goblin = rmp::tilemap::detail::object_at(p.data, 0);
    REQUIRE(goblin != nullptr);
    CHECK(goblin->property_float("speed") == doctest::Approx(1.5));
    CHECK(during.find(found) != std::string::npos); // the game's locale, given back
}

TEST_CASE("rubbish is refused rather than half-read") {
    const std::string text = "{ this is not a map at all";
    const std::vector<unsigned char> junk(text.begin(), text.end());
    auto data = rmp::tilemap::detail::parse_map(junk.data(), junk.size(), "junk");
    CHECK(data == nullptr);
    CHECK(rmp::tilemap::detail::parse_map(nullptr, 10, "null") == nullptr);
    CHECK(rmp::tilemap::detail::parse_map(junk.data(), 0, "empty") == nullptr);
}

// ---------------------------------------------------------------------------
// The scene's half
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "an empty map costs a menu scene nothing") {
    World world;
    CHECK_FALSE(world.map.valid());
    CHECK(world.map.layer_count() == 0);
    CHECK(world.map.bounds().width == doctest::Approx(0));
    CHECK_FALSE(world.map.solid_at({ 0, 0 }));
    world.map.spawn_objects(world); // silent
    CHECK(world.object_count() == 0);
}

TEST_CASE_FIXTURE(Fixture, "empty bounds mean the MAP when the scene has one") {
    // A paddle in a one-screen game wants the window; a player in a Tiled level
    // wants the level. Neither should have to say so.
    World world;
    Parsed loaded("map_minimal.json");
    world.map = std::move(loaded.map);
    REQUIRE(world.map.valid());

    auto &player = world.spawn({ .position = { 32, 32 },
                                 .shape = rmp::rect({ 10, 10 }),
                                 .velocity = { -1000, 0 },
                                 .edges = rmp::Edge::CLAMP });
    rmp::objects::detail::update(world, 1.0f);
    // Stopped at the map's left edge, which is 0 -- and the map is 64 wide,
    // far smaller than the 800-wide view it would otherwise have used.
    CHECK(player.world_collider().x == doctest::Approx(0));

    auto &right = world.spawn({ .position = { 32, 32 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { 1000, 0 },
                                .edges = rmp::Edge::CLAMP });
    rmp::objects::detail::update(world, 1.0f);
    const Rectangle box = right.world_collider();
    CHECK(box.x + box.width == doctest::Approx(64));
}

TEST_CASE_FIXTURE(Fixture, "a map moves, and moving it does not free it twice") {
    World world;
    {
        Parsed loaded("map_minimal.json");
        world.map = std::move(loaded.map);
        CHECK_FALSE(loaded.map.valid()); // moved out
    }
    CHECK(world.map.valid());
    CHECK(world.map.layer_count() == 2);
}

TEST_CASE("a map moved into a new one is that map, and `if (map)` says which is which") {
    Parsed loaded("map_minimal.json");
    REQUIRE(loaded.map);
    const rmp::Tilemap moved(std::move(loaded.map));
    CHECK(static_cast<bool>(moved));
    CHECK(moved.layer_count() == 2);
    CHECK(moved.object_count() == 2);
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the promise
    CHECK_FALSE(static_cast<bool>(loaded.map));
    CHECK_FALSE(rmp::Tilemap());
}

// ---------------------------------------------------------------------------
// The tileset image: margin, spacing, and where a gid actually is in it
// ---------------------------------------------------------------------------

TEST_CASE("margin and spacing are part of the source rectangle") {
    // map_spaced.json's tileset is 4 columns of 16x16 with margin 1 and
    // spacing 2 -- the first two fields of Tiled's own import dialog, and what
    // every atlas packer produces. Without them the grid drifts by one gutter
    // per column, so the tile drawn is a slice of two neighbours, and by the
    // bottom-right of the sheet it is different art altogether.
    const Parsed loaded("map_spaced.json");
    REQUIRE(loaded.map.valid());

    SUBCASE("the first tile starts at the margin, not at the origin") {
        const Rectangle first = rmp::tilemap::detail::tile_source(loaded.data, 1);
        CHECK(first.x == doctest::Approx(1));
        CHECK(first.y == doctest::Approx(1));
        CHECK(first.width == doctest::Approx(16));
        CHECK(first.height == doctest::Approx(16));
    }
    SUBCASE("and the last of the first row is 1 + 3 * (16 + 2), not 3 * 16") {
        const Rectangle fourth = rmp::tilemap::detail::tile_source(loaded.data, 4);
        CHECK(fourth.x == doctest::Approx(55));
        CHECK(fourth.y == doctest::Approx(1));
    }
    SUBCASE("the spacing counts down the rows too") {
        const Rectangle sixth = rmp::tilemap::detail::tile_source(loaded.data, 6);
        CHECK(sixth.x == doctest::Approx(19));
        CHECK(sixth.y == doctest::Approx(19));
    }
    SUBCASE("the bottom-right tile, where the drift is more than a whole tile") {
        const Rectangle last = rmp::tilemap::detail::tile_source(loaded.data, 16);
        CHECK(last.x == doctest::Approx(55));
        CHECK(last.y == doctest::Approx(55));
    }
    SUBCASE("and a gid no tileset holds is an empty rectangle, not a guess") {
        CHECK(rmp::tilemap::detail::tile_source(loaded.data, 0).width ==
              doctest::Approx(0));
        CHECK(rmp::tilemap::detail::tile_source(loaded.data, 99).width ==
              doctest::Approx(0));
        CHECK(rmp::tilemap::detail::tile_source(nullptr, 1).width == doctest::Approx(0));
    }
}

// ---------------------------------------------------------------------------
// A tileset image that is missing, and one that is there and does not decode
// ---------------------------------------------------------------------------
//
// Both said 'is not in resources/', and a corrupt image that is right there
// sends you looking for a file you can see. tests/fixtures/corrupt/ is the
// resources root for these: broken.png is in it, missing.png is not.

namespace {

// What raylib's log said while a test listened. The dot says "file state".
struct {
    std::vector<std::string> lines;
} heard;

void hear(int /*level*/, const char *text, va_list args) {
    std::array<char, 512> line{};
    std::vsnprintf(line.data(), line.size(), text, args);
    heard.lines.emplace_back(line.data());
}

// The resources root is tests/fixtures/corrupt/ and the log is listened to,
// for the length of one test.
struct AtCorrupt {
    std::string previous;
    AtCorrupt() : previous(rmp::assets::detail::resources_root()) {
        rmp::assets::detail::set_resources_root(RMP_TEST_FIXTURES "corrupt/");
        heard.lines.clear();
        SetTraceLogCallback(hear);
    }
    ~AtCorrupt() {
        SetTraceLogCallback(nullptr);
        rmp::assets::detail::set_resources_root(previous.c_str());
    }
    AtCorrupt(const AtCorrupt &) = delete;
    AtCorrupt &operator=(const AtCorrupt &) = delete;
};

std::string tileset_line() {
    for (const std::string &line : heard.lines) {
        if (line.starts_with("MAP: the tileset image")) return line;
    }
    return "";
}

// map_minimal.json with its tileset image renamed.
Parsed with_image(const char *image) {
    const std::vector<unsigned char> raw = bytes_of("map_minimal.json");
    std::string text(raw.begin(), raw.end());
    const std::string::size_type at = text.find("\"tiles.png\"");
    REQUIRE(at != std::string::npos);
    text.replace(at, std::strlen("\"tiles.png\""), std::string("\"") + image + "\"");
    const std::vector<unsigned char> edited(text.begin(), text.end());
    return { edited, "map_minimal.json" };
}

} // namespace

TEST_CASE("a tileset image that is not there is said to be missing") {
    const AtCorrupt at;
    const Parsed loaded = with_image("missing.png");
    CHECK(loaded.map.valid());
    const std::string line = tileset_line();
    CAPTURE(line);
    CHECK(line.find("\"missing.png\" is not in resources/") != std::string::npos);
}

TEST_CASE("a tileset image that is there and does not decode is not called missing") {
    const AtCorrupt at;
    const Parsed loaded = with_image("broken.png");
    CHECK(loaded.map.valid());
    const std::string line = tileset_line();
    CAPTURE(line);
    CHECK(line.find("\"broken.png\" is in resources/") != std::string::npos);
    CHECK(line.find("does not decode") != std::string::npos);
    CHECK(line.find("is not in resources/") == std::string::npos);
}

// ---------------------------------------------------------------------------
// External tilesets -- Tiled does NOT embed by default
// ---------------------------------------------------------------------------

TEST_CASE("an external tileset is fetched, and the map is whole") {
    // Tiled's New Tileset dialog writes a separate file unless you tick
    // "Embed in map", so this is the first experience a real Tiled user has.
    // Before it was handled the map loaded, valid() was true and the bounds
    // were right -- and nothing drew and nothing was solid.
    const AtFixtures at_fixtures;
    const Parsed loaded("map_external_tileset.json");
    REQUIRE(loaded.map.valid());

    // tileset_ext.tsj: 4 columns of 16x16, and tile 5 carries `solid`.
    const Rectangle sixth = rmp::tilemap::detail::tile_source(loaded.data, 6);
    CHECK(sixth.x == doctest::Approx(16));
    CHECK(sixth.y == doctest::Approx(16));
    CHECK(sixth.width == doctest::Approx(16));

    SUBCASE("and its tile properties came with it, so the map is solid again") {
        CHECK(loaded.map.solid_at({ 24, 24 })); // column 1, row 1: gid 6
        CHECK_FALSE(loaded.map.solid_at({ 8, 8 })); // gid 1, not marked
    }
}

TEST_CASE("an external tileset that cannot be read says so, and counts as failed") {
    // tileset_external.tsx is what Tiled actually writes by default: XML. This
    // reads JSON, so the answer has to be one line that names the file and the
    // setting -- not a map that quietly draws nothing.
    const AtFixtures at_fixtures;
    rmp::detail::reset_reports_for_tests();
    const int failed_before = rmp::assets::failed_loads();

    const Parsed loaded("map_external_tsx.json");
    CHECK(loaded.map.valid()); // the MAP is fine; one tileset of it is not
    CHECK(rmp::detail::report_count() == 1);
    // The CI boot gate reads this, so a shipped game whose tileset never
    // arrived comes back red instead of green and empty.
    CHECK(rmp::assets::failed_loads() == failed_before + 1);

    SUBCASE("and nothing is claimed about tiles it does not have") {
        CHECK(rmp::tilemap::detail::tile_source(loaded.data, 1).width ==
              doctest::Approx(0));
    }
}

// ---------------------------------------------------------------------------
// Objects: the flip bits, and what is and is not a wall
// ---------------------------------------------------------------------------

TEST_CASE("a flipped tile object's gid has Tiled's flip bits taken off") {
    // Press X in the editor and Tiled sets bit 31. Read back as an int that is
    // -2147483647, so a factory switching on the gid to pick a sprite falls
    // through to its default -- and `gid == 0 means not a tile object` still
    // passes, so nothing looks wrong. The tile-layer path has always masked
    // these; the object path did not, and one loop twenty lines from the other
    // is exactly the asymmetry a test finds and reading does not.
    const std::vector<unsigned char> raw = bytes_of("map_objects_flipped.json");
    auto data = rmp::tilemap::detail::parse_map(raw.data(), static_cast<int>(raw.size()),
                                                "map_objects_flipped.json");
    REQUIRE(data != nullptr);
    REQUIRE(rmp::tilemap::detail::object_count(data.get()) == 4);

    const rmp::MapObject *chest = rmp::tilemap::detail::object_at(data.get(), 0);
    REQUIRE(chest != nullptr);
    CHECK(std::string(chest->name) == "mirrored");
    CHECK(chest->gid == 1);

    SUBCASE("and an object that is not a tile object is still 0") {
        const rmp::MapObject *point = rmp::tilemap::detail::object_at(data.get(), 1);
        REQUIRE(point != nullptr);
        CHECK(point->gid == 0);
    }
}

TEST_CASE_FIXTURE(Fixture, "a point or a zero-size object is a marker, not a wall") {
    // The other half of what an object layer holds: spawn points, camera
    // targets, waypoints, audio emitters. A Tiled point has width and height
    // 0, and turning one into a solid collider gives the player an invisible
    // wall at a spot the designer meant as a label.
    World world;
    Parsed loaded("map_objects_flipped.json");
    REQUIRE(loaded.map.valid());
    loaded.map.spawn_objects(world);
    REQUIRE(world.object_count() == 4);

    const std::vector<rmp::Object *> all = rmp::objects::detail::live_objects(world);
    const auto at = [&all](Vector2 where) -> rmp::Object * {
        for (rmp::Object *object : all) {
            if (object->position.x == doctest::Approx(where.x) &&
                object->position.y == doctest::Approx(where.y)) {
                return object;
            }
        }
        return nullptr;
    };

    const rmp::Object *chest = at({ 8, 8 }); // a 16x16 tile object, anchored bottom-left
    const rmp::Object *wall = at({ 56, 8 }); // a 16x16 rectangle
    const rmp::Object *point = at({ 32, 32 }); // a Tiled point: 0 by 0
    const rmp::Object *line = at({ 8, 48 }); // a polyline, which is also 0 by 0
    REQUIRE(chest != nullptr);
    REQUIRE(wall != nullptr);
    REQUIRE(point != nullptr);
    REQUIRE(line != nullptr);

    CHECK(chest->solid);
    CHECK(wall->solid);
    CHECK_FALSE(point->solid);
    CHECK_FALSE(line->solid);
}

// ---------------------------------------------------------------------------
// Where Tiled anchors things, which is not always the corner you would guess
// ---------------------------------------------------------------------------

TEST_CASE("a tile object hangs from its bottom edge, a rectangle from its top") {
    // Both objects in this fixture are at x 32, y 48, sixteen by sixteen. The
    // ONLY difference between them is that one has a gid -- and Tiled anchors
    // the two kinds differently: a rectangle, an ellipse, a point or a polygon
    // is placed by its TOP-left corner, while a tile object, the one you stamp
    // with a tile, hangs from its BOTTOM-left, which is where the cursor was.
    // Reading y as the top for both put every stamped chest, torch and door
    // exactly one tile into the floor.
    const std::vector<unsigned char> raw = bytes_of("map_object_anchor.json");
    auto data = rmp::tilemap::detail::parse_map(raw.data(), static_cast<int>(raw.size()),
                                                "map_object_anchor.json");
    REQUIRE(data != nullptr);
    REQUIRE(rmp::tilemap::detail::object_count(data.get()) == 2);

    const rmp::MapObject *stamped = rmp::tilemap::detail::object_at(data.get(), 0);
    const rmp::MapObject *drawn = rmp::tilemap::detail::object_at(data.get(), 1);
    REQUIRE(stamped != nullptr);
    REQUIRE(drawn != nullptr);
    REQUIRE(stamped->gid == 1);
    REQUIRE(drawn->gid == 0);

    CHECK(stamped->position.x == doctest::Approx(40));
    CHECK(stamped->position.y == doctest::Approx(40)); // y 48 is its BOTTOM
    CHECK(drawn->position.x == doctest::Approx(40));
    CHECK(drawn->position.y == doctest::Approx(56)); // y 48 is its TOP

    SUBCASE("so the two are a whole tile apart, which is the size of the bug") {
        CHECK(drawn->position.y - stamped->position.y == doctest::Approx(16));
    }
}

TEST_CASE("a tileset taller than the grid reaches up out of its cell") {
    // The same anchoring, in the tile layers. A 16x32 tileset on a 16x16 map
    // -- trees, walls, anything drawn standing up -- sits with its FOOT in the
    // cell and its head a row above. Drawn from the cell's own corner instead,
    // the whole layer is one tile too low and the tops are cut off.
    const Parsed loaded("map_tall_tiles.json");
    REQUIRE(loaded.map.valid());
    CHECK(rmp::tilemap::detail::tile_source(loaded.data, 2).height ==
          doctest::Approx(32));

    const Vector2 top_row = rmp::tilemap::detail::tile_origin(loaded.data, 2, 1, 0);
    CHECK(top_row.x == doctest::Approx(16));
    CHECK(top_row.y == doctest::Approx(-16)); // a row above the map, on purpose

    const Vector2 second_row = rmp::tilemap::detail::tile_origin(loaded.data, 3, 0, 1);
    CHECK(second_row.x == doctest::Approx(0));
    CHECK(second_row.y == doctest::Approx(0));

    SUBCASE("and a tileset the size of the grid still starts at the cell") {
        const Parsed ordinary("map_minimal.json");
        REQUIRE(ordinary.map.valid());
        const Vector2 cell = rmp::tilemap::detail::tile_origin(ordinary.data, 1, 2, 3);
        CHECK(cell.x == doctest::Approx(32));
        CHECK(cell.y == doctest::Approx(48));
    }
    SUBCASE("and a gid nothing holds is the cell's corner, not a guess") {
        const Vector2 cell = rmp::tilemap::detail::tile_origin(loaded.data, 99, 1, 1);
        CHECK(cell.x == doctest::Approx(16));
        CHECK(cell.y == doctest::Approx(16));
        CHECK(rmp::tilemap::detail::tile_origin(nullptr, 1, 1, 1).x ==
              doctest::Approx(0));
    }
}

// ---------------------------------------------------------------------------
// What fuzz_tiled found (tests/fuzz/regressions/tiled/ has each input). Every
// one of these crashed, leaked or did undefined behaviour before
// thirdparty/cute_tiled/PATCHES.md's hardening and the readers' number checks.
// ---------------------------------------------------------------------------

namespace {

std::vector<unsigned char> bytes_from(const std::string &text) {
    return { text.begin(), text.end() };
}

// A map of `layers` and `tilesets`, the rest a plain 2 x 2 grid of 16 px.
std::string tiled_map(const std::string &layers, const std::string &tilesets = "[]",
                      const std::string &size = R"("width":2,"height":2,)"
                                                R"("tilewidth":16,"tileheight":16)") {
    return "{" + size + R"(,"layers":)" + layers + R"(,"tilesets":)" + tilesets + "}";
}

} // namespace

TEST_CASE("a map cut short anywhere is refused, and nothing past it is read") {
    // A download that failed half way, a disk that filled up. cute_tiled read
    // past the end of the bytes -- a game's bytes have no NUL after them, and
    // strtod, its whitespace loop and its look-ahead scans ran on -- and
    // trapped (__builtin_trap) when it asked for a character at the end.
    // Every prefix of a real map, each in an allocation of exactly its size
    // so ASan sees a read one past it.
    SetTraceLogLevel(LOG_NONE);
    const std::vector<unsigned char> whole = bytes_of("map_minimal.json");
    std::size_t last_brace = whole.size();
    while (last_brace > 0 && whole[last_brace - 1] != '}') --last_brace;
    for (std::size_t size = 1; size + 1 < last_brace; ++size) {
        CAPTURE(size);
        const std::vector<unsigned char> cut(
            whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(size));
        CHECK(rmp::tilemap::detail::parse_map(cut.data(), static_cast<int>(cut.size()),
                                              "cut.json") == nullptr);
    }
    CHECK(Parsed(whole, "whole.json").map.valid());
    SetTraceLogLevel(LOG_INFO);
}

TEST_CASE("a map that fails half way frees what it had read") {
    // cute_tiled malloc'ed the CSV tiles, the vertices and the properties one
    // by one and freed them only with a map that loaded, so every refusal
    // after them leaked -- and a key given twice leaked its first array.
    // LeakSanitizer reads this test at exit.
    SetTraceLogLevel(LOG_NONE);
    for (
        const std::string &text :
        { std::string(R"({"layers":[{"type":"tilelayer","width":2,"height":1,)"
                      R"("data":[1,2],"opacity":}]})"),
          std::string(R"({"layers":[{"type":"objectgroup","objects":[{"polygon":)"
                      R"([{"x":1,"y":2}],"properties":[{"name":"a","type":"int",)"
                      R"("value":1}],"x":}]}]})"),
          tiled_map(
              R"([{"type":"tilelayer","width":1,"height":1,"data":[1],"data":[2]}])") }) {
        CAPTURE(text);
        const std::vector<unsigned char> raw = bytes_from(text);
        static_cast<void>(Parsed(raw, "half.json"));
    }
    SetTraceLogLevel(LOG_INFO);
}

TEST_CASE("group layers nested past 32 deep are refused, not recursed into") {
    // The parser recursed once a group, and so did the pass that freed the
    // map: a file of groups inside groups ran the stack out.
    SetTraceLogLevel(LOG_NONE);
    const auto nested = [](int depth) {
        std::string text =
            R"({"width":1,"height":1,"tilewidth":16,"tileheight":16,"layers":[)";
        for (int i = 0; i < depth; ++i) text += R"({"type":"group","layers":[)";
        for (int i = 0; i < depth; ++i) text += "]}";
        return text + "]}";
    };
    CHECK(Parsed(bytes_from(nested(31)), "groups.json").map.valid());
    CHECK_FALSE(Parsed(bytes_from(nested(33)), "groups.json").map.valid());
    CHECK_FALSE(Parsed(bytes_from(nested(100000)), "groups.json").map.valid());
    SetTraceLogLevel(LOG_INFO);
}

TEST_CASE("an object's numbers come out finite and its size never negative") {
    // cute_tiled reads a number with strtod, which takes "nan" and "inf" and
    // a 1e39 no float holds; and a corner plus half a size, each a float, can
    // sum past float's range. Each came out as the object's position.
    SetTraceLogLevel(LOG_NONE);
    for (const std::string_view object :
         { R"({"x":nan,"y":-inf,"width":16,"height":16})",
           R"({"x":1e39,"y":0,"width":16,"height":16})",
           R"({"x":3e38,"y":0,"width":3e38,"height":16,"rotation":inf})",
           R"({"x":0,"y":0,"width":-16,"height":-1})" }) {
        CAPTURE(object);
        const Parsed p(
            bytes_from(tiled_map(std::string(R"([{"type":"objectgroup","objects":[)") +
                                 std::string(object) + "]}]")),
            "numbers.json");
        REQUIRE(p.map.object_count() == 1);
        const rmp::MapObject &o = p.data->objects[0];
        CHECK(std::isfinite(o.position.x));
        CHECK(std::isfinite(o.position.y));
        CHECK(std::isfinite(o.rotation));
        CHECK(o.size.x >= 0);
        CHECK(o.size.y >= 0);
    }
    // NaN is 0, and the rest of the object is what it says.
    const Parsed p(
        bytes_from(tiled_map(
            R"([{"type":"objectgroup","objects":[{"x":nan,"y":8,"width":16,"height":16}]}])")),
        "nan.json");
    REQUIRE(p.map.object_count() == 1);
    const rmp::MapObject &o = p.data->objects[0];
    CHECK(o.position.x == doctest::Approx(8));
    CHECK(o.position.y == doctest::Approx(16));
    SetTraceLogLevel(LOG_INFO);
}

TEST_CASE("a float property past int's range reads as the nearest int") {
    SetTraceLogLevel(LOG_NONE);
    const Parsed p(
        bytes_from(tiled_map(
            R"([{"type":"objectgroup","objects":[{"x":0,"y":0,"width":1,"height":1,)"
            R"("properties":[{"name":"up","type":"float","value":1e30},)"
            R"({"name":"nan","type":"float","value":nan},)"
            R"({"name":"down","type":"float","value":-1e39}]}]}])")),
        "props.json");
    REQUIRE(p.map.object_count() == 1);
    const rmp::MapObject &o = p.data->objects[0];
    CHECK(o.property_int("up") == std::numeric_limits<int>::max());
    CHECK(o.property_int("down") == std::numeric_limits<int>::min());
    CHECK(o.property_int("nan", 7) == 0); // NaN is never kept: it is 0
    CHECK(std::isfinite(o.property_float("down")));
    SetTraceLogLevel(LOG_INFO);
}

TEST_CASE("sizes and gids past int's range are finite, and never negative") {
    // A map 100 000 tiles of 100 000 px wide, a tileset whose firstgid plus
    // its tile count passes INT_MAX, a tile size plus a spacing past it: each
    // was an int sum or product, undefined behaviour, and UBSan said so.
    SetTraceLogLevel(LOG_NONE);
    const Parsed wide(bytes_from(tiled_map("[]", "[]",
                                           R"("width":100000,"height":100000,)"
                                           R"("tilewidth":100000,"tileheight":100000)")),
                      "wide.json");
    REQUIRE(wide.map.valid());
    CHECK(wide.map.bounds().width == doctest::Approx(1e10));
    const Parsed negative(bytes_from(tiled_map("[]", "[]",
                                               R"("width":-2,"height":2,)"
                                               R"("tilewidth":16,"tileheight":-16)")),
                          "negative.json");
    CHECK(negative.map.bounds().width == doctest::Approx(0));
    CHECK(negative.map.bounds().height == doctest::Approx(0));
    CHECK(negative.map.tile_size().y == doctest::Approx(0));

    const Parsed last(
        bytes_from(tiled_map(
            R"([{"type":"tilelayer","width":1,"height":1,"data":[2147483647]}])",
            R"([{"firstgid":2147483646,"columns":1,"tilecount":4,)"
            R"("tilewidth":16,"tileheight":16}])")),
        "last.json");
    REQUIRE(last.data->tilesets.size() == 1);
    CHECK(last.data->tilesets[0].last_gid == std::numeric_limits<int>::max());
    CHECK(rmp::tilemap::detail::tile_source(last.data, 2147483647).y ==
          doctest::Approx(16));

    const Parsed spaced(
        bytes_from(tiled_map(
            R"([{"type":"tilelayer","width":2,"height":1,"data":[1,2]}])",
            R"([{"firstgid":1,"columns":2,"tilecount":2,"tilewidth":2147483647,)"
            R"("tileheight":16,"spacing":2147483647}])")),
        "spaced.json");
    const Rectangle second = rmp::tilemap::detail::tile_source(spaced.data, 2);
    CHECK(std::isfinite(second.x));
    CHECK(second.x == doctest::Approx(4294967294.0));
    const Vector2 far =
        rmp::tilemap::detail::tile_origin(wide.data, 0, 2147483647, 2147483647);
    CHECK(std::isfinite(far.x));
    CHECK(std::isfinite(far.y));
    SetTraceLogLevel(LOG_INFO);
}

TEST_CASE("a tileset that says it holds two billion tiles allocates nothing for them") {
    // tilecount is a number in the file; the solid flags were a vector of
    // that many. LDtk's budget (MAX_TILES) holds the Tiled reader too.
    SetTraceLogLevel(LOG_NONE);
    const Parsed p(bytes_from(tiled_map("[]",
                                        R"([{"firstgid":1,"columns":1,)"
                                        R"("tilecount":2147483647,"tilewidth":16,)"
                                        R"("tileheight":16}])")),
                   "many.json");
    REQUIRE(p.map.valid());
    REQUIRE(p.data->tilesets.size() == 1);
    CHECK(p.data->tilesets[0].solid.empty());
    CHECK(p.data->tilesets[0].last_gid < p.data->tilesets[0].first_gid); // holds none
    SetTraceLogLevel(LOG_INFO);
}

TEST_CASE(
    "tile layers past the cell budget are left out, and the rest of the map loads") {
    // A layer's width and height are two numbers in the file, whatever data it
    // holds, and draw() and solid_in() walk it cell by cell: 150 bytes saying
    // 100 000 x 100 000 were ten billion cells, and fuzz_tiled's walk timed
    // out on them (regressions/tiled/a_layer_past_the_cell_budget). LDtk's
    // budget, MAX_CELLS, is the whole file's here too.
    SetTraceLogLevel(LOG_NONE);
    const std::int64_t budget = rmp::tilemap::detail::MAX_CELLS;
    const auto side = static_cast<std::int64_t>(std::sqrt(static_cast<double>(budget)));
    REQUIRE(side * side == budget);
    const auto layer = [](std::int64_t width, std::int64_t height) {
        return R"({"type":"tilelayer","width":)" + std::to_string(width) +
            R"(,"height":)" + std::to_string(height) + R"(,"data":[1]})";
    };
    const auto widths = [](const std::string &layers) {
        const Parsed loaded(bytes_from(tiled_map(layers)), "cells.json");
        REQUIRE(loaded.map.valid());
        std::vector<int> out;
        out.reserve(loaded.data->layers.size());
        for (const rmp::tilemap::detail::Layer &l : loaded.data->layers)
            out.push_back(l.width);
        return out;
    };
    // Exactly the budget, in one layer: kept.
    CHECK(widths("[" + layer(side, side) + "]") ==
          std::vector<int>{ static_cast<int>(side) });
    // Past it in one: left out, and the small one after it kept.
    CHECK(widths("[" + layer(100000, 100000) + "," + layer(2, 2) + "]") ==
          std::vector<int>{ 2 });
    // The budget is the whole file's: a layer that fits, then one that does
    // not fit what is left.
    CHECK(widths("[" + layer(side, side - 1) + "," + layer(side, 2) + "," + layer(1, 1) +
                 "]") == std::vector<int>{ static_cast<int>(side), 1 });
    SetTraceLogLevel(LOG_INFO);
}

// ---------------------------------------------------------------------------
// What Mull found no test holding (tests/mutation/floors/tilemap.txt): each
// case below kills mutants that survived every test before it.
// ---------------------------------------------------------------------------

namespace {

// A map of layers with these cells (0 = the map's tile size), for the
// arithmetic that needs no reader.
rmp::Tilemap map_of_cells(std::initializer_list<std::pair<int, int>> cells,
                          int tile = 10) {
    auto data = std::make_unique<rmp::tilemap::detail::MapData>();
    data->tile_width = tile;
    data->tile_height = tile;
    for (const auto &[w, h] : cells) {
        rmp::tilemap::detail::Layer layer;
        layer.width = 1;
        layer.height = 1;
        layer.cell_width = w;
        layer.cell_height = h;
        layer.gids = { 0 };
        data->layers.push_back(std::move(layer));
    }
    rmp::Tilemap map;
    rmp::tilemap::detail::Access::adopt(
        map,
        rmp::tilemap::detail::MapPtr(data.release(), &rmp::tilemap::detail::free_map));
    return map;
}

float smallest(const rmp::Tilemap &map) {
    return rmp::tilemap::detail::smallest_cell(rmp::tilemap::detail::Access::data(map));
}

} // namespace

TEST_CASE("the smallest cell is the least side of any layer's grid, the map's for one "
          "with none") {
    // How far a moving object may step before the collision pass looks at the
    // map again (src/rmp/object.cpp): too big and it steps through a wall.
    CHECK(rmp::tilemap::detail::smallest_cell(nullptr) == doctest::Approx(0));
    CHECK(smallest(map_of_cells({})) == doctest::Approx(0));
    CHECK(smallest(map_of_cells({ { 24, 24 }, { 12, 16 }, { 16, 16 } })) ==
          doctest::Approx(12));
    CHECK(smallest(map_of_cells({ { 16, 8 } })) == doctest::Approx(8));
    CHECK(smallest(map_of_cells({ { 32, 32 }, { 0, 0 } }, 10)) == doctest::Approx(10));
    CHECK(smallest(map_of_cells({ { 0, 6 } }, 10)) == doctest::Approx(6));
    CHECK(smallest(map_of_cells({ { 4, 4 }, { 0, 0 } }, 10)) == doctest::Approx(4));
    // A map whose tiles have no size has no cell to step by.
    CHECK(smallest(map_of_cells({ { 0, 0 } }, 0)) == doctest::Approx(0));
}

TEST_CASE("object_at answers every index below object_count and nothing past it") {
    const Parsed loaded("map_minimal.json");
    REQUIRE(loaded.map.object_count() == 2);
    CHECK(rmp::tilemap::detail::object_at(loaded.data, 1) != nullptr);
    CHECK(rmp::tilemap::detail::object_at(loaded.data, 2) == nullptr);
    CHECK(rmp::tilemap::detail::object_at(loaded.data, -1) == nullptr);
    CHECK(rmp::tilemap::detail::object_at(nullptr, 0) == nullptr);
    CHECK(rmp::tilemap::detail::object_count(nullptr) == 0);
}

TEST_CASE("a tile layer holds exactly its cells, and knows its own height") {
    const Parsed loaded("map_minimal.json");
    REQUIRE(loaded.map.layer_count() == 2);
    for (const rmp::tilemap::detail::Layer &layer : loaded.data->layers) {
        CHECK(layer.width == 4);
        CHECK(layer.height == 4);
        CHECK(layer.gids.size() == 16U);
    }
}

TEST_CASE_FIXTURE(
    Fixture,
    "a Tiled object's rotation reaches the object, which the map draws instead") {
    SetTraceLogLevel(LOG_NONE);
    const std::string text = tiled_map(
        R"([{"type":"objectgroup","objects":[{"x":0,"y":0,"width":16,"height":16,)"
        R"("rotation":30},{"x":32,"y":0,"width":0,"height":16},)"
        R"({"x":64,"y":0,"width":16,"height":0}]}])");
    Parsed loaded(bytes_from(text), "rotated.json");
    REQUIRE(loaded.map.object_count() == 3);
    CHECK(loaded.data->objects[0].rotation == doctest::Approx(30));
    World world;
    loaded.map.spawn_objects(world);
    REQUIRE(world.object_count() == 3);
    for (const auto &object : rmp::objects::detail::live_objects(world)) {
        CHECK_FALSE(object->visible); // the tiles are the picture
        // A line -- no width, or no height -- is a marker, like a point.
        const bool box = object->position.x == doctest::Approx(8);
        CHECK(object->solid == box);
        if (box) CHECK(object->rotation == doctest::Approx(30));
    }
    SetTraceLogLevel(LOG_INFO);
}

TEST_CASE("the first and the last tile of a tileset can be marked solid") {
    SetTraceLogLevel(LOG_NONE);
    const auto solid_with = [](int gid) {
        const std::string text = tiled_map(
            R"([{"type":"tilelayer","width":1,"height":1,"data":[)" +
                std::to_string(gid) + "]}]",
            R"([{"firstgid":3,"columns":2,"tilecount":4,"tilewidth":16,"tileheight":16,)"
            R"("tiles":[{"id":0,"properties":[{"name":"solid","type":"bool","value":true}]},)"
            R"({"id":3,"properties":[{"name":"solid","type":"bool","value":true}]}]}])");
        const Parsed loaded(bytes_from(text), "ends.json");
        REQUIRE(loaded.map.valid());
        return loaded.map.solid_at({ 8, 8 });
    };
    CHECK(solid_with(3)); // the first, firstgid itself
    CHECK_FALSE(solid_with(4));
    CHECK(solid_with(6)); // the last, firstgid + tilecount - 1
    CHECK_FALSE(solid_with(7)); // past it: no tileset's
    SetTraceLogLevel(LOG_INFO);
}

TEST_CASE("a NaN float property is the fallback to property_int, never an int") {
    // The readers never keep one (finite_float); property_int does not
    // trust them to.
    auto data = std::make_unique<rmp::tilemap::detail::MapData>();
    auto info = std::make_unique<rmp::tilemap::detail::ObjectInfo>();
    rmp::tilemap::detail::Property nan;
    nan.key = "speed";
    nan.kind = rmp::tilemap::detail::Property::Kind::FLOAT;
    nan.floating = std::numeric_limits<float>::quiet_NaN();
    info->properties.push_back(nan);
    const rmp::MapObject &object = data->add_object(std::move(info));
    CHECK(object.property_int("speed", 7) == 7);
    CHECK(object.property_int("speed", -7) == -7);
}

TEST_CASE(
    "the tile budget is the whole map's: a set that fits it whole, and none after") {
    SetTraceLogLevel(LOG_NONE);
    const std::int64_t budget = rmp::tilemap::detail::MAX_TILES;
    const std::string all = std::to_string(budget);
    const std::string half = std::to_string((budget / 2) + 1);
    const auto counts_of = [](const std::string &tilesets) {
        const Parsed loaded(bytes_from(tiled_map("[]", tilesets)), "budget.json");
        REQUIRE(loaded.map.valid());
        std::vector<std::size_t> out;
        out.reserve(loaded.data->tilesets.size());
        for (const rmp::tilemap::detail::Tileset &set : loaded.data->tilesets)
            out.push_back(set.solid.size());
        return out;
    };
    // Exactly the budget: every tile of it.
    CHECK(counts_of(R"([{"firstgid":1,"columns":2048,"tilecount":)" + all +
                    R"(,"tilewidth":1,"tileheight":1}])") ==
          std::vector<std::size_t>{ static_cast<std::size_t>(budget) });
    // Two halves and a tile more: the second does not fit what is left.
    CHECK(counts_of(R"([{"firstgid":1,"columns":1,"tilecount":)" + half +
                    R"(,"tilewidth":1,"tileheight":1},{"firstgid":5000000,"columns":1,)"
                    R"("tilecount":)" +
                    half + R"(,"tilewidth":1,"tileheight":1}])") ==
          std::vector<std::size_t>{ static_cast<std::size_t>((budget / 2) + 1), 0U });
    SetTraceLogLevel(LOG_INFO);
}

TEST_CASE("a Tiled tileset of no columns is one column") {
    SetTraceLogLevel(LOG_NONE);
    const Parsed loaded(
        bytes_from(tiled_map(R"([{"type":"tilelayer","width":1,"height":1,"data":[3]}])",
                             R"([{"firstgid":1,"columns":0,"tilecount":4,"tilewidth":16,)"
                             R"("tileheight":16}])")),
        "columns.json");
    REQUIRE(loaded.map.valid());
    const Rectangle third = rmp::tilemap::detail::tile_source(loaded.data, 3);
    CHECK(third.x == doctest::Approx(0));
    CHECK(third.y == doctest::Approx(32));
    SetTraceLogLevel(LOG_INFO);
}
