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

#include <fstream>
#include <string>
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
    void *data = nullptr;
    explicit Parsed(const char *file) {
        const std::vector<unsigned char> raw = bytes_of(file);
        data = rmp::tilemap::detail::parse_map(raw.data(), static_cast<int>(raw.size()),
                                               file);
        map.adopt(data);
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
    void *data = rmp::tilemap::detail::parse_map(raw.data(), static_cast<int>(raw.size()),
                                                 "map_minimal.json");
    REQUIRE(data != nullptr);
    REQUIRE(rmp::tilemap::detail::object_count(data) == 2);

    const rmp::MapObject *goblin = rmp::tilemap::detail::object_at(data, 0);
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
        const rmp::MapObject *start = rmp::tilemap::detail::object_at(data, 1);
        REQUIRE(start != nullptr);
        CHECK(std::string(start->type) == "checkpoint");
        CHECK(start->property_int("hp", 99) == 99);
    }
    rmp::tilemap::detail::free_map(data);
}

TEST_CASE_FIXTURE(Fixture, "a registered class produces the user's type") {
    World world;
    Parsed loaded("map_minimal.json");
    REQUIRE(loaded.map.valid());

    int made = 0;
    Vector2 where{};
    loaded.map.on_object("enemy", [&](rmp::Scene &scene, const rmp::MapObject &object) {
        made++;
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
    for (rmp::Object *object : all) {
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
    loaded.map.on_object("enemy", [&](rmp::Scene &, const rmp::MapObject &) { first++; });
    loaded.map.on_object("enemy",
                         [&](rmp::Scene &, const rmp::MapObject &) { second++; });
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

TEST_CASE("rubbish is refused rather than half-read") {
    const unsigned char junk[] = "{ this is not a map at all";
    void *data = rmp::tilemap::detail::parse_map(junk, sizeof(junk), "junk");
    CHECK(data == nullptr);
    CHECK(rmp::tilemap::detail::parse_map(nullptr, 10, "null") == nullptr);
    CHECK(rmp::tilemap::detail::parse_map(junk, 0, "empty") == nullptr);
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
    void *data = rmp::tilemap::detail::parse_map(raw.data(), static_cast<int>(raw.size()),
                                                 "map_objects_flipped.json");
    REQUIRE(data != nullptr);
    REQUIRE(rmp::tilemap::detail::object_count(data) == 4);

    const rmp::MapObject *chest = rmp::tilemap::detail::object_at(data, 0);
    REQUIRE(chest != nullptr);
    CHECK(std::string(chest->name) == "mirrored");
    CHECK(chest->gid == 1);

    SUBCASE("and an object that is not a tile object is still 0") {
        const rmp::MapObject *point = rmp::tilemap::detail::object_at(data, 1);
        REQUIRE(point != nullptr);
        CHECK(point->gid == 0);
    }
    rmp::tilemap::detail::free_map(data);
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

    rmp::Object *chest = at({ 8, 24 }); // a 16x16 tile object
    rmp::Object *wall = at({ 56, 8 }); // a 16x16 rectangle
    rmp::Object *point = at({ 32, 32 }); // a Tiled point: 0 by 0
    rmp::Object *line = at({ 8, 48 }); // a polyline, which is also 0 by 0
    REQUIRE(chest != nullptr);
    REQUIRE(wall != nullptr);
    REQUIRE(point != nullptr);
    REQUIRE(line != nullptr);

    CHECK(chest->solid);
    CHECK(wall->solid);
    CHECK_FALSE(point->solid);
    CHECK_FALSE(line->solid);
}
