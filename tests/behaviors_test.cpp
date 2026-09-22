// ===========================================================================
// The three behaviors the catalogue had no tests for at all.
//
// tests/behavior_test.cpp covers the ENGINE -- attaching, the hook order, the
// life cycle -- and the behaviors whose arithmetic needs nothing but a stack
// object. This file is the other half: Platformer, Parallax and TopDown's
// sprite rule, all three of which need something the engine tests did not set
// up. Platformer needs a scene, because the ground is a raycast; TopDown's tag
// rule needs a sheet with tags in it; Parallax needs the pure half of its
// _draw split out, because the drawing half is a GL context.
//
// None of them needs a window, and the reason is the same one that makes every
// other test here possible: input comes through a seam, time is a number the
// caller passes, and randomness is seeded. The devices below are a struct this
// file writes by hand.
//
// Platformer shipped with no test file section at all, and what got through was
// CORE-10: a coin lying on the floor turned the ground off, because the ground
// check took the NEAREST hit and only then asked whether it was solid.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/animation_internal.h"
#include "../src/rmp/behavior_internal.h"
#include "../src/rmp/internal.h"
#include "../src/rmp/object_internal.h"

#include <rmp/assets.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

// The device state the fake provider hands over, exactly as tests/input_test.cpp
// does it: there is no raylib under any of this and no device attached to the
// machine running it.
rmp::input::detail::DeviceState g_devices;

void fake_sample(rmp::input::detail::DeviceState *out) { *out = g_devices; }

struct Fixture {
    Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
        rmp::objects::detail::reset_pointer_for_tests();
        rmp::input::detail::reset();
        g_devices = rmp::input::detail::DeviceState{};
        rmp::input::detail::set_sample_provider(fake_sample);
        rmp::detail::reset_reports_for_tests();
    }
    ~Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
        // reset() puts the raylib provider back, so nothing leaks a pointer
        // into this translation unit after the case ends.
        rmp::input::detail::reset();
        g_devices = rmp::input::detail::DeviceState{};
        rmp::detail::release_all();
    }
};

void hold(::KeyboardKey key, bool down = true) { g_devices.keys[key] = down; }

// One turn of the loop for ONE object's behaviors. No integration, so the test
// keeps control of where the object is -- which is what lets a ground check be
// tested at an exact distance from the floor.
void tick(rmp::Object &object, float delta) {
    rmp::input::detail::begin_frame();
    rmp::objects::detail::update_behaviors(object, delta);
}

// A whole frame of a scene, the way the scene stack drives it.
void frame(rmp::Scene &scene, float delta) {
    rmp::input::detail::begin_frame();
    rmp::objects::detail::update(scene, delta);
    rmp::objects::detail::collide(scene);
    rmp::objects::detail::collect();
}

// A floor 400 wide centred at y = 100, so its TOP EDGE is at y = 90. A 20-tall
// object centred at y = 80 therefore has its feet exactly on it.
rmp::Object &solid_floor(World &world) {
    rmp::Object &floor =
        world.spawn({ .position = { 0, 100 }, .shape = rmp::rect({ 400, 20 }) });
    floor.solid = true;
    floor.immovable = true;
    return floor;
}

rmp::Object &stander(World &world) {
    return world.spawn({ .position = { 0, 80 }, .shape = rmp::rect({ 10, 20 }) });
}

// A sheet with the given tags and nothing else: one 1x1 frame, no texture, no
// file. The resource slot owns the tables from the moment it adopts them and
// frees them with delete[], which is what the new[] here is for.
rmp::SpriteSheet sheet_with(const std::vector<const char *> &names) {
    rmp::SheetData data{};
    data.width = 16;
    data.height = 16;
    data.frames.resize(1);
    data.frames[0].seconds = 0.1f;
    data.tags.resize(names.size());
    for (std::size_t i = 0; i < names.size(); i++) {
        std::snprintf(data.tags[i].name, sizeof(data.tags[i].name), "%s", names[i]);
    }

    auto *slot = rmp::detail::adopt(rmp::detail::ResourceKind::SHEET, std::move(data));
    REQUIRE(slot != nullptr);
    return rmp::SpriteSheet{ slot };
}

std::string playing(const rmp::Object &object) {
    const char *tag = object.sprite.playing();
    return tag == nullptr ? std::string() : std::string(tag);
}

} // namespace

// ---------------------------------------------------------------------------
// Platformer — the ground, and what counts as it
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "Platformer configures the object rather than owning gravity") {
    World world;
    rmp::Object &player = stander(world);
    player.add<rmp::behavior::Platformer>({});
    // The same two fields the game would write, so `player.gravity_scale = 2`
    // afterwards works and there are not two places where gravity lives.
    CHECK(player.solid);
    CHECK(player.gravity_scale == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture, "Platformer: gravity accumulates with nothing underneath") {
    World world;
    rmp::Object &player =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 20 }) });
    auto &platformer = player.add<rmp::behavior::Platformer>({ .gravity = 1200 });

    for (int i = 0; i < 60; i++) tick(player, 1.0f / 60);
    CHECK_FALSE(platformer.on_ground());
    CHECK(player.velocity.y == doctest::Approx(1200).epsilon(0.001));
}

TEST_CASE_FIXTURE(Fixture, "Platformer: a solid floor under the feet is ground") {
    World world;
    solid_floor(world);
    rmp::Object &player = stander(world);
    auto &platformer = player.add<rmp::behavior::Platformer>({ .gravity = 1200 });

    for (int i = 0; i < 60; i++) tick(player, 1.0f / 60);
    CHECK(platformer.on_ground());
    // And gravity does NOT accumulate while standing, which is the symptom the
    // ground check exists to prevent.
    CHECK(player.velocity.y == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture, "Platformer: a coin lying on the floor is not the floor") {
    // CORE-10, and the reason this file exists. raycast() answers with the
    // NEAREST hit and the check then asked whether THAT one was solid, so any
    // non-solid collider between the feet and the floor hid the floor: the
    // player could not jump and velocity.y accumulated while it stood still.
    // Coins, pickups, damage triggers and decoration on the floor are the
    // ordinary contents of a platformer level.
    World world;
    solid_floor(world);
    rmp::Object &coin =
        world.spawn({ .position = { 0, 90 }, .shape = rmp::rect({ 8, 8 }) });
    coin.solid = false; // a coin is not a wall
    rmp::Object &player = stander(world);
    auto &platformer = player.add<rmp::behavior::Platformer>({ .gravity = 1200 });

    tick(player, 1.0f / 60);
    CHECK(platformer.on_ground());
    CHECK(player.velocity.y == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture,
                  "Platformer: a non-solid object over a hole is still not ground") {
    // The other half of the same rule, and the one that says the fix did not
    // simply stop looking at `solid`: with no floor at all, the coin must not
    // become one.
    World world;
    rmp::Object &coin =
        world.spawn({ .position = { 0, 90 }, .shape = rmp::rect({ 8, 8 }) });
    coin.solid = false;
    rmp::Object &player = stander(world);
    auto &platformer = player.add<rmp::behavior::Platformer>({ .gravity = 1200 });

    tick(player, 1.0f / 60);
    CHECK_FALSE(platformer.on_ground());
    CHECK(player.velocity.y == doctest::Approx(20).epsilon(0.001));
}

TEST_CASE_FIXTURE(Fixture, "Platformer: the ground is what the collision mask can see") {
    World world;
    rmp::Object &floor = solid_floor(world);
    floor.collision_layer = 1u << 4;
    rmp::Object &player = stander(world);
    player.collision_mask = 1u; // not the floor's layer
    auto &platformer = player.add<rmp::behavior::Platformer>({});

    tick(player, 1.0f / 60);
    CHECK_FALSE(platformer.on_ground());

    player.collision_mask = 1u << 4;
    tick(player, 1.0f / 60);
    CHECK(platformer.on_ground());
}

// ---------------------------------------------------------------------------
// Platformer — moving and jumping
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "Platformer: the move actions set the horizontal velocity") {
    World world;
    rmp::Object &player =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 20 }) });
    player.add<rmp::behavior::Platformer>({ .speed = 200, .gravity = 0 });

    tick(player, 1.0f / 60);
    CHECK(player.velocity.x == doctest::Approx(0));

    hold(KEY_D); // move_right, out of the box
    tick(player, 1.0f / 60);
    CHECK(player.velocity.x == doctest::Approx(200));

    hold(KEY_D, false);
    hold(KEY_A); // move_left
    tick(player, 1.0f / 60);
    CHECK(player.velocity.x == doctest::Approx(-200));

    // Both at once is neither, which is what a player mashing both expects.
    hold(KEY_D);
    tick(player, 1.0f / 60);
    CHECK(player.velocity.x == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture,
                  "Platformer: it jumps from the ground, and not again in the air") {
    World world;
    solid_floor(world);
    rmp::Object &player = stander(world);
    auto &platformer =
        player.add<rmp::behavior::Platformer>({ .gravity = 0, .jump = 500 });

    tick(player, 1.0f / 60); // standing, and no edge on the first frame
    REQUIRE(platformer.on_ground());

    hold(KEY_SPACE); // ui_accept
    tick(player, 1.0f / 60);
    CHECK(player.velocity.y == doctest::Approx(-500));
    CHECK_FALSE(platformer.on_ground());

    // Off the ground with air_jumps = 0, a second press is refused.
    hold(KEY_SPACE, false);
    tick(player, 1.0f / 60);
    hold(KEY_SPACE);
    tick(player, 1.0f / 60);
    CHECK(player.velocity.y == doctest::Approx(-500));
}

TEST_CASE_FIXTURE(Fixture,
                  "Platformer: an object that was never on the ground cannot jump") {
    // `jumps_used < air_jumps + 1` on its own hands a free jump to anything
    // that has not jumped yet -- including something that has never touched the
    // ground, and including a player long past the end of coyote time. Leaving
    // the ground SPENDS the ground jump; that is what makes coyote_time mean
    // anything and what makes the buffered jump observable at all.
    World world;
    rmp::Object &player =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 20 }) });
    auto &platformer =
        player.add<rmp::behavior::Platformer>({ .gravity = 0, .jump = 500 });

    tick(player, 1.0f / 60);
    hold(KEY_SPACE);
    tick(player, 1.0f / 60);
    CHECK_FALSE(platformer.on_ground());
    CHECK(player.velocity.y == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture,
                  "Platformer: coyote time is just after the ledge, and only just") {
    // Two runs of the same fall, differing only in how long the player waits
    // before pressing. The boundary is the feature: nobody can name coyote
    // time, and everybody notices its absence.
    auto fall_then_jump = [](float wait_seconds) {
        World world;
        rmp::Object &floor = solid_floor(world);
        rmp::Object &player = stander(world);
        player.add<rmp::behavior::Platformer>(
            { .gravity = 0, .jump = 500, .coyote_time = 0.1f });

        tick(player, 1.0f / 60); // standing on it
        floor.destroy();
        rmp::objects::detail::collect(); // the ledge is gone

        const auto steps = static_cast<int>(wait_seconds * 60);
        for (int i = 0; i < steps; i++) tick(player, 1.0f / 60);
        hold(KEY_SPACE);
        tick(player, 1.0f / 60);
        hold(KEY_SPACE, false);
        return player.velocity.y;
    };

    CHECK(fall_then_jump(0.05f) == doctest::Approx(-500)); // inside the window
    CHECK(fall_then_jump(0.30f) == doctest::Approx(0)); // and outside it
}

TEST_CASE_FIXTURE(Fixture,
                  "Platformer: a jump pressed just before landing fires on landing") {
    World world;
    solid_floor(world);
    // Well above the floor: in the air, with the ground jump already spent.
    rmp::Object &player =
        world.spawn({ .position = { 0, 40 }, .shape = rmp::rect({ 10, 20 }) });
    auto &platformer = player.add<rmp::behavior::Platformer>(
        { .gravity = 0, .jump = 500, .jump_buffer = 0.12f });

    tick(player, 1.0f / 60);
    REQUIRE_FALSE(platformer.on_ground());

    hold(KEY_SPACE);
    tick(player, 1.0f / 60);
    hold(KEY_SPACE, false);
    CHECK(player.velocity.y == doctest::Approx(0)); // too early, and remembered

    player.position.y = 80; // lands, one frame later
    tick(player, 1.0f / 60);
    CHECK(player.velocity.y == doctest::Approx(-500));
}

TEST_CASE_FIXTURE(Fixture,
                  "Platformer: a jump pressed too long before landing is forgotten") {
    World world;
    solid_floor(world);
    rmp::Object &player =
        world.spawn({ .position = { 0, 40 }, .shape = rmp::rect({ 10, 20 }) });
    player.add<rmp::behavior::Platformer>(
        { .gravity = 0, .jump = 500, .jump_buffer = 0.12f });

    tick(player, 1.0f / 60);
    hold(KEY_SPACE);
    tick(player, 1.0f / 60);
    hold(KEY_SPACE, false);
    for (int i = 0; i < 30; i++) tick(player, 1.0f / 60); // half a second

    player.position.y = 80;
    tick(player, 1.0f / 60);
    CHECK(player.velocity.y == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture,
                  "Platformer: air_jumps = 1 buys one more jump, and exactly one") {
    World world;
    solid_floor(world);
    rmp::Object &player = stander(world);
    auto &platformer = player.add<rmp::behavior::Platformer>(
        { .gravity = 0, .jump = 500, .air_jumps = 1 });

    tick(player, 1.0f / 60);
    REQUIRE(platformer.on_ground());

    auto press = [&player]() {
        hold(KEY_SPACE);
        tick(player, 1.0f / 60);
        hold(KEY_SPACE, false);
        tick(player, 1.0f / 60);
    };

    press(); // 1: from the ground
    CHECK(player.velocity.y == doctest::Approx(-500));

    player.velocity.y = -1; // airborne, and a marker
    press(); // 2: the air jump
    CHECK(player.velocity.y == doctest::Approx(-500));

    player.velocity.y = -1;
    press(); // 3: and no more
    CHECK(player.velocity.y == doctest::Approx(-1));
}

// ---------------------------------------------------------------------------
// Parallax
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "Parallax: the first copy's offset never leaves [-width, 0]") {
    // The twenty lines everybody writes wrong the first time. std::fmod returns
    // a NEGATIVE for a negative shift and a POSITIVE for a positive one, and a
    // positive offset is a gap down the left-hand side of the screen.
    // An integer counter with the shift derived from it: a float loop counter
    // accumulates the very error this behavior exists to keep out of the seam.
    for (int step = -200; step <= 200; step++) {
        const float shift = static_cast<float>(step) * 7.5f;
        const rmp::behavior::detail::ParallaxTiling tiling =
            rmp::behavior::detail::parallax_tiling(shift, 100, 800);
        REQUIRE(tiling.offset <= 0.0f);
        REQUIRE(tiling.offset > -100.0f);
    }
}

TEST_CASE_FIXTURE(Fixture, "Parallax: the offset repeats every texture width, exactly") {
    // The seam this behavior exists to remove is a third of a pixel between two
    // copies, so "about equal" is not the assertion.
    using rmp::behavior::detail::parallax_tiling;
    for (int step = -20; step <= 20; step++) {
        const float shift = static_cast<float>(step) * 12.5f;
        const float here = parallax_tiling(shift, 100, 800).offset;
        const float wrapped = parallax_tiling(shift + 100, 100, 800).offset;
        REQUIRE(wrapped == doctest::Approx(here).epsilon(0.0001));
    }
}

TEST_CASE_FIXTURE(Fixture,
                  "Parallax: enough copies to cover the screen, whatever the offset") {
    using rmp::behavior::detail::parallax_tiling;
    for (int step = -167; step <= 167; step++) {
        const float shift = static_cast<float>(step) * 3.0f;
        const rmp::behavior::detail::ParallaxTiling tiling =
            parallax_tiling(shift, 100, 800);
        // The last copy's right-hand edge has to be at or past the screen's.
        const float covered = tiling.offset + static_cast<float>(tiling.copies) * 100.0f;
        REQUIRE(covered >= 800.0f);
        // And not wastefully more than one copy past it.
        REQUIRE(covered < 800.0f + 2 * 100.0f);
    }
}

TEST_CASE_FIXTURE(
    Fixture,
    "Parallax: a texture with no width draws nothing rather than dividing by it") {
    using rmp::behavior::detail::parallax_tiling;
    CHECK(parallax_tiling(120, 0, 800).copies == 0);
    CHECK(parallax_tiling(120, -5, 800).copies == 0);
    // And a screen narrower than one copy still gets one.
    CHECK(parallax_tiling(0, 100, 10).copies == 1);
}

TEST_CASE_FIXTURE(Fixture,
                  "Parallax: speed is its own drift, and factor is not applied to it") {
    // `factor` multiplies where the OBJECT is, at draw time, so that moving one
    // object moves every layer at its own rate with no bookkeeping in between.
    // Applying it to the drift as well would make a pinned layer (factor 0)
    // stop drifting, and a drifting sky is the case `speed` exists for.
    World world;
    rmp::Object &sky = world.spawn();
    auto &parallax = sky.add<rmp::behavior::Parallax>({ .factor = 0, .speed = 30 });

    for (int i = 0; i < 60; i++) tick(sky, 1.0f / 60);
    CHECK(parallax.ours.scroll == doctest::Approx(30).epsilon(0.001));
}

TEST_CASE_FIXTURE(
    Fixture, "Parallax: no texture name loads nothing, and drawing it is not a crash") {
    World world;
    rmp::Object &layer = world.spawn();
    auto &parallax = layer.add<rmp::behavior::Parallax>({});
    CHECK_FALSE(parallax.ours.art.valid());
    // _draw returns before it reaches raylib, which is the only reason this
    // line can be in a test with no GL context at all.
    rmp::objects::detail::draw_behaviors(layer);
    CHECK_FALSE(parallax.ours.art.valid());
}

// ---------------------------------------------------------------------------
// TopDown — the direction, and the tag that follows from it
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "TopDown: eight directions, and the diagonal is not 41 % faster") {
    World world;
    rmp::Object &player = world.spawn();
    auto &top = player.add<rmp::behavior::TopDown>({ .speed = 100 });

    struct Case {
        ::KeyboardKey first;
        ::KeyboardKey second;
        float x;
        float y;
    };
    // +Y is DOWN, matching raylib and rmp::Object::position.
    const Case cases[] = {
        { KEY_D, KEY_NULL, 1, 0 },           { KEY_A, KEY_NULL, -1, 0 },
        { KEY_W, KEY_NULL, 0, -1 },          { KEY_S, KEY_NULL, 0, 1 },
        { KEY_D, KEY_W, 0.7071f, -0.7071f }, { KEY_A, KEY_W, -0.7071f, -0.7071f },
        { KEY_A, KEY_S, -0.7071f, 0.7071f }, { KEY_D, KEY_S, 0.7071f, 0.7071f },
    };

    for (const Case &one : cases) {
        g_devices = rmp::input::detail::DeviceState{};
        hold(one.first);
        if (one.second != KEY_NULL) hold(one.second);
        tick(player, 1.0f / 60);

        CHECK(player.velocity.x == doctest::Approx(one.x * 100).epsilon(0.001));
        CHECK(player.velocity.y == doctest::Approx(one.y * 100).epsilon(0.001));
        CHECK(top.direction().x == doctest::Approx(one.x).epsilon(0.001));
        CHECK(top.direction().y == doctest::Approx(one.y).epsilon(0.001));
    }
}

TEST_CASE_FIXTURE(Fixture, "TopDown: four-way keeps the axis being pushed harder") {
    World world;
    rmp::Object &player = world.spawn();
    player.add<rmp::behavior::TopDown>({ .speed = 100, .eight_way = false });

    hold(KEY_D);
    hold(KEY_W);
    tick(player, 1.0f / 60);
    // A dead heat on a keyboard keeps the horizontal, because that is what
    // pressing both at once means to a player.
    CHECK(player.velocity.x == doctest::Approx(100));
    CHECK(player.velocity.y == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture,
                  "TopDown: the last non-zero direction is what it keeps facing") {
    World world;
    rmp::Object &player = world.spawn();
    auto &top = player.add<rmp::behavior::TopDown>({ .speed = 100 });

    hold(KEY_A);
    tick(player, 1.0f / 60);
    REQUIRE(top.direction().x == doctest::Approx(-1));

    hold(KEY_A, false);
    tick(player, 1.0f / 60);
    CHECK(player.velocity.x == doctest::Approx(0)); // stopped
    CHECK(top.direction().x == doctest::Approx(-1)); // still facing that way
}

TEST_CASE_FIXTURE(Fixture,
                  "TopDown: the tag is <walk>_<suffix> when the sheet has that tag") {
    World world;
    rmp::Object &player = world.spawn();
    player.sprite.sheet =
        sheet_with({ "idle", "walk", "walk_e", "walk_n", "walk_w", "walk_s", "walk_ne",
                     "walk_nw", "walk_sw", "walk_se" });
    player.add<rmp::behavior::TopDown>({ .speed = 100 });

    struct Case {
        ::KeyboardKey first;
        ::KeyboardKey second;
        const char *tag;
    };
    const Case cases[] = {
        { KEY_D, KEY_NULL, "walk_e" }, { KEY_W, KEY_NULL, "walk_n" },
        { KEY_A, KEY_NULL, "walk_w" }, { KEY_S, KEY_NULL, "walk_s" },
        { KEY_D, KEY_W, "walk_ne" },   { KEY_A, KEY_W, "walk_nw" },
        { KEY_A, KEY_S, "walk_sw" },   { KEY_D, KEY_S, "walk_se" },
    };

    for (const Case &one : cases) {
        g_devices = rmp::input::detail::DeviceState{};
        hold(one.first);
        if (one.second != KEY_NULL) hold(one.second);
        tick(player, 1.0f / 60);
        CHECK_MESSAGE(playing(player) == one.tag, "wanted ", one.tag);
    }
}

TEST_CASE_FIXTURE(Fixture,
                  "TopDown: idle when still, walk when moving, and the names are yours") {
    World world;
    rmp::Object &player = world.spawn();
    player.sprite.sheet = sheet_with({ "reposo", "corre", "corre_e" });
    // WE INVENT NO NAMES: these are the tags in the .aseprite.
    player.add<rmp::behavior::TopDown>(
        { .speed = 100, .idle = "reposo", .walk = "corre" });

    tick(player, 1.0f / 60);
    CHECK(playing(player) == "reposo");

    hold(KEY_D);
    tick(player, 1.0f / 60);
    CHECK(playing(player) == "corre_e");

    hold(KEY_D, false);
    tick(player, 1.0f / 60);
    CHECK(playing(player) == "reposo");
}

TEST_CASE_FIXTURE(Fixture,
                  "TopDown: a sheet with no suffixes falls back to <walk> and flips") {
    // The half that works with no direction sheet at all: one set of frames,
    // mirrored for the left. A two-, four- or eight-direction sheet all work
    // with nothing configured, and this is the two-direction one.
    World world;
    rmp::Object &player = world.spawn();
    player.sprite.sheet = sheet_with({ "idle", "walk" });
    player.add<rmp::behavior::TopDown>({ .speed = 100 });

    hold(KEY_D);
    tick(player, 1.0f / 60);
    CHECK(playing(player) == "walk");
    CHECK_FALSE(player.flip_x);

    hold(KEY_D, false);
    hold(KEY_A);
    tick(player, 1.0f / 60);
    CHECK(playing(player) == "walk");
    CHECK(player.flip_x);
}

TEST_CASE_FIXTURE(
    Fixture,
    "TopDown: a four-tag sheet uses the four it has and falls back for the rest") {
    World world;
    rmp::Object &player = world.spawn();
    player.sprite.sheet =
        sheet_with({ "idle", "walk", "walk_e", "walk_n", "walk_w", "walk_s" });
    player.add<rmp::behavior::TopDown>({ .speed = 100 });

    hold(KEY_W);
    tick(player, 1.0f / 60);
    CHECK(playing(player) == "walk_n");

    // North-east is not in the sheet, so the base tag carries it.
    hold(KEY_D);
    tick(player, 1.0f / 60);
    CHECK(playing(player) == "walk");
}

TEST_CASE_FIXTURE(Fixture,
                  "TopDown: a tag name too long for the buffer falls back instead of "
                  "truncating into a stranger") {
    // The tag is built into a fixed char[kMaxTagName * 2] from two strings the
    // user supplied. A name that does not fit has to come back as "not in the
    // sheet", not as some other tag's name by accident.
    World world;
    rmp::Object &player = world.spawn();
    const char *long_base = "a_very_long_animation_tag_name_that_fills_the_buffer_"
                            "and_then_some_more_for_good_measure";
    player.sprite.sheet = sheet_with({ "idle", long_base });
    player.add<rmp::behavior::TopDown>({ .speed = 100, .walk = long_base });

    hold(KEY_D);
    tick(player, 1.0f / 60);
    // The sheet's tag names are 32 bytes, so the long one is not in it either;
    // what matters is that nothing wrote past the buffer and the answer is the
    // base name being asked for.
    CHECK(rmp::animation::detail::tag_index(player.sprite.sheet.raw(), long_base) < 0);
}

TEST_CASE_FIXTURE(Fixture,
                  "TopDown: no sheet at all is not a crash and not a warning storm") {
    World world;
    rmp::Object &player = world.spawn();
    player.add<rmp::behavior::TopDown>({ .speed = 100 });
    hold(KEY_D);
    for (int i = 0; i < 60; i++) tick(player, 1.0f / 60);
    CHECK(player.velocity.x == doctest::Approx(100));
    CHECK(rmp::detail::report_count() == 0);
}

// ---------------------------------------------------------------------------
// Spawner — the cap is on what is ALIVE
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "Spawner: max_alive counts the living, and lets go when they die") {
    // CORE-11. `ours.alive` was incremented and never decremented, so the field
    // called max_alive was a cap on how many the spawner had EVER made: an
    // endless runner stopped producing obstacles thirty seconds in, with
    // nothing it had made still on screen.
    World world;
    int made = 0;
    std::vector<rmp::Handle<rmp::Object>> children;
    rmp::Object &source = world.spawn();
    auto &spawner = source.add<rmp::behavior::Spawner>({
        .every_seconds = 0.1f,
        .max_alive = 3,
        .on_spawn =
            [&](rmp::Scene &scene, Vector2 where) {
                children.push_back(scene.spawn({ .position = where }).handle());
                made++;
            },
    });

    for (int i = 0; i < 300; i++) frame(world, 1.0f / 60);
    CHECK(made == 3);
    CHECK(spawner.alive() == 3);

    // One dies, and the next due tick makes one more. That is the whole
    // difference between a cap and a quota.
    REQUIRE(children.front());
    children.front()->destroy();
    rmp::objects::detail::collect();
    CHECK(spawner.alive() == 2);

    for (int i = 0; i < 30; i++) frame(world, 1.0f / 60);
    CHECK(made == 4);
    CHECK(spawner.alive() == 3);
}

TEST_CASE_FIXTURE(Fixture, "Spawner: objects that live briefly never stop it") {
    // The failure as a player would meet it: things that die on their own, and
    // a spawner that has to keep going for the whole run.
    World world;
    int made = 0;
    rmp::Object &source = world.spawn();
    auto &spawner = source.add<rmp::behavior::Spawner>({
        .every_seconds = 0.1f,
        .max_alive = 3,
        .on_spawn =
            [&](rmp::Scene &scene, Vector2 where) {
                rmp::Object &one = scene.spawn({ .position = where });
                one.add<rmp::behavior::Lifespan>({ .seconds = 0.05f });
                made++;
            },
    });

    for (int i = 0; i < 600; i++) frame(world, 1.0f / 60); // ten seconds
    CHECK(made > 80); // about one every 0.1 s, and not three ever
    CHECK(spawner.alive() <= 3);
}

TEST_CASE_FIXTURE(Fixture, "Spawner: a call that makes five counts five") {
    World world;
    rmp::Object &source = world.spawn();
    auto &spawner = source.add<rmp::behavior::Spawner>({
        .every_seconds = 0.1f,
        .max_alive = 4,
        .on_spawn =
            [](rmp::Scene &scene, Vector2 where) {
                for (int i = 0; i < 5; i++) scene.spawn({ .position = where });
            },
    });

    for (int i = 0; i < 120; i++) frame(world, 1.0f / 60);
    // A wave spawner makes five, and the cap has to count them all or it is not
    // a cap: one wave and then nothing, because five is already over four.
    CHECK(spawner.alive() == 5);
}

TEST_CASE_FIXTURE(Fixture,
                  "Spawner: a callback that spawns nothing does not fill the table") {
    World world;
    int calls = 0;
    rmp::Object &source = world.spawn();
    auto &spawner = source.add<rmp::behavior::Spawner>({
        .every_seconds = 0.1f,
        .max_alive = 2,
        .on_spawn = [&](rmp::Scene &, Vector2) { calls++; },
    });

    for (int i = 0; i < 600; i++) frame(world, 1.0f / 60);
    CHECK(calls > 80);
    CHECK(spawner.alive() == 0);
}

TEST_CASE_FIXTURE(
    Fixture, "Spawner: a cap bigger than it can track says so once and holds the limit") {
    World world;
    int made = 0;
    rmp::Object &source = world.spawn();
    auto &spawner = source.add<rmp::behavior::Spawner>({
        .every_seconds = 0.01f,
        .max_alive = rmp::behavior::kMaxSpawned + 10,
        .on_spawn =
            [&](rmp::Scene &scene, Vector2 where) {
                scene.spawn({ .position = where });
                made++;
            },
    });

    for (int i = 0; i < 600; i++) frame(world, 1.0f / 60);
    CHECK(made == rmp::behavior::kMaxSpawned);
    CHECK(spawner.alive() == rmp::behavior::kMaxSpawned);
    // Once, not once per frame: a warning printed sixty times a second is the
    // same as no warning at all.
    CHECK(rmp::detail::report_count() == 1);
}

// ---------------------------------------------------------------------------
// Health — which layers hurt
// ---------------------------------------------------------------------------

namespace {
constexpr unsigned kEnemyLayer = 1u << 1;
constexpr unsigned kCoinLayer = 1u << 2;

rmp::Object &toucher(World &world, unsigned layer) {
    rmp::Object &one =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 8, 8 }) });
    one.collision_layer = layer;
    one.collision_mask = 0xFFFFFFFFu;
    return one;
}
} // namespace

TEST_CASE_FIXTURE(Fixture, "Health: contact with a layer in hurt_by costs hp") {
    World world;
    rmp::Object &player =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 20, 20 }) });
    player.collision_mask = 0xFFFFFFFFu;
    auto &health = player.add<rmp::behavior::Health>({ .hp = 5,
                                                       .max_hp = 5,
                                                       .invulnerable_for = 0,
                                                       .hurt_by = kEnemyLayer,
                                                       .damage_on_hit = 2 });

    // A coin is contact, and contact is not damage.
    toucher(world, kCoinLayer);
    frame(world, 1.0f / 60);
    CHECK(health.hp == 5);

    toucher(world, kEnemyLayer);
    frame(world, 1.0f / 60);
    CHECK(health.hp == 3);
}

TEST_CASE_FIXTURE(Fixture, "Health: hurt_by = 0 is never, which is the default") {
    World world;
    rmp::Object &player =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 20, 20 }) });
    player.collision_mask = 0xFFFFFFFFu;
    auto &health = player.add<rmp::behavior::Health>({ .hp = 5, .invulnerable_for = 0 });

    toucher(world, kEnemyLayer);
    toucher(world, kCoinLayer);
    for (int i = 0; i < 10; i++) frame(world, 1.0f / 60);
    CHECK(health.hp == 5);
}

TEST_CASE_FIXTURE(Fixture,
                  "Health: the invulnerable window applies to contact damage too") {
    // Standing inside a fire costs one heart and a window, not sixty a second.
    World world;
    rmp::Object &player =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 20, 20 }) });
    player.collision_mask = 0xFFFFFFFFu;
    auto &health = player.add<rmp::behavior::Health>(
        { .hp = 5, .max_hp = 5, .invulnerable_for = 0.5f, .hurt_by = kEnemyLayer });

    toucher(world, kEnemyLayer);
    for (int i = 0; i < 12; i++) frame(world, 1.0f / 60); // a fifth of a second
    CHECK(health.hp == 4);
}

TEST_CASE_FIXTURE(
    Fixture, "Health: contact damage goes through on_damage and kills like any other") {
    World world;
    int landed = 0;
    int deaths = 0;
    rmp::Object &player =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 20, 20 }) });
    player.collision_mask = 0xFFFFFFFFu;
    player.add<rmp::behavior::Health>({
        .hp = 1,
        .max_hp = 1,
        .invulnerable_for = 0,
        .destroy_on_death = false,
        .on_death = [&](rmp::Object &) { deaths++; },
        .on_damage = [&](rmp::Object &, int amount) { landed += amount; },
        .hurt_by = kEnemyLayer,
    });

    toucher(world, kEnemyLayer);
    for (int i = 0; i < 10; i++) frame(world, 1.0f / 60);
    CHECK(landed == 1);
    CHECK(deaths == 1); // once, however many frames the contact lasts
}

// ---------------------------------------------------------------------------
// Runner: the two holes the first round of Platformer tests exposed, in the
// behavior that shares its ground check.
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "Runner: an object that was never on the ground cannot jump") {
    // The same budget rule as Platformer: leaving the ground spends the ground
    // jump, so `jumps_used < air_jumps + 1` cannot hand a free one to a runner
    // that has never stood on anything.
    World world;
    rmp::Object &player =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 20 }) });
    player.add<rmp::behavior::Runner>({ .gravity = 0, .jump = 500 });

    tick(player, 1.0f / 60);
    hold(KEY_SPACE);
    tick(player, 1.0f / 60);
    CHECK(player.velocity.y == doctest::Approx(0));

    SUBCASE("and one air jump is exactly one, after the ground jump is spent") {
        hold(KEY_SPACE, false);
        tick(player, 1.0f / 60);
        rmp::Object &other =
            world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 20 }) });
        other.add<rmp::behavior::Runner>({ .gravity = 0, .jump = 500, .air_jumps = 1 });
        tick(other, 1.0f / 60);
        hold(KEY_SPACE);
        tick(other, 1.0f / 60);
        CHECK(other.velocity.y == doctest::Approx(-500));
        hold(KEY_SPACE, false);
        tick(other, 1.0f / 60);
        hold(KEY_SPACE);
        tick(other, 1.0f / 60);
        CHECK(other.velocity.y == doctest::Approx(-500)); // no second one
    }
}

// ---------------------------------------------------------------------------
// What the six games found: a hit is a hit even when the bullet dies of it,
// and `{ .hp = 5 }` is a whole configuration.
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "a projectile that dies on contact still counts as a hit") {
    // Contact notification used to stop the moment EITHER side died. A
    // Projectile with destroy_on_hit -- the default -- destroyed itself in its
    // own _collision, and the alien's on_collision and Health never ran: every
    // player in the six games was immortal and every enemy unkillable, on a
    // coin flip of which object the pair listed first.
    World world;
    constexpr unsigned kBullets = 1u << 3;
    int hits = 0;
    rmp::Object &alien =
        world.spawn({ .position = { 100, 100 }, .shape = rmp::rect({ 40, 40 }) });
    alien.add<rmp::behavior::Health>(
        { .hp = 2, .invulnerable_for = 0, .hurt_by = kBullets });
    alien.on_collision([&hits](rmp::Object &, rmp::Object &) { hits++; });

    for (int order = 0; order < 2; order++) {
        // Both orders of the pair, because that was the coin.
        rmp::Object &bullet =
            world.spawn({ .position = { 100, 100 }, .shape = rmp::rect({ 4, 4 }) });
        bullet.collision_layer = kBullets;
        bullet.add<rmp::behavior::Projectile>({ .speed = 0 });
        frame(world, 1.0f / 60);
        CHECK_FALSE(bullet.alive());
        CHECK(hits == order + 1); // the killing blow is reported too
        if (order == 0) {
            CHECK(alien.get<rmp::behavior::Health>()->hp == 1);
        } else {
            CHECK_FALSE(alien.alive()); // and it did kill
        }
    }
}

TEST_CASE_FIXTURE(Fixture, "Health: max_hp follows hp unless it is given") {
    World world;
    rmp::Object &a =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    auto &five = a.add<rmp::behavior::Health>({ .hp = 5 });
    CHECK(five.max_hp == 5);
    five.damage(a, 2);
    five.heal(10);
    CHECK(five.hp == 5); // clamped to the five, not to a three nobody set

    rmp::Object &b =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    auto &given = b.add<rmp::behavior::Health>({ .hp = 2, .max_hp = 9 });
    CHECK(given.max_hp == 9);
    given.heal(100);
    CHECK(given.hp == 9);
}
