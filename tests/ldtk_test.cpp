// ===========================================================================
// LDtk projects: the reader in src/rmp/ldtk.cpp, without a GPU.
//
// Two kinds of fixture (tests/fixtures/ldtk/README.md says where each came
// from), for two different questions:
//
//   minimal.ldtk   written by hand, small enough to know every number in it.
//                  "Does each value end up where it should?"
//   the samples    saved by LDtk 1.5.3 itself, from its installer. "Does the
//                  reader understand what the editor really writes?" -- and
//                  they are checked against LDtk's OWN arithmetic: every tile
//                  against the `src` rectangle LDtk stored next to it, every
//                  entity against the `__worldX`/`__worldY` LDtk computed. A
//                  reader can agree with its author's idea of the format and
//                  still disagree with the editor; these cannot.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/internal.h"
#include "../src/rmp/json_internal.h"
#include "../src/rmp/object_internal.h"
#include "../src/rmp/tilemap_internal.h"

#include <rmp/assets.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/tilemap.h>

#include <cJSON.h>
#include <raylib.h>

#include <algorithm>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {

using rmp::detail::Json;
using rmp::tilemap::detail::Layer;
using rmp::tilemap::detail::MapData;
using rmp::tilemap::detail::PlacedTile;

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
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
};

std::string ldtk_dir() { return std::string(RMP_TEST_FIXTURES) + "ldtk/"; }

std::string text_of(const std::string &file) {
    std::ifstream in(ldtk_dir() + file, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "missing " << ldtk_dir() << file);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

// The level files and tileset images are fetched through rmp::assets, which
// looks in the resources root -- so for one test the root is the LDtk folder.
// Put back after, because it is process-wide.
struct AtLdtk {
    std::string previous;
    AtLdtk() : previous(rmp::assets::detail::resources_root()) {
        rmp::assets::detail::set_resources_root(ldtk_dir().c_str());
    }
    ~AtLdtk() { rmp::assets::detail::set_resources_root(previous.c_str()); }
    AtLdtk(const AtLdtk &) = delete;
    AtLdtk &operator=(const AtLdtk &) = delete;
};

// None of the art the samples name is here, and parsing every level of them
// would print a warning per missing tileset. Quiet while a test parses in
// bulk; put back after.
struct Quiet {
    Quiet() { SetTraceLogLevel(LOG_NONE); }
    ~Quiet() { SetTraceLogLevel(LOG_INFO); }
    Quiet(const Quiet &) = delete;
    Quiet &operator=(const Quiet &) = delete;
};

// A parsed level that frees itself, through the same dispatch load_map uses.
struct Parsed {
    rmp::Tilemap map;
    const MapData *data = nullptr;
    Parsed(const std::string &text, const char *name, const char *level = "") {
        map.adopt(rmp::tilemap::detail::parse_map(
            text.data(), static_cast<int>(text.size()), name, level));
        data = map.detail_data();
    }
};

const rmp::MapObject *object_of_type(const MapData *data, const char *type, int nth = 0) {
    for (int i = 0; i < rmp::tilemap::detail::object_count(data); i++) {
        const rmp::MapObject *o = rmp::tilemap::detail::object_at(data, i);
        if (std::string(o->type) == type && nth-- == 0) return o;
    }
    return nullptr;
}

const rmp::MapObject *object_by_iid(const MapData *data, const char *iid) {
    for (int i = 0; i < rmp::tilemap::detail::object_count(data); i++) {
        const rmp::MapObject *o = rmp::tilemap::detail::object_at(data, i);
        if (std::string(o->iid) == iid) return o;
    }
    return nullptr;
}

const Layer *layer_named(const MapData *data, const char *name) {
    for (const Layer &layer : data->layers) {
        if (layer.name == name) return &layer;
    }
    return nullptr;
}

double num(const cJSON *o, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, key);
    REQUIRE(cJSON_IsNumber(item));
    return item->valuedouble;
}

double pair(const cJSON *o, const char *key, int i) {
    const cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o, key), i);
    REQUIRE(cJSON_IsNumber(item));
    return item->valuedouble;
}

const char *str(const cJSON *o, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}

constexpr const char *SAMPLES[] = {
    "Test_file_for_API_showing_all_features.ldtk",
    "Typical_2D_platformer_example.ldtk",
    "Typical_TopDown_example.ldtk",
};

} // namespace

TEST_SUITE("ldtk") {
    // -----------------------------------------------------------------------
    // minimal.ldtk, value by value
    // -----------------------------------------------------------------------

    TEST_CASE("the level's size, grid and place in the world come out of the file") {
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk");
        REQUIRE(p.map.valid());
        CHECK(std::string(p.map.level()) == "Start");
        const Rectangle b = p.map.bounds();
        CHECK(b.x == doctest::Approx(0));
        CHECK(b.y == doctest::Approx(0));
        CHECK(b.width == doctest::Approx(64));
        CHECK(b.height == doctest::Approx(48));
        CHECK(p.map.tile_size().x == doctest::Approx(16));
        CHECK(p.data->width == 4);
        CHECK(p.data->height == 3);
        CHECK(p.map.layer_count() == 3); // the entity layer is objects, not a layer
        CHECK(p.data->source == "minimal.ldtk");
    }

    TEST_CASE("layers come bottom first, which is the reverse of the file") {
        // LDtk lists Things, Ground, Fine, Collisions: top first.
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk");
        REQUIRE(p.data->layers.size() == 3);
        CHECK(p.data->layers[0].name == "Collisions");
        CHECK(p.data->layers[1].name == "Fine");
        CHECK(p.data->layers[2].name == "Ground");
        CHECK_FALSE(p.data->layers[0].visible); // hidden in the editor
        CHECK(p.data->layers[2].visible);
        CHECK(p.data->layers[2].opacity == doctest::Approx(0.5));
        CHECK(p.data->layers[2].offset.x == doctest::Approx(4));
        CHECK(p.data->layers[2].offset.y == doctest::Approx(-2));
        CHECK(p.data->layers[1].cell_width == 8);
        CHECK(p.data->layers[1].width == 8);
        CHECK(p.data->layers[1].height == 6);
    }

    TEST_CASE(
        "an IntGrid value called solid is solid, in any case, and no other value is") {
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk");
        // Collisions: value 1 is "Solid" -- what LDtk's default identifier
        // style makes of "solid" -- 2 is water and 3 has no name.
        CHECK(p.map.solid_at({ 56, 8 })); // column 3, row 0: 1
        CHECK(p.map.solid_at({ 8, 40 })); // column 0, row 2: 1
        CHECK_FALSE(p.map.solid_at({ 24, 24 })); // water
        CHECK_FALSE(p.map.solid_at({ 40, 40 })); // value 3, nameless
        CHECK_FALSE(p.map.solid_at({ 8, 8 })); // 0
        // Fine, an 8 px grid whose value 1 is "SOLID": only column 5, row 1.
        CHECK(p.map.solid_at({ 44, 12 }));
        CHECK_FALSE(p.map.solid_at({ 36, 12 }));
        CHECK_FALSE(p.map.solid_at({ 44, 4 }));
        // Outside the level, nothing.
        CHECK_FALSE(p.map.solid_at({ -1, 8 }));
        CHECK_FALSE(p.map.solid_at({ 64, 8 }));
    }

    TEST_CASE(
        "a tile tagged solid makes its cell solid even under another, at the offset") {
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk");
        // Ground, shifted by (4, -2): its cell (1, 2) holds tile 5, tagged
        // Solid, with tile 2 stacked on top. World x 20..36, y 30..46. At
        // (34, 31) Collisions is 0 there, so only the tag can say solid --
        // and only if the offset is applied, because without it (34, 31) is
        // Ground's empty cell (2, 1).
        CHECK(p.map.solid_at({ 34, 31 }));
        CHECK(p.map.tile_at(2, 1, 2) == 3); // the TOP tile: gid 1 + 2
        // Ground (0, 2) holds tile 0, untagged; Collisions there is water.
        CHECK_FALSE(p.map.solid_at({ 19, 31 }));
        CHECK(p.map.tile_at(2, 0, 2) == 1);
        // Tile 6 is tagged Water: a tag that is not solid is nothing.
        CHECK_FALSE(p.data->tilesets[0].solid[6]);
        CHECK(p.data->tilesets[0].solid[5]);
    }

    TEST_CASE("solid_in asks each layer in its own grid") {
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk");
        // x 33..39, y 1..7: Collisions' cell (2, 0) and Fine's (4, 0), both
        // empty. The middle of Collisions' cell, (40, 8), IS solid -- in
        // Fine's grid -- and asking the whole map about that middle point is
        // what the Tiled-only version did. Mixed grids made it a false wall.
        CHECK_FALSE(p.map.solid_in({ 33, 1, 6, 6 }));
        CHECK(p.map.solid_in({ 40, 8, 4, 4 }));
        CHECK(p.map.solid_in({ 36, 4, 6, 6 })); // reaches into Fine (5, 1)
        // Wholly left of the map: a truncating division once called that
        // column 0.
        CHECK_FALSE(p.map.solid_in({ -10, 34, 5, 5 }));
    }

    TEST_CASE("tiles: where, which, flipped, faded, and the ones that are not") {
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk");
        const Layer *ground = layer_named(p.data, "Ground");
        REQUIRE(ground != nullptr);
        // Six in the file; tile 99 does not exist in a 16-tile set.
        REQUIRE(ground->placed.size() == 5);
        const PlacedTile &solid = ground->placed[1];
        CHECK(solid.gid == 6);
        CHECK(solid.at.x == doctest::Approx(16));
        CHECK(solid.at.y == doctest::Approx(32));
        CHECK(solid.flip_x);
        CHECK_FALSE(solid.flip_y);
        CHECK(solid.alpha == doctest::Approx(0.25));
        CHECK_FALSE(ground->placed[2].flip_x);
        CHECK(ground->placed[2].flip_y);
        CHECK(ground->placed[3].flip_x);
        CHECK(ground->placed[3].flip_y);
        CHECK(p.map.tile_at(2, 3, 0) == 16);
        // Tile at x 64 is past the layer's four columns: drawn, not gridded.
        CHECK(ground->placed[4].at.x == doctest::Approx(64));

        // Padding 1 and spacing 2 are in the source rectangle: tile 5 is
        // column 1, row 1 of four columns.
        const Rectangle src = rmp::tilemap::detail::tile_source(p.data, 6);
        CHECK(src.x == doctest::Approx(19));
        CHECK(src.y == doctest::Approx(19));
        CHECK(src.width == doctest::Approx(16));
        // The second tileset numbers on from the first, used or not, so a gid
        // is the same tile whichever level is loaded.
        CHECK(p.data->tilesets[1].first_gid == 17);
        CHECK(p.data->tilesets[1].last_gid == 20);
        const Rectangle prop = rmp::tilemap::detail::tile_source(p.data, 20);
        CHECK(prop.x == doctest::Approx(8));
        CHECK(prop.y == doctest::Approx(8));
        CHECK(prop.width == doctest::Approx(8));
    }

    TEST_CASE("entities: the centre from the pivot, at the layer's offset") {
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk");
        REQUIRE(p.map.object_count() == 3);
        // The entity layer is shifted by (2, 0).
        const rmp::MapObject *player = object_of_type(p.data, "Player");
        REQUIRE(player != nullptr);
        CHECK(std::string(player->iid) == "player-1");
        CHECK(std::string(player->name).empty());
        CHECK(player->size.x == doctest::Approx(16));
        CHECK(player->size.y == doctest::Approx(24));
        // px (24, 40) is the middle of its bottom edge (pivot 0.5, 1).
        CHECK(player->position.x == doctest::Approx(26));
        CHECK(player->position.y == doctest::Approx(28));
        // Pivot 0, 0: px is the top-left corner.
        const rmp::MapObject *enemy = object_of_type(p.data, "Enemy");
        REQUIRE(enemy != nullptr);
        CHECK(enemy->position.x == doctest::Approx(42));
        CHECK(enemy->position.y == doctest::Approx(24));
        const rmp::MapObject *door = object_by_iid(p.data, "door-1");
        REQUIRE(door != nullptr);
        CHECK(door->position.x == doctest::Approx(58));
        CHECK(door->position.y == doctest::Approx(24));
        // In the file's order.
        CHECK(std::string(rmp::tilemap::detail::object_at(p.data, 0)->type) == "Player");
        CHECK(std::string(rmp::tilemap::detail::object_at(p.data, 2)->type) == "Door");
    }

    TEST_CASE("fields: every kind that is read, and a null one is the fallback") {
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk");
        const rmp::MapObject *e = object_of_type(p.data, "Enemy");
        REQUIRE(e != nullptr);
        CHECK(e->property_int("hp") == 3);
        CHECK(e->property_float("hp") == doctest::Approx(3));
        CHECK(e->property_float("speed") == doctest::Approx(1.5));
        CHECK(e->property_int("speed") == 1);
        CHECK(e->property_bool("boss"));
        CHECK(std::string(e->property_string("name")) == "Bob");
        CHECK(std::string(e->property_string("note")) == "a\nb");
        CHECK(std::string(e->property_string("kind")) == "Slime"); // an enum
        CHECK(std::string(e->property_string("tint")) == "#FF8000"); // a colour
        CHECK(std::string(e->property_string("target")) == "door-1"); // an EntityRef
        CHECK(std::string(e->property_string("hp", "x")) == "x"); // not text

        // A Point is the world centre of its cell: grid 16, offset (2, 0).
        const Vector2 home = e->property_point("home", { -1, -1 });
        CHECK(home.x == doctest::Approx(10));
        CHECK(home.y == doctest::Approx(8));
        CHECK(e->property_count("home") == 1);
        // Array<Point>, with the null in the middle left out.
        REQUIRE(e->property_count("patrol") == 2);
        CHECK(e->property_point("patrol", 0).x == doctest::Approx(26));
        CHECK(e->property_point("patrol", 0).y == doctest::Approx(40));
        CHECK(e->property_point("patrol", 1).x == doctest::Approx(58));
        CHECK(e->property_point("patrol", 1).y == doctest::Approx(40));
        CHECK(e->property_point("patrol").x == doctest::Approx(26)); // the first
        CHECK(e->property_point("patrol", 2, { -1, -1 }).x == doctest::Approx(-1));
        CHECK(e->property_point("patrol", -1, { -1, -1 }).x == doctest::Approx(-1));
        CHECK(e->property_count("empty") == 0);
        CHECK(e->property_point("empty", { -7, -7 }).x == doctest::Approx(-7));
        CHECK(e->property_point("hp", { -7, -7 }).x == doctest::Approx(-7));
        CHECK(e->property_count("hp") == 1);

        // Null is "nobody set it": the fallback, and not there at all.
        CHECK(e->property_int("unset", 42) == 42);
        CHECK(e->property_count("unset") == 0);
        CHECK(e->property_point("nowhere", { 5, 5 }).x == doctest::Approx(5));
        CHECK(e->property_count("nowhere") == 0);
        // Kinds not read yet: FilePath, Array<Int>.
        CHECK(e->property_count("file") == 0);
        CHECK(std::string(e->property_string("file", "fb")) == "fb");
        CHECK(e->property_count("ints") == 0);
        CHECK(e->property_count("no such field") == 0);
    }

    TEST_CASE_FIXTURE(Fixture, "an entity with no factory is a thing, never a wall") {
        // Every LDtk entity has a box -- 16x16 unless resized -- and in Tiled
        // an object with an area and no factory becomes a wall. An LDtk
        // player start that came out as an invisible solid block would be
        // the first thing anybody hit.
        Parsed p(text_of("minimal.ldtk"), "minimal.ldtk");
        World world;
        int doors = 0;
        p.map.on_object("Door",
                        [&doors](rmp::Scene &, const rmp::MapObject &) { doors++; });
        p.map.spawn_objects(world);
        CHECK(doors == 1);
        CHECK(world.object_count() == 2); // the player and the enemy, plain
        for (rmp::Object *o : rmp::objects::detail::live_objects(world)) {
            CHECK_FALSE(o->solid);
            CHECK(o->shape.size.x > 0); // they DO have an area
        }
    }

    TEST_CASE("the level is picked by name, and its world place comes with it") {
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk", "Next");
        REQUIRE(p.map.valid());
        CHECK(std::string(p.map.level()) == "Next");
        const Rectangle b = p.map.bounds();
        CHECK(b.x == doctest::Approx(64));
        CHECK(b.y == doctest::Approx(-16));
        CHECK(b.width == doctest::Approx(32));
        CHECK(b.height == doctest::Approx(64));
        const rmp::MapObject *coin = object_of_type(p.data, "Coin");
        REQUIRE(coin != nullptr);
        CHECK(coin->position.x == doctest::Approx(72));
        CHECK(coin->position.y == doctest::Approx(-8));
        const Vector2 target = coin->property_point("target");
        CHECK(target.x == doctest::Approx(88));
        CHECK(target.y == doctest::Approx(8));
        // Solids at the world position too: value 1 at cells 0 and 7.
        CHECK(p.map.solid_at({ 72, -8 }));
        CHECK(p.map.solid_at({ 88, 40 }));
        CHECK_FALSE(p.map.solid_at({ 72, 8 }));
        CHECK_FALSE(p.map.solid_at({ 8, 8 })); // where Start's solid would be
    }

    TEST_CASE("a level that does not exist is no map") {
        const Quiet quiet;
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk", "Nope");
        CHECK_FALSE(p.map.valid());
        const Parsed q(text_of("minimal.ldtk"), "minimal.ldtk", "start"); // case matters
        CHECK_FALSE(q.map.valid());
    }

    TEST_CASE("neighbour_at names the OTHER level at a point, and only that") {
        const Parsed start(text_of("minimal.ldtk"), "minimal.ldtk");
        CHECK(std::string(start.map.neighbour_at({ 70, 0 })) == "Next");
        CHECK(std::string(start.map.neighbour_at({ 95, 47 })) == "Next");
        // Inside this level: no neighbour, so "walk out and change" is one if.
        CHECK(std::string(start.map.neighbour_at({ 10, 10 })).empty());
        CHECK(std::string(start.map.neighbour_at({ 200, 0 })).empty());
        CHECK(std::string(start.map.neighbour_at({ 70, -17 })).empty()); // above Next
        const Parsed next(text_of("minimal.ldtk"), "minimal.ldtk", "Next");
        CHECK(std::string(next.map.neighbour_at({ 10, 10 })) == "Start");
        CHECK(std::string(next.map.neighbour_at({ 70, 0 })).empty());
        // An empty map has no world.
        const rmp::Tilemap empty;
        CHECK(std::string(empty.neighbour_at({ 0, 0 })).empty());
        CHECK(std::string(empty.level()).empty());
    }

    TEST_CASE("a project of several worlds: neighbours stay in their own world") {
        // With LDtk's multi-world flag the levels live in worlds[] and the
        // top-level `levels` is empty. Two worlds can overlap in coordinates
        // -- they are separate maps -- so a level of the other world is never
        // a neighbour. And a world with a linear layout has no coordinates.
        const std::string level = R"("pxWid":16,"pxHei":16,"layerInstances":[])";
        const std::string doc = R"({"defs":{},"levels":[],"worlds":[)"
                                R"({"identifier":"W1","worldLayout":"Free","levels":[)"
                                R"({"identifier":"A","worldX":0,"worldY":0,)" +
            level + "}," + R"({"identifier":"B","worldX":16,"worldY":0,)" + level +
            "}]}," +
            R"({"identifier":"W2","worldLayout":"Free","levels":[)"
            R"({"identifier":"C","worldX":32,"worldY":0,)" +
            level + "}]}," +
            R"({"identifier":"W3","worldLayout":"LinearHorizontal","levels":[)"
            R"({"identifier":"D","worldX":-1,"worldY":-1,)" +
            level + "}]}]}";
        const Parsed a(doc, "worlds.ldtk");
        REQUIRE(a.map.valid());
        CHECK(std::string(a.map.level()) == "A"); // the first, of the first world
        CHECK(std::string(a.map.neighbour_at({ 20, 8 })) == "B");
        CHECK(std::string(a.map.neighbour_at({ 40, 8 })).empty()); // C: world 2
        const Parsed c(doc, "worlds.ldtk", "C");
        REQUIRE(c.map.valid());
        CHECK(c.map.bounds().x == doctest::Approx(32));
        CHECK(std::string(c.map.neighbour_at({ 20, 8 })).empty()); // B: world 1
        const Parsed d(doc, "worlds.ldtk", "D");
        REQUIRE(d.map.valid());
        CHECK(d.map.bounds().x == doctest::Approx(0)); // not -1: there is no world
        CHECK(std::string(d.map.neighbour_at({ 20, 8 })).empty());
    }

    TEST_CASE("only the tilesets the level draws with are loaded") {
        // Start draws with Tiles (twice: Ground and Collisions' auto-tiles)
        // and never with Props: one image asked for, not two, and not three.
        const Quiet quiet;
        const AtLdtk at;
        const int before = rmp::assets::requested_loads();
        const Parsed start(text_of("minimal.ldtk"), "minimal.ldtk");
        CHECK(rmp::assets::requested_loads() == before + 1);
        // Next draws no tiles at all.
        const int mid = rmp::assets::requested_loads();
        const Parsed next(text_of("minimal.ldtk"), "minimal.ldtk", "Next");
        CHECK(rmp::assets::requested_loads() == mid);
    }

    TEST_CASE("numbers read the same under a locale with a decimal comma") {
        // A game with a German interface reads its levels like anyone else:
        // cJSON's strtod follows LC_NUMERIC, and without the guard the 0.5
        // opacity, the 1.5 speed and every 0.5 pivot read as 0.
        const char *names[] = { "rmp_comma", "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8",
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
                FAIL_CHECK(
                    "no locale with a decimal comma: run tools/test_locales.sh and "
                    "set LOCPATH");
            } else {
                MESSAGE("no locale with a decimal comma here: skipped");
            }
            return;
        }
        char raw[16];
        std::snprintf(raw, sizeof raw, "%g", 0.5);
        const bool comma = std::string(raw) != "0.5"; // or this tests nothing
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk");
        const std::string during = std::setlocale(LC_NUMERIC, nullptr);
        std::setlocale(LC_NUMERIC, was.c_str());
        CHECK(comma);
        REQUIRE(p.map.valid());
        CHECK(layer_named(p.data, "Ground")->opacity == doctest::Approx(0.5));
        CHECK(object_of_type(p.data, "Enemy")->property_float("speed") ==
              doctest::Approx(1.5));
        CHECK(object_of_type(p.data, "Player")->position.x == doctest::Approx(26));
        CHECK(during.find(found) != std::string::npos); // the game's locale, given back
    }

    // -----------------------------------------------------------------------
    // Hostile input
    // -----------------------------------------------------------------------

    TEST_CASE("what is not a project is refused, and says so") {
        const Quiet quiet;
        const char *bad[] = {
            "",
            "not json",
            "[]",
            "42",
            "{}",
            R"({"defs":{}})",
            R"({"levels":[{}]})",
            R"({"defs":[],"levels":[{}]})",
            R"({"defs":{},"levels":{}})",
            R"({"defs":{},"levels":[]})",
            // A level with no layers and no file to find them in.
            R"({"defs":{},"levels":[{"identifier":"A","layerInstances":null}]})",
            R"({"defs":{},"levels":[{"identifier":"A"}]})",
        };
        for (const char *doc : bad) {
            CAPTURE(doc);
            const std::string text(doc);
            const Parsed p(text, "bad.ldtk");
            CHECK_FALSE(p.map.valid());
        }
        // A project and its layers, but nothing in them: a valid, empty map.
        const Parsed empty(
            R"({"defs":{},"levels":[{"identifier":"A","pxWid":32,"pxHei":16,)"
            R"("layerInstances":[]}]})",
            "empty.ldtk");
        REQUIRE(empty.map.valid());
        CHECK(empty.map.bounds().width == doctest::Approx(32));
        CHECK(empty.map.layer_count() == 0);
    }

    TEST_CASE("every value of the project, replaced by the wrong type, is survived") {
        // Each node of minimal.ldtk in turn is replaced -- KEEPING ITS KEY, so
        // {"t": 5} becomes {"t": 1e300} and not {"": 1e300}, which is what
        // replacing it as an array element did, deleting every field instead
        // of corrupting it -- and the result is parsed. A number becomes each
        // of the numbers a file can hold that hurt (1e300, -1e300, INT_MAX,
        // -1) and a string; anything else becomes two of a string, an array,
        // null, an object and a huge number, rotating, so every kind meets
        // every depth. Nothing may crash, read out of bounds, allocate without
        // bound or hang: the reader refuses the document or reads what it
        // can. The UBSan build (`rmp test sanitize`) is what sees an int
        // overflow or a float cast that happens to land somewhere harmless.
        // Of a long array (a CSV, a list of tiles) only the first two elements
        // are walked: the rest are the same role again. The clock is part of
        // the test, because `rmp test` runs this twice.
        const Quiet quiet;
        const Json original(cJSON_Parse(text_of("minimal.ldtk").c_str()));
        REQUIRE(original != nullptr);

        struct Node {
            std::vector<int> path;
            bool number;
        };
        std::vector<Node> nodes;
        const std::function<void(const cJSON *, std::vector<int> &)> walk =
            [&](const cJSON *node, std::vector<int> &path) {
                if (!path.empty()) nodes.push_back({ path, cJSON_IsNumber(node) != 0 });
                int i = 0;
                for (const cJSON *child = node->child; child != nullptr;
                     child = child->next) {
                    if (cJSON_IsArray(node) && i >= 2) break;
                    path.push_back(i++);
                    walk(child, path);
                    path.pop_back();
                }
            };
        std::vector<int> root_path;
        walk(original.get(), root_path);
        REQUIRE(nodes.size() > 300);

        enum Kind { STRING, ARRAY, NUL, OBJECT, HUGE_UP, HUGE_DOWN, INT_TOP, MINUS_ONE };
        const auto hostile = [](Kind kind) -> cJSON * {
            switch (kind) {
                case STRING:
                    return cJSON_CreateString("x");
                case ARRAY:
                    return cJSON_CreateArray();
                case NUL:
                    return cJSON_CreateNull();
                case OBJECT:
                    return cJSON_CreateObject();
                case HUGE_UP:
                    return cJSON_CreateNumber(1e300);
                case HUGE_DOWN:
                    return cJSON_CreateNumber(-1e300);
                case INT_TOP:
                    return cJSON_CreateNumber(2147483647.0);
                default:
                    return cJSON_CreateNumber(-1);
            }
        };

        const auto start = std::chrono::steady_clock::now();
        int parsed = 0;
        int valid = 0;
        int keyed = 0;
        for (std::size_t n = 0; n < nodes.size(); n++) {
            const Node &node = nodes[n];
            std::vector<Kind> kinds;
            if (node.number) {
                kinds = { HUGE_UP, HUGE_DOWN, INT_TOP, MINUS_ONE, STRING };
            } else {
                const Kind general[] = { STRING, ARRAY, NUL, OBJECT, HUGE_UP };
                kinds = { general[n % 5], general[(n + 1) % 5] };
            }
            for (Kind kind : kinds) {
                const Json copy(cJSON_Duplicate(original.get(), 1));
                cJSON *parent = copy.get();
                for (std::size_t d = 0; d + 1 < node.path.size(); d++) {
                    parent = cJSON_GetArrayItem(parent, node.path[d]);
                }
                REQUIRE(parent != nullptr);
                const cJSON *child = cJSON_GetArrayItem(parent, node.path.back());
                REQUIRE(child != nullptr);
                if (cJSON_IsObject(parent)) {
                    REQUIRE(child->string != nullptr);
                    const std::string key = child->string;
                    REQUIRE(cJSON_ReplaceItemInObjectCaseSensitive(parent, key.c_str(),
                                                                   hostile(kind)));
                    REQUIRE(cJSON_GetObjectItemCaseSensitive(parent, key.c_str()) !=
                            nullptr);
                    keyed++;
                } else {
                    REQUIRE(cJSON_ReplaceItemInArray(parent, node.path.back(),
                                                     hostile(kind)));
                }
                char *printed = cJSON_PrintUnformatted(copy.get());
                REQUIRE(printed != nullptr);
                const std::string text(printed);
                cJSON_free(printed);
                const Parsed p(text, "hostile.ldtk");
                parsed++;
                if (p.map.valid()) {
                    valid++;
                    // Whatever was read can be asked about.
                    (void)p.map.solid_in(p.map.bounds());
                    (void)p.map.solid_at({ 1e20f, 1e20f });
                    (void)p.map.neighbour_at({ 70, 0 });
                    for (int i = 0; i < p.map.object_count(); i++) {
                        const rmp::MapObject *o =
                            rmp::tilemap::detail::object_at(p.data, i);
                        (void)o->property_point("patrol", 1);
                        (void)o->property_count("patrol");
                    }
                }
            }
        }
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                .count();
        MESSAGE(parsed << " hostile documents (" << keyed << " by key), " << valid
                       << " read, in " << seconds << " s");
        CHECK(keyed > parsed / 2); // most nodes are fields, and they kept their keys
        CHECK(valid > parsed / 2); // most single changes leave a readable level
#if defined(RMP_SANITIZE)
        CHECK(seconds < 20.0); // ASan and UBSan cost several times the time
#else
        CHECK(seconds < 2.0);
#endif
    }

    TEST_CASE("a tile id near INT_MAX is dropped, not read past its tileset") {
        // first_gid + id overflowed for an id near INT_MAX, passed the bounds
        // check, and indexed the tileset's solid flags far out of range.
        const Quiet quiet;
        for (const char *t : { "2147483647", "2147483646", "1e10", "-1", "16" }) {
            CAPTURE(std::string(t));
            const std::string doc =
                std::string(
                    R"({"defs":{"tilesets":[{"uid":1,"__cWid":4,"__cHei":4,"tileGridSize":16,)"
                    R"("enumTags":[{"enumValueId":"Solid","tileIds":[0]}]}]},"levels":[)"
                    R"({"identifier":"A","pxWid":32,"pxHei":32,"layerInstances":[)"
                    R"({"__type":"Tiles","__gridSize":16,"__cWid":2,"__cHei":2,)"
                    R"("__tilesetDefUid":1,"gridTiles":[{"px":[0,0],"t":)") +
                t + R"(}]}]}]})";
            const Parsed p(doc, "big_id.ldtk");
            REQUIRE(p.map.valid());
            REQUIRE(p.map.layer_count() == 1);
            CHECK(p.data->layers[0].placed.empty()); // 16 is past the last tile too
        }
    }

    TEST_CASE("a tile somewhere no float can say is drawn nowhere and gridded nowhere") {
        const Quiet quiet;
        for (const char *px : { "[1e300,0]", "[0,-1e300]", "[3e9,3e9]", "[-1,-1]" }) {
            CAPTURE(std::string(px));
            const std::string doc =
                std::string(
                    R"({"defs":{"tilesets":[{"uid":1,"__cWid":4,"__cHei":4,"tileGridSize":16}]},)"
                    R"("levels":[{"identifier":"A","pxWid":32,"pxHei":32,"layerInstances":[)"
                    R"({"__type":"Tiles","__gridSize":16,"__cWid":2,"__cHei":2,)"
                    R"("__tilesetDefUid":1,"gridTiles":[{"t":1,"px":)") +
                px + R"(}]}]}]})";
            const Parsed p(doc, "far.ldtk");
            REQUIRE(p.map.valid());
            CHECK(p.map.tile_at(0, 0, 0) == 0);
            CHECK(p.map.tile_at(0, 1, 1) == 0);
        }
        // And a map asked about such places answers no.
        const Parsed p(text_of("minimal.ldtk"), "minimal.ldtk");
        CHECK_FALSE(p.map.solid_at({ 1e20f, 1e20f }));
        CHECK_FALSE(p.map.solid_at({ NAN, 8 }));
        CHECK_FALSE(p.map.solid_at({ 8, INFINITY }));
        CHECK_FALSE(p.map.solid_at({ -INFINITY, 8 }));
    }

    TEST_CASE("sizes that would ask for gigabytes are refused, not allocated") {
        const Quiet quiet;
        // 2^20 x 2^20 cells is a terabyte of grid. And a tileset of 2^31 tiles.
        const std::string layer =
            R"({"defs":{"tilesets":[]},"levels":[{"identifier":"A","pxWid":16,"pxHei":16,)"
            R"("layerInstances":[{"__type":"IntGrid","__gridSize":16,"__cWid":1048576,)"
            R"("__cHei":1048576,"intGridCsv":[1]}]}]})";
        const Parsed big(layer, "big.ldtk");
        REQUIRE(big.map.valid());
        CHECK(big.map.layer_count() == 0); // the layer is skipped, the level is not
        const std::string tiles =
            R"({"defs":{"tilesets":[{"uid":1,"__cWid":65536,"__cHei":32768,"tileGridSize":16}]},)"
            R"("levels":[{"identifier":"A","layerInstances":[]}]})";
        const Parsed huge(tiles, "huge.ldtk");
        REQUIRE(huge.map.valid());
        CHECK(huge.data->tilesets.size() == 1);
        CHECK(huge.data->tilesets[0].solid.empty());
    }

    TEST_CASE("the budgets are for the whole file, not for each layer or tileset") {
        // A limit per layer let a 1.5 KB file of a dozen 4096 x 4096 layers take
        // three quarters of a gigabyte; a limit per tileset let 600 tilesets
        // overflow the gid counter into negative numbers.
        const Quiet quiet;
        std::string layers;
        for (int i = 0; i < 6; i++) {
            if (!layers.empty()) layers += ",";
            layers += R"({"__identifier":"L)" + std::to_string(i) +
                R"(","__type":"IntGrid","__gridSize":16,"__cWid":2048,"__cHei":2048})";
        }
        const Parsed many(
            R"({"defs":{},"levels":[{"identifier":"A","pxWid":16,"pxHei":16,)"
            R"("layerInstances":[)" +
                layers + "]}]}",
            "many.ldtk");
        REQUIRE(many.map.valid());
        CHECK(many.map.layer_count() == 4); // 4 x 2048^2 is the whole budget, exactly

        std::string sets;
        for (int i = 0; i < 600; i++) {
            if (!sets.empty()) sets += ",";
            sets += R"({"uid":)" + std::to_string(i + 1) +
                R"(,"__cWid":2048,"__cHei":2048,"tileGridSize":16})";
        }
        const Parsed tilesets(
            R"({"defs":{"tilesets":[)" + sets +
                R"(]},"levels":[{"identifier":"A","layerInstances":[]}]})",
            "tilesets.ldtk");
        REQUIRE(tilesets.map.valid());
        REQUIRE(tilesets.data->tilesets.size() == 600);
        std::size_t flags = 0;
        for (const auto &set : tilesets.data->tilesets) {
            CHECK(set.first_gid >= 1); // never wrapped round into the negatives
            CHECK(set.last_gid >= set.first_gid - 1);
            flags += set.solid.size();
        }
        CHECK(flags == 4194304); // one tileset's worth: the budget
    }

    TEST_CASE("a level file on its own is refused with what to do instead") {
        const Quiet quiet;
        const Parsed p(text_of("World_Level_0.ldtkl"), "World_Level_0.ldtkl");
        CHECK_FALSE(p.map.valid());
    }

    // -----------------------------------------------------------------------
    // What LDtk 1.5.3 itself saved
    // -----------------------------------------------------------------------

    TEST_CASE(
        "every tile and every entity of every sample agrees with LDtk's own numbers") {
        // TILES. LDtk writes `src` -- the tile's corner in the tileset image
        // -- next to `t`, the id this reader numbers from. If tile ids,
        // columns, padding or spacing were read wrong anywhere, some tile
        // somewhere would come from a different rectangle than the editor drew
        // it from. Every tile, every layer, every level, every sample: and in
        // order, bottom layer first and each layer's tiles as the file has
        // them.
        //
        // ENTITIES. In GridVania and Free worlds LDtk writes __worldX and
        // __worldY: where the entity's PIVOT is in the world, by its own
        // arithmetic. The centre is that plus (0.5 - pivot) of the size, so
        // the level's origin, the layer's offset and the pivot all have to be
        // right for every entity to agree.
        //
        // One test and one parse per level for both, because the samples are
        // 850 KB of JSON and `rmp test` runs this twice.
        const Quiet quiet;
        int tiles = 0;
        int entities = 0;
        for (const char *sample : SAMPLES) {
            CAPTURE(std::string(sample));
            const std::string text = text_of(sample);
            const Json root(cJSON_Parse(text.c_str()));
            REQUIRE(root != nullptr);
            const std::string layout = str(root.get(), "worldLayout");
            const bool world = layout == "GridVania" || layout == "Free";
            const cJSON *level = nullptr;
            cJSON_ArrayForEach(level,
                               cJSON_GetObjectItemCaseSensitive(root.get(), "levels")) {
                const std::string name = str(level, "identifier");
                CAPTURE(name);
                const Parsed p(text, sample, name.c_str());
                REQUIRE(p.map.valid());
                const cJSON *layers =
                    cJSON_GetObjectItemCaseSensitive(level, "layerInstances");
                std::size_t index = 0;
                for (int i = cJSON_GetArraySize(layers) - 1; i >= 0; i--) {
                    const cJSON *li = cJSON_GetArrayItem(layers, i);
                    if (std::string(str(li, "__type")) == "Entities") {
                        const cJSON *e = nullptr;
                        cJSON_ArrayForEach(
                            e, cJSON_GetObjectItemCaseSensitive(li, "entityInstances")) {
                            const rmp::MapObject *o =
                                object_by_iid(p.data, str(e, "iid"));
                            REQUIRE(o != nullptr);
                            CHECK(std::string(o->type) == str(e, "__identifier"));
                            if (!world) continue;
                            const double w = num(e, "width");
                            const double h = num(e, "height");
                            CHECK(o->position.x ==
                                  doctest::Approx(num(e, "__worldX") +
                                                  (0.5 - pair(e, "__pivot", 0)) * w));
                            CHECK(o->position.y ==
                                  doctest::Approx(num(e, "__worldY") +
                                                  (0.5 - pair(e, "__pivot", 1)) * h));
                            entities++;
                        }
                        continue;
                    }
                    REQUIRE(index < p.data->layers.size());
                    const Layer &layer = p.data->layers[index++];
                    CHECK(layer.name == str(li, "__identifier"));
                    std::size_t k = 0;
                    for (const char *key : { "gridTiles", "autoLayerTiles" }) {
                        const cJSON *t = nullptr;
                        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(li, key)) {
                            REQUIRE(k < layer.placed.size());
                            const PlacedTile &placed = layer.placed[k++];
                            const Rectangle src =
                                rmp::tilemap::detail::tile_source(p.data, placed.gid);
                            CHECK(src.x == doctest::Approx(pair(t, "src", 0)));
                            CHECK(src.y == doctest::Approx(pair(t, "src", 1)));
                            CHECK(placed.at.x == doctest::Approx(pair(t, "px", 0)));
                            CHECK(placed.at.y == doctest::Approx(pair(t, "px", 1)));
                            const int f = static_cast<int>(num(t, "f"));
                            CHECK(placed.flip_x == ((f & 1) != 0));
                            CHECK(placed.flip_y == ((f & 2) != 0));
                            CHECK(placed.alpha == doctest::Approx(num(t, "a")));
                            tiles++;
                        }
                    }
                    CHECK(k == layer.placed.size());
                }
                CHECK(index == p.data->layers.size());
            }
        }
        MESSAGE(tiles << " tiles and " << entities << " entities checked against LDtk");
        CHECK(tiles > 3000);
        CHECK(entities > 20);
    }

    TEST_CASE("the all-features sample: fields as LDtk saved them") {
        const Quiet quiet;
        const Parsed p(text_of("Test_file_for_API_showing_all_features.ldtk"),
                       "Test_file_for_API_showing_all_features.ldtk");
        REQUIRE(p.map.valid());
        CHECK(std::string(p.map.level()) == "Everything");
        const rmp::MapObject *f = object_of_type(p.data, "EntityFieldsTest");
        REQUIRE(f != nullptr);
        CHECK(f->position.x == doctest::Approx(152)); // pivot 0.5, 0.5
        CHECK(f->position.y == doctest::Approx(312));
        CHECK(f->property_int("Integer") == 1);
        CHECK(f->property_float("Float") == doctest::Approx(1.5));
        CHECK_FALSE(f->property_bool("Boolean", true));
        CHECK(std::string(f->property_string("String_singleLine")) == "foo");
        CHECK(std::string(f->property_string("String_multiLines")) == "foo bar");
        CHECK(std::string(f->property_string("Enum")) == "A");
        CHECK(std::string(f->property_string("ExternEnum")) == "Value1");
        CHECK(std::string(f->property_string("Color")) == "#D181E8");
        const Vector2 point = f->property_point("Point");
        CHECK(point.x == doctest::Approx(72)); // cell (4, 17) of a 16 px grid
        CHECK(point.y == doctest::Approx(280));
        REQUIRE(f->property_count("Array_points") == 4);
        CHECK(f->property_point("Array_points", 0).x == doctest::Approx(232));
        CHECK(f->property_point("Array_points", 0).y == doctest::Approx(312));
        CHECK(f->property_point("Array_points", 2).x == doctest::Approx(296));
        CHECK(f->property_point("Array_points", 3).y == doctest::Approx(312));
        CHECK(f->property_count("Array_Integer") == 0); // not read yet
        CHECK(f->property_count("FilePath") == 0);

        // Three EntityRefTest entities, each pointing at the next, in a ring.
        const rmp::MapObject *a =
            object_by_iid(p.data, "d08b1280-66b0-11ec-895f-aff95798ac90");
        REQUIRE(a != nullptr);
        const rmp::MapObject *b = object_by_iid(p.data, a->property_string("target"));
        REQUIRE(b != nullptr);
        const rmp::MapObject *c = object_by_iid(p.data, b->property_string("target"));
        REQUIRE(c != nullptr);
        CHECK(std::string(c->property_string("target")) == a->iid);

        // Pivot 0, 0.5: px is the middle of the left edge.
        const rmp::MapObject *label =
            object_by_iid(p.data, "566aec40-66b0-11ec-895f-d12a68bdf6fd");
        REQUIRE(label != nullptr);
        CHECK(label->position.x == doctest::Approx(352));
        CHECK(label->position.y == doctest::Approx(88));
    }

    TEST_CASE("the all-features sample: layers, grids, and no value named solid") {
        const Quiet quiet;
        const std::string text = text_of("Test_file_for_API_showing_all_features.ldtk");
        const Parsed p(text, "Test_file_for_API_showing_all_features.ldtk");
        REQUIRE(p.map.valid());
        const char *expected[] = { "Tiles", "IntGrid_with_rules", "IntGrid_without_rules",
                                   "PureAutoLayer", "IntGrid_8px_grid" };
        REQUIRE(p.data->layers.size() == 5);
        for (std::size_t i = 0; i < 5; i++) CHECK(p.data->layers[i].name == expected[i]);
        CHECK(p.data->layers[4].cell_width == 8);
        CHECK(p.data->layers[4].width == 85);
        // Cell (13, 7) of IntGrid_with_rules stacks tile 140 and then 105:
        // the top one is 105, gid 106 in the first tileset.
        CHECK(p.map.tile_at(1, 13, 7) == 106);
        // Its values are "walls", "first", "second"... and none is "solid",
        // so this map is solid nowhere. Which is the point: the reader does
        // not guess that an IntGrid is a wall.
        CHECK_FALSE(p.map.solid_in(p.map.bounds()));
        // A linear layout: no world, so no neighbours, and every level at 0,0.
        CHECK(std::string(p.map.neighbour_at({ 10, 600 })).empty());
        const Parsed other(text, "Test_file_for_API_showing_all_features.ldtk",
                           "Tiles_and_intgrid");
        REQUIRE(other.map.valid());
        CHECK(other.map.bounds().x == doctest::Approx(0));
        CHECK(other.map.bounds().width == doctest::Approx(256));
    }

    TEST_CASE("a Free world: levels where they are, above zero too, and neighbours") {
        const Quiet quiet;
        const std::string text = text_of("Typical_2D_platformer_example.ldtk");
        const Parsed first(text, "Typical_2D_platformer_example.ldtk");
        REQUIRE(first.map.valid());
        CHECK(std::string(first.map.level()) == "Your_typical_2D_platformer");
        // "Top" sits at (352, -352), 672 x 352: above the first level.
        CHECK(std::string(first.map.neighbour_at({ 400, -10 })) == "Top");
        CHECK(std::string(first.map.neighbour_at({ 900, 10 })) == "World_Level_3");
        CHECK(std::string(first.map.neighbour_at({ 100, 100 })).empty()); // its own

        const Parsed top(text, "Typical_2D_platformer_example.ldtk", "Top");
        REQUIRE(top.map.valid());
        const Rectangle b = top.map.bounds();
        CHECK(b.x == doctest::Approx(352));
        CHECK(b.y == doctest::Approx(-352));
        CHECK(b.width == doctest::Approx(672));
        // The first Mob patrols to cell (22, 14): the world, not the level.
        const rmp::MapObject *mob = object_of_type(top.data, "Mob");
        REQUIRE(mob != nullptr);
        REQUIRE(mob->property_count("patrol") == 1);
        CHECK(mob->property_point("patrol", 0).x == doctest::Approx(712));
        CHECK(mob->property_point("patrol", 0).y == doctest::Approx(-120));
        CHECK(layer_named(top.data, "Wall_shadows")->opacity == doctest::Approx(0.17));
        const rmp::MapObject *door = object_of_type(top.data, "Door", 1);
        REQUIRE(door != nullptr);
        CHECK(door->property_bool("locked"));
    }

    TEST_CASE("a GridVania world: a layer's offset and tiles with alpha") {
        const Quiet quiet;
        const Parsed p(text_of("Typical_TopDown_example.ldtk"),
                       "Typical_TopDown_example.ldtk");
        REQUIRE(p.map.valid());
        CHECK(p.map.bounds().x == doctest::Approx(256));
        const Layer *tops = layer_named(p.data, "Wall_tops");
        REQUIRE(tops != nullptr);
        CHECK(tops->offset.y == doctest::Approx(-16));
        const Layer *walls = layer_named(p.data, "Collisions");
        REQUIRE(walls != nullptr);
        bool faded = false;
        for (const PlacedTile &t : walls->placed)
            faded = faded || std::fabs(t.alpha - 0.3f) < 1e-4f;
        CHECK(faded);
        CHECK(std::string(p.map.neighbour_at({ 100, 100 })) == "World_Level_1");
        CHECK(std::string(p.map.neighbour_at({ 800, 100 })) == "World_Level_2");
        const rmp::MapObject *player = object_of_type(p.data, "Player");
        REQUIRE(player != nullptr);
        CHECK(player->property_int("life") == 100);
        // An enum left empty in the editor is null: the fallback.
        const rmp::MapObject *door = object_of_type(p.data, "Door");
        REQUIRE(door != nullptr);
        CHECK(std::string(door->property_string("lockedWith", "none")) == "none");
    }

    TEST_CASE("levels saved to their own files are fetched by name") {
        const Quiet quiet;
        const std::string text = text_of("SeparateLevelFiles.ldtk");
        {
            const AtLdtk at;
            const Parsed p(text, "SeparateLevelFiles.ldtk", "World_Level_1");
            REQUIRE(p.map.valid());
            CHECK(std::string(p.map.level()) == "World_Level_1");
            REQUIRE(p.map.layer_count() == 1);
            CHECK(p.data->layers[0].name == "IntGrid");
            CHECK(p.data->layers[0].width == 15);
        }
        // From somewhere the .ldtkl is not: no map, and a failed load the CI
        // boot gate counts.
        const int failed = rmp::assets::failed_loads();
        const Parsed missing(text, "SeparateLevelFiles.ldtk", "World_Level_1");
        CHECK_FALSE(missing.map.valid());
        CHECK(rmp::assets::failed_loads() > failed);
    }

    TEST_CASE("the platformer's own world can still be played through") {
        // examples/games/07_platformer/resources/world.ldtk is edited in LDtk
        // from now on, by hand. What the game needs from it is checked here,
        // so that a redesign that breaks the game breaks `rmp test` first:
        // every level loads, they chain from the first one rightwards through
        // neighbours, there is one Player and one Goal, every key opens a door
        // that exists in its level, and every route stays inside its level.
        const Quiet quiet;
        const std::string file = std::string(RMP_TEST_FIXTURES) +
            "../../examples/games/07_platformer/resources/world.ldtk";
        std::ifstream in(file, std::ios::binary);
        REQUIRE_MESSAGE(in.good(), "missing " << file);
        const std::string text{ std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>() };

        int players = 0;
        int goals = 0;
        int keys = 0;
        std::string level;
        Parsed first(text, "world.ldtk");
        REQUIRE(first.map.valid());
        level = first.map.level();
        std::vector<std::string> visited;
        while (!level.empty() && visited.size() < 32) {
            CAPTURE(level);
            visited.push_back(level);
            const Parsed p(text, "world.ldtk", level.c_str());
            REQUIRE(p.map.valid());
            const Rectangle b = p.map.bounds();
            CHECK(p.map.layer_count() > 0);
            CHECK(p.map.solid_in(b)); // there is ground to stand on
            for (int i = 0; i < rmp::tilemap::detail::object_count(p.data); i++) {
                const rmp::MapObject *o = rmp::tilemap::detail::object_at(p.data, i);
                const std::string type = o->type;
                CAPTURE(type);
                if (type == "Player") players++;
                if (type == "Goal") goals++;
                if (type == "Key") {
                    keys++;
                    const rmp::MapObject *door =
                        object_by_iid(p.data, o->property_string("opens"));
                    REQUIRE_MESSAGE(door != nullptr,
                                    "a key that opens no door in its level");
                    CHECK(std::string(door->type) == "Door");
                }
                for (const char *route : { "patrol", "to" }) {
                    for (int k = 0; k < o->property_count(route); k++) {
                        const Vector2 at = o->property_point(route, k);
                        CHECK_MESSAGE(CheckCollisionPointRec(at, b),
                                      route << " leaves the level");
                    }
                }
            }
            // The next level is the one past the right edge, at the height of
            // the ground there.
            std::string next;
            for (int step = 0; static_cast<float>(step * 9) < b.height && next.empty();
                 step++) {
                next = p.map.neighbour_at(
                    { b.x + b.width + 1, b.y + static_cast<float>(step * 9) });
            }
            level = next;
            CHECK(std::ranges::find(visited, level) == visited.end());
        }
        CHECK(visited.size() >= 2);
        CHECK(players == 1);
        CHECK(goals == 1);
        CHECK(keys >= 1);
        MESSAGE(visited.size() << " levels, left to right");
    }

    TEST_CASE("load_map picks the reader by the extension") {
        const Quiet quiet;
        // The same LDtk text named .json goes to the Tiled reader, which
        // skips every key it does not know and finds no level in it; named
        // .ldtk, it is the level.
        const std::string text = text_of("minimal.ldtk");
        const Parsed as_tiled(text, "minimal.json");
        CHECK(std::string(as_tiled.map.level()).empty());
        CHECK(as_tiled.map.object_count() == 0);
        const Parsed as_ldtk(text, "minimal.ldtk");
        CHECK(std::string(as_ldtk.map.level()) == "Start");
        CHECK(as_ldtk.map.object_count() == 3);
        CHECK(Parsed(text, "dir/minimal.ldtk").map.valid());
    }
}
