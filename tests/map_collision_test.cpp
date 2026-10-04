// ===========================================================================
// The map is solid: a solid object that moves is stopped by its solid cells.
//
// rmp/tilemap.h promised it from the day the map arrived ("collides against
// its solid tiles"), and nothing did it: the collision pass knew objects and
// nothing else, so a player dropped into a level fell through the floor. It
// went unnoticed because no game used a map until the platformer of phase
// 12B. These tests are what would have said so.
//
// The maps are LDtk projects built inline from a picture -- '#' is a solid
// cell, '.' is empty, 'w' is an IntGrid value that is not solid -- so each
// test shows the level it runs in.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/object_internal.h"
#include "../src/rmp/tilemap_internal.h"

#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/tilemap.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

namespace {

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

// The fake device the input seam reads. The dot says "file state".
struct {
    rmp::input::detail::DeviceState devices;
} fake;
void fake_sample(rmp::input::detail::DeviceState *out) { *out = fake.devices; }

struct Fixture {
    Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
        rmp::input::detail::reset();
        fake.devices = rmp::input::detail::DeviceState{};
        rmp::input::detail::set_sample_provider(fake_sample);
    }
    ~Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
        rmp::input::detail::reset();
        fake.devices = rmp::input::detail::DeviceState{};
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
};

constexpr int CELL = 16;
constexpr float DELTA = 1.0f / 60;

// A one-level LDtk project of `rows`, 16 px cells, its level at `origin`.
std::string project_of(const std::vector<std::string> &rows, Vector2 origin = {}) {
    const int h = static_cast<int>(rows.size());
    const int w = static_cast<int>(rows[0].size());
    std::string csv;
    for (const std::string &row : rows) {
        for (char c : row) {
            if (!csv.empty()) csv += ',';
            csv += c == '#' ? "1" : (c == 'w' ? "2" : "0");
        }
    }
    const std::string cells = std::to_string(CELL);
    return R"({"defs":{"layers":[{"uid":1,"identifier":"Collisions","type":"IntGrid",)"
           R"("gridSize":16,"intGridValues":[{"value":1,"identifier":"Solid"},)"
           R"({"value":2,"identifier":"water"}]}],"tilesets":[]},)"
           R"("worldLayout":"Free","levels":[{"identifier":"Level","worldX":)" +
        std::to_string(static_cast<int>(origin.x)) + R"(,"worldY":)" +
        std::to_string(static_cast<int>(origin.y)) + R"(,"pxWid":)" +
        std::to_string(w * CELL) + R"(,"pxHei":)" + std::to_string(h * CELL) +
        R"(,"layerInstances":[{"__identifier":"Collisions","__type":"IntGrid",)"
        R"("__gridSize":)" +
        cells + R"(,"__cWid":)" + std::to_string(w) + R"(,"__cHei":)" +
        std::to_string(h) + R"(,"layerDefUid":1,"intGridCsv":[)" + csv + "]}]}]}";
}

void load(World &world, const std::vector<std::string> &rows, Vector2 origin = {}) {
    const std::string text = project_of(rows, origin);
    world.map.adopt(rmp::tilemap::detail::parse_map(
        text.data(), static_cast<int>(text.size()), "test.ldtk"));
    REQUIRE(world.map.valid());
}

void frame(rmp::Scene &scene, float delta = DELTA) {
    rmp::input::detail::begin_frame();
    rmp::objects::detail::update(scene, delta);
    rmp::objects::detail::collide(scene);
    rmp::objects::detail::collect();
}

// A solid, falling box: 10 wide, 20 tall, gravity on.
rmp::Object &faller(World &world, Vector2 at) {
    rmp::Object &o = world.spawn({ .position = at, .shape = rmp::rect({ 10, 20 }) });
    o.solid = true;
    o.gravity_scale = 1;
    return o;
}

float bottom_of(const rmp::Object &o) {
    const Rectangle b = o.world_collider();
    return b.y + b.height;
}

std::vector<std::string> room() {
    return {
        // 0123456789
        "##########", // 0
        "#........#", // 1
        "#........#", // 2
        "#........#", // 3
        "#........#", // 4
        "#........#", // 5
        "#........#", // 6
        "##########", // 7: the floor's top is at y = 112
    };
}

} // namespace

TEST_SUITE("map collision") {
    TEST_CASE("touching a solid cell is not being in it") {
        World world;
        load(world, room());
        // Flush on the floor, flush against the left wall, flush against the
        // right one: not in.
        CHECK_FALSE(world.map.solid_in({ 16, 92, 10, 20 }));
        CHECK_FALSE(world.map.solid_in({ 134, 92, 10, 20 }));
        CHECK(world.map.solid_in({ 134.01f, 92, 10, 20 }));
        // A hundredth of a pixel into either: in.
        CHECK(world.map.solid_in({ 16, 92.01f, 10, 20 }));
        CHECK(world.map.solid_in({ 15.99f, 92, 10, 20 }));
        // A rectangle with no area is the cell its corner is in.
        CHECK(world.map.solid_in({ 16, 112, 0, 0 }));
        CHECK_FALSE(world.map.solid_in({ 16, 111.9f, 0, 0 }));
        // Absurd rectangles are answered, not looped over.
        CHECK(world.map.solid_in({ -1e9f, -1e9f, 2e9f, 2e9f }));
        CHECK_FALSE(world.map.solid_in({ NAN, 0, 10, 10 }));
        CHECK_FALSE(world.map.solid_in({ 0, 0, INFINITY, 10 }));
        CHECK_FALSE(world.map.solid_in({ 32, 32, -5, 10 }));
    }

    TEST_CASE_FIXTURE(Fixture, "a solid object falls onto the floor and stays there") {
        World world;
        load(world, room());
        rmp::Object &box = faller(world, { 80, 40 });
        for (int i = 0; i < 120; i++) frame(world);
        CHECK(bottom_of(box) == doctest::Approx(112).epsilon(0.0001));
        CHECK(bottom_of(box) <= 112.0f); // on it, never in it
        CHECK(box.velocity.y == doctest::Approx(0));
        CHECK(box.position.x == doctest::Approx(80)); // and nothing sideways
    }

    TEST_CASE_FIXTURE(Fixture, "faster than a cell per frame, it still lands") {
        // 3000 px/s at 60 Hz is 50 px a frame: three cells. A floor one cell
        // thick is crossed in a single step unless the move is divided.
        World world;
        load(world,
             { "..........", "..........", "..........", "..........", "..........",
               "..........", "##########", ".........." });
        rmp::Object &box = faller(world, { 80, 10 });
        box.gravity_scale = 0;
        box.velocity = { 0, 3000 };
        for (int i = 0; i < 10; i++) frame(world);
        CHECK(bottom_of(box) == doctest::Approx(96).epsilon(0.0001));
        CHECK(box.velocity.y == doctest::Approx(0));
    }

    TEST_CASE_FIXTURE(Fixture, "running along a floor of tiles never catches on a seam") {
        // The floor is ten separate cells. Pushed into it by gravity every
        // frame and running at 300 px/s, the box must cross every seam at
        // full speed. A per-cell push-out stops it dead at the first one,
        // where the shortest way out of the next cell is sideways.
        World world;
        load(world, room());
        rmp::Object &box = faller(world, { 24, 101.99f });
        for (int i = 0; i < 10; i++) frame(world); // settle
        box.velocity.x = 300;
        const float start = box.position.x;
        for (int i = 0; i < 20; i++) {
            box.velocity.x = 300;
            frame(world);
            CHECK(box.velocity.x == doctest::Approx(300));
        }
        CHECK(box.position.x ==
              doctest::Approx(start + (300 * 20 * DELTA)).epsilon(0.001));
        CHECK(bottom_of(box) == doctest::Approx(112).epsilon(0.0001));
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a wall stops the axis that runs into it, and only that one") {
        World world;
        load(world, room());
        rmp::Object &box = faller(world, { 120, 40 });
        box.gravity_scale = 0;
        box.velocity = { 600, 60 };
        for (int i = 0; i < 10; i++) frame(world);
        // The right wall's left face is at x = 144; the box is 10 wide.
        CHECK(box.position.x == doctest::Approx(139).epsilon(0.0001));
        CHECK(box.velocity.x == doctest::Approx(0));
        CHECK(box.velocity.y == doctest::Approx(60)); // still sliding down
        CHECK(box.position.y == doctest::Approx(40 + (60 * 10 * DELTA)).epsilon(0.001));
    }

    TEST_CASE_FIXTURE(Fixture, "the ceiling stops a jump") {
        World world;
        load(world, room());
        rmp::Object &box = faller(world, { 80, 60 });
        box.gravity_scale = 0;
        box.velocity = { 0, -900 };
        for (int i = 0; i < 10; i++) frame(world);
        // The ceiling's underside is at y = 16; the box is 20 tall.
        CHECK(box.position.y == doctest::Approx(26).epsilon(0.0001));
        CHECK(box.velocity.y == doctest::Approx(0));
    }

    TEST_CASE_FIXTURE(Fixture,
                      "only solid, movable objects: a coin and a moved platform pass") {
        World world;
        load(world, room());
        rmp::Object &coin =
            world.spawn({ .position = { 80, 40 }, .shape = rmp::rect({ 8, 8 }) });
        coin.gravity_scale = 1; // not solid: falls through, like a trigger would
        rmp::Object &platform =
            world.spawn({ .position = { 80, 40 }, .shape = rmp::rect({ 32, 8 }) });
        platform.solid = true;
        platform.immovable = true; // the game moves it; the map does not stop it
        platform.velocity = { 0, 600 };
        for (int i = 0; i < 60; i++) frame(world);
        CHECK(coin.position.y > 128);
        CHECK(platform.position.y > 128);
    }

    TEST_CASE_FIXTURE(Fixture, "an IntGrid value that is not solid is not a floor") {
        World world;
        load(world,
             { "..........", "..........", "..........", "..........", "..........",
               "..........", "wwwwwwwwww", "##########" });
        rmp::Object &box = faller(world, { 80, 40 });
        for (int i = 0; i < 120; i++) frame(world);
        CHECK(bottom_of(box) ==
              doctest::Approx(112).epsilon(0.0001)); // through the water
    }

    TEST_CASE_FIXTURE(Fixture,
                      "pushed into the floor, it comes back out the shortest way") {
        // The collision pass separates objects without looking at the map,
        // so one can end a frame inside a cell. Left there, both axes would
        // be blocked from the first step and it would never move again.
        World world;
        load(world, room());
        rmp::Object &box = faller(world, { 80, 107 }); // 5 px into the floor
        box.gravity_scale = 0;
        frame(world);
        CHECK(bottom_of(box) == doctest::Approx(112).epsilon(0.0001));
        CHECK(box.position.x == doctest::Approx(80)); // up, not sideways
        box.velocity.x = 120;
        frame(world);
        CHECK(box.position.x == doctest::Approx(82).epsilon(0.001)); // and free to go
    }

    TEST_CASE_FIXTURE(Fixture,
                      "out of a corner it goes UP when up is as short as sideways") {
        // A box 3 px into the top-left corner of a lone block: 3 px up or
        // 3 px left would both free it. Up, because the commonest way into a
        // cell is being pushed down into the floor.
        World world;
        load(world,
             { "..........", "..........", "..........", "..........", "....#.....",
               "..........", "..........", ".........." });
        rmp::Object &box =
            world.spawn({ .position = { 62, 62 }, .shape = rmp::rect({ 10, 10 }) });
        box.solid = true;
        frame(world);
        CHECK(box.position.x == doctest::Approx(62));
        CHECK(box.position.y == doctest::Approx(59).epsilon(0.0001));
    }

    TEST_CASE_FIXTURE(Fixture, "a jump that reaches a ledge's corner lands on it") {
        // Across first, then down: moving 4 px right and 4 px down onto the
        // top-left corner of a ledge, the across part is still above the
        // ledge and the down part lands on it. The other order goes down
        // first, alongside the ledge, and then hits its side -- the jump that
        // visibly made it falls off.
        World world;
        load(world,
             { "..........", "..........", "..........", "..........", "..........",
               "..........", ".....#####", ".........." });
        rmp::Object &box =
            world.spawn({ .position = { 73, 85 }, .shape = rmp::rect({ 10, 20 }) });
        box.solid = true;
        box.velocity = { 240, 240 }; // 4 px a frame each way
        frame(world);
        CHECK(bottom_of(box) == doctest::Approx(96).epsilon(0.0001));
        CHECK(box.position.x == doctest::Approx(77));
        CHECK(box.velocity.y == doctest::Approx(0));
        CHECK(box.velocity.x == doctest::Approx(240));
    }

    TEST_CASE_FIXTURE(Fixture,
                      "pushed into a thin wall by something moving, it stays this side") {
        // A platform the game moves by hand (immovable, solid) shoves the box
        // into a wall one cell thick. Separation does not look at the map, and
        // once the box was more than halfway in, "the shortest way out" was
        // the far side: it came out through the wall. The map outranks a
        // push now, and the box stays against the near face.
        World world;
        load(world,
             { "..........", "..........", "..........", "..........", "..........",
               "..........", ".....#....", "##########" });
        rmp::Object &box =
            world.spawn({ .position = { 70, 102 }, .shape = rmp::rect({ 10, 20 }) });
        box.solid = true;
        rmp::Object &pusher =
            world.spawn({ .position = { 40, 102 }, .shape = rmp::rect({ 30, 20 }) });
        pusher.solid = true;
        pusher.immovable = true;
        pusher.velocity = { 180, 0 }; // 3 px a frame, into the box and the wall
        float furthest = 0;
        for (int i = 0; i < 40; i++) {
            frame(world);
            furthest = std::max(furthest, box.position.x);
        }
        // The wall's near face is x = 80; the box is 10 wide.
        CHECK(furthest <= 75.01f);
        CHECK_FALSE(world.map.solid_in(box.world_collider()));
    }

    TEST_CASE_FIXTURE(Fixture, "buried with no way out, it stays where it is") {
        // Deeper in solid cells than a cell plus its own size, in every
        // direction: there is no right answer. It used to move freely, and
        // gravity took it down through every row of the map.
        World world;
        load(world,
             { "##########", "##########", "##########", "##########", "##########",
               "##########", "##########", "##########" });
        rmp::Object &box = faller(world, { 80, 64 });
        for (int i = 0; i < 120; i++) frame(world);
        CHECK(box.position.x == doctest::Approx(80));
        CHECK(box.position.y == doctest::Approx(64));
        CHECK(box.velocity.y == doctest::Approx(0));
    }

    TEST_CASE_FIXTURE(Fixture,
                      "an absurd velocity costs a bounded frame and nothing undefined") {
        // 1e13 px/s is a step count past INT_MAX: the cast was undefined, and
        // a merely huge one was a million solid_in() calls in one frame.
        World world;
        load(world, room());
        rmp::Object &box = faller(world, { 80, 40 });
        box.gravity_scale = 0;
        box.velocity = { 1e13f, 3e8f };
        const auto start = std::chrono::steady_clock::now();
        frame(world);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
        CHECK(std::isfinite(box.position.x));
        CHECK(std::isfinite(box.position.y));
        CHECK(ms < 200);
    }

    TEST_CASE_FIXTURE(Fixture, "the map is where the level is in the world") {
        World world;
        load(world, room(), { 1000, -500 });
        rmp::Object &box = faller(world, { 1080, -460 });
        for (int i = 0; i < 120; i++) frame(world);
        CHECK(bottom_of(box) == doctest::Approx(-388).epsilon(0.0001)); // -500 + 112
    }

    TEST_CASE_FIXTURE(
        Fixture, "Platformer stands on the map, jumps from it, and even off a ledge") {
        World world;
        load(world,
             { "..........", "..........", "..........", "..........", "..........",
               "..........", "....######", ".........." });
        rmp::Object &player =
            world.spawn({ .position = { 120, 60 }, .shape = rmp::rect({ 10, 20 }) });
        auto &platformer =
            player.add<rmp::behavior::Platformer>({ .gravity = 1500, .jump = 400 });
        for (int i = 0; i < 90; i++) frame(world);
        CHECK(bottom_of(player) == doctest::Approx(96).epsilon(0.0001));
        CHECK(platformer.on_ground());
        CHECK(player.velocity.y == doctest::Approx(0));

        fake.devices.keys[KEY_SPACE] = true; // ui_accept
        frame(world);
        fake.devices.keys[KEY_SPACE] = false;
        CHECK_FALSE(platformer.on_ground());
        for (int i = 0; i < 4; i++) frame(world);
        CHECK(bottom_of(player) < 90); // in the air
        for (int i = 0; i < 90; i++) frame(world);
        CHECK(platformer.on_ground()); // and back down

        // Half over the ledge at x = 64: the centre is over the gap, the
        // right half over the floor. The map holds it up, so it is ground --
        // or it would be held up and unable to jump.
        player.position.x = 62;
        frame(world);
        CHECK(bottom_of(player) == doctest::Approx(96).epsilon(0.0001));
        CHECK(platformer.on_ground());
        // Past the ledge, it falls.
        player.position.x = 50;
        for (int i = 0; i < 30; i++) frame(world);
        CHECK(bottom_of(player) > 100);
        CHECK_FALSE(platformer.on_ground());
    }
}
