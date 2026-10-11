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

#include <array>
#include <cmath>
#include <limits>
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
// The fake device the input seam reads. The dot says "file state".
struct {
    rmp::input::detail::DeviceState devices;
} fake;

void fake_sample(rmp::input::detail::DeviceState &out) { out = fake.devices; }

struct Fixture {
    Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
        rmp::objects::detail::reset_pointer_for_tests();
        rmp::input::detail::reset();
        fake.devices = rmp::input::detail::DeviceState{};
        rmp::input::detail::set_sample_provider(fake_sample);
        rmp::detail::reset_reports_for_tests();
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    ~Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
        // reset() puts the raylib provider back, so nothing leaks a pointer
        // into this translation unit after the case ends.
        rmp::input::detail::reset();
        fake.devices = rmp::input::detail::DeviceState{};
        rmp::detail::release_all();
    }
};

void hold(::KeyboardKey key, bool down = true) { fake.devices.keys[key] = down; }

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
rmp::SpriteSheet sheet_with(const std::vector<std::string> &names) {
    rmp::SheetData data{};
    data.width = 16;
    data.height = 16;
    data.frames.resize(1);
    data.frames[0].seconds = 0.1f;
    data.tags.resize(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        data.tags[i].name = names[i];
    }

    auto *slot = rmp::detail::adopt(rmp::detail::ResourceKind::SHEET, std::move(data));
    REQUIRE(slot != nullptr);
    return rmp::SpriteSheet{ slot };
}

std::string playing(const rmp::Object &object) {
    return std::string(object.sprite.playing());
}

} // namespace

// ---------------------------------------------------------------------------
// Platformer — the ground, and what counts as it
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(
    Fixture, "Platformer falls by its own gravity, and gravity_scale adds the scene's") {
    // The behavior integrates its own `gravity`, so `jump` reaches the same
    // height in any scene, and leaves gravity_scale at 0. Writing gravity_scale
    // afterwards does not change its gravity: it adds the scene's on top.
    World world;
    world.gravity = { 0, 1000 };
    rmp::Object &player =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 20 }) });
    player.add<rmp::behavior::Platformer>({ .gravity = 2000 });
    CHECK(player.solid);
    CHECK(player.gravity_scale == doctest::Approx(0));

    const float delta = 1.0f / 60;
    frame(world, delta);
    CHECK(player.velocity.y == doctest::Approx(2000 * delta)); // the scene's is not in it

    player.gravity_scale = 1;
    frame(world, delta);
    CHECK(player.velocity.y == doctest::Approx(2000 * delta + (2000 + 1000) * delta));

    player.gravity_scale = 0;
    player.get<rmp::behavior::Platformer>()->gravity = 3000; // heavier is this field
    frame(world, delta);
    CHECK(player.velocity.y == doctest::Approx((2000 + 3000 + 3000) * delta));
}

TEST_CASE_FIXTURE(Fixture, "Platformer: gravity accumulates with nothing underneath") {
    World world;
    rmp::Object &player =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 20 }) });
    auto &platformer = player.add<rmp::behavior::Platformer>({ .gravity = 1200 });

    for (int i = 0; i < 60; ++i) tick(player, 1.0f / 60);
    CHECK_FALSE(platformer.grounded());
    CHECK(player.velocity.y == doctest::Approx(1200).epsilon(0.001));
}

TEST_CASE_FIXTURE(Fixture, "Platformer: a solid floor under the feet is ground") {
    World world;
    solid_floor(world);
    rmp::Object &player = stander(world);
    auto &platformer = player.add<rmp::behavior::Platformer>({ .gravity = 1200 });

    for (int i = 0; i < 60; ++i) tick(player, 1.0f / 60);
    CHECK(platformer.grounded());
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
    CHECK(platformer.grounded());
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
    CHECK_FALSE(platformer.grounded());
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
    CHECK_FALSE(platformer.grounded());

    player.collision_mask = 1u << 4;
    tick(player, 1.0f / 60);
    CHECK(platformer.grounded());
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
    REQUIRE(platformer.grounded());

    hold(KEY_SPACE); // ui_accept
    tick(player, 1.0f / 60);
    CHECK(player.velocity.y == doctest::Approx(-500));
    CHECK_FALSE(platformer.grounded());

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
    CHECK_FALSE(platformer.grounded());
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
        for (int i = 0; i < steps; ++i) tick(player, 1.0f / 60);
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
    REQUIRE_FALSE(platformer.grounded());

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
    for (int i = 0; i < 30; ++i) tick(player, 1.0f / 60); // half a second

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
    REQUIRE(platformer.grounded());

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
    for (int step = -200; step <= 200; ++step) {
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
    for (int step = -20; step <= 20; ++step) {
        const float shift = static_cast<float>(step) * 12.5f;
        const float here = parallax_tiling(shift, 100, 800).offset;
        const float wrapped = parallax_tiling(shift + 100, 100, 800).offset;
        REQUIRE(wrapped == doctest::Approx(here).epsilon(0.0001));
    }
}

TEST_CASE_FIXTURE(Fixture,
                  "Parallax: enough copies to cover the screen, whatever the offset") {
    using rmp::behavior::detail::parallax_tiling;
    for (int step = -167; step <= 167; ++step) {
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

TEST_CASE_FIXTURE(
    Fixture, "Parallax: factor 1 stands still in the world, factor 0 on the screen") {
    // What `factor` means, which the header and the implementation once said
    // two different ways. The first copy's left edge is offset pixels from the
    // view's left edge; the view's left edge is at view_x in the world.
    using rmp::behavior::detail::parallax_shift;
    using rmp::behavior::detail::parallax_tiling;
    constexpr float WIDTH = 1000;
    auto on_screen = [](float factor, float view_x) {
        return parallax_tiling(parallax_shift(0, view_x, factor, 0), WIDTH, 800).offset;
    };
    for (const float view_x : { 0.0f, 130.0f, 470.0f, 999.0f }) {
        CAPTURE(view_x);
        // Factor 1: wherever the view goes, the layer is at the same place in
        // the world -- it scrolls exactly as the ground does.
        CHECK(view_x + on_screen(1, view_x) == doctest::Approx(0));
        // Factor 0: the same place on the screen -- it goes with the camera.
        CHECK(on_screen(0, view_x) == doctest::Approx(0));
        // In between, a share of the scroll: half, at 0.5.
        CHECK(on_screen(0.5f, view_x) == doctest::Approx(-view_x / 2));
    }
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

    for (int i = 0; i < 60; ++i) tick(sky, 1.0f / 60);
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
    const std::array<Case, 8> cases{ {
        { KEY_D, KEY_NULL, 1, 0 },
        { KEY_A, KEY_NULL, -1, 0 },
        { KEY_W, KEY_NULL, 0, -1 },
        { KEY_S, KEY_NULL, 0, 1 },
        { KEY_D, KEY_W, 0.7071f, -0.7071f },
        { KEY_A, KEY_W, -0.7071f, -0.7071f },
        { KEY_A, KEY_S, -0.7071f, 0.7071f },
        { KEY_D, KEY_S, 0.7071f, 0.7071f },
    } };

    for (const Case &one : cases) {
        fake.devices = rmp::input::detail::DeviceState{};
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
    const std::array<Case, 8> cases{ {
        { KEY_D, KEY_NULL, "walk_e" },
        { KEY_W, KEY_NULL, "walk_n" },
        { KEY_A, KEY_NULL, "walk_w" },
        { KEY_S, KEY_NULL, "walk_s" },
        { KEY_D, KEY_W, "walk_ne" },
        { KEY_A, KEY_W, "walk_nw" },
        { KEY_A, KEY_S, "walk_sw" },
        { KEY_D, KEY_S, "walk_se" },
    } };

    for (const Case &one : cases) {
        fake.devices = rmp::input::detail::DeviceState{};
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
    "TopDown: a tag with a direction is not mirrored, and the rest faces right") {
    // "walk_w" is drawn facing west. Mirroring it because the character faces
    // left turned it to face east -- the flip is for the frames that have no
    // direction of their own, which are drawn facing right.
    World world;
    rmp::Object &player = world.spawn();
    player.sprite.sheet = sheet_with({ "idle", "walk", "walk_e", "walk_w" });
    player.add<rmp::behavior::TopDown>({ .speed = 100 });

    hold(KEY_A);
    tick(player, 1.0f / 60);
    CHECK(playing(player) == "walk_w");
    CHECK_FALSE(player.flip_x);

    // Up and left: no "walk_nw", so the base tag, mirrored to face left.
    hold(KEY_W);
    tick(player, 1.0f / 60);
    CHECK(playing(player) == "walk");
    CHECK(player.flip_x);

    // Standing still facing left: idle has no direction, so it is mirrored.
    hold(KEY_A, false);
    hold(KEY_W, false);
    tick(player, 1.0f / 60);
    CHECK(playing(player) == "idle");
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
                  "TopDown: a tag name of any length is matched whole, never by its "
                  "beginning") {
    // Tag names were a fixed char[32], cut at 31 characters, and the tag
    // TopDown asks for was built in a char[64]. They are std::strings now: a
    // long name is found as itself, and a name that only BEGINS like another
    // is not taken for it.
    World world;
    rmp::Object &player = world.spawn();
    const std::string long_base = "a_very_long_animation_tag_name_that_fills_the_buffer_"
                                  "and_then_some_more_for_good_measure";
    player.sprite.sheet = sheet_with({ "idle", long_base, long_base + "_e" });
    player.add<rmp::behavior::TopDown>({ .speed = 100, .walk = long_base });

    hold(KEY_D);
    tick(player, 1.0f / 60);
    CHECK(playing(player) == long_base + "_e");

    World other;
    rmp::Object &cut = other.spawn();
    const std::string first_31 = long_base.substr(0, 31);
    cut.sprite.sheet = sheet_with({ "idle", first_31 });
    cut.add<rmp::behavior::TopDown>({ .speed = 100, .walk = long_base });
    tick(cut, 1.0f / 60);
    CHECK(playing(cut) != first_31);
    CHECK(rmp::animation::detail::tag_index(cut.sprite.sheet.raw(), long_base) < 0);
}

TEST_CASE_FIXTURE(Fixture,
                  "TopDown: no sheet at all is not a crash and not a warning storm") {
    World world;
    rmp::Object &player = world.spawn();
    player.add<rmp::behavior::TopDown>({ .speed = 100 });
    hold(KEY_D);
    for (int i = 0; i < 60; ++i) tick(player, 1.0f / 60);
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
                ++made;
            },
    });

    for (int i = 0; i < 300; ++i) frame(world, 1.0f / 60);
    CHECK(made == 3);
    CHECK(spawner.alive() == 3);

    // One dies, and the next due tick makes one more. That is the whole
    // difference between a cap and a quota.
    REQUIRE(children.front());
    children.front()->destroy();
    rmp::objects::detail::collect();
    CHECK(spawner.alive() == 2);

    for (int i = 0; i < 30; ++i) frame(world, 1.0f / 60);
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
                ++made;
            },
    });

    for (int i = 0; i < 600; ++i) frame(world, 1.0f / 60); // ten seconds
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
                for (int i = 0; i < 5; ++i) scene.spawn({ .position = where });
            },
    });

    for (int i = 0; i < 120; ++i) frame(world, 1.0f / 60);
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
        .on_spawn = [&](rmp::Scene &, Vector2) { ++calls; },
    });

    for (int i = 0; i < 600; ++i) frame(world, 1.0f / 60);
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
        .max_alive = rmp::behavior::MAX_SPAWNED + 10,
        .on_spawn =
            [&](rmp::Scene &scene, Vector2 where) {
                scene.spawn({ .position = where });
                ++made;
            },
    });

    for (int i = 0; i < 600; ++i) frame(world, 1.0f / 60);
    CHECK(made == rmp::behavior::MAX_SPAWNED);
    CHECK(spawner.alive() == rmp::behavior::MAX_SPAWNED);
    // Once, not once per frame: a warning printed sixty times a second is the
    // same as no warning at all.
    CHECK(rmp::detail::report_count() == 1);
}

// ---------------------------------------------------------------------------
// Health — which layers hurt
// ---------------------------------------------------------------------------

namespace {
constexpr unsigned ENEMY_LAYER = 1u << 1;
constexpr unsigned COIN_LAYER = 1u << 2;

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
                                                       .hurt_by = ENEMY_LAYER,
                                                       .damage_on_hit = 2 });

    // A coin is contact, and contact is not damage.
    toucher(world, COIN_LAYER);
    frame(world, 1.0f / 60);
    CHECK(health.hp == 5);

    toucher(world, ENEMY_LAYER);
    frame(world, 1.0f / 60);
    CHECK(health.hp == 3);
}

TEST_CASE_FIXTURE(Fixture, "Health: hurt_by = 0 is never, which is the default") {
    World world;
    rmp::Object &player =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 20, 20 }) });
    player.collision_mask = 0xFFFFFFFFu;
    auto &health = player.add<rmp::behavior::Health>({ .hp = 5, .invulnerable_for = 0 });

    toucher(world, ENEMY_LAYER);
    toucher(world, COIN_LAYER);
    for (int i = 0; i < 10; ++i) frame(world, 1.0f / 60);
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
        { .hp = 5, .max_hp = 5, .invulnerable_for = 0.5f, .hurt_by = ENEMY_LAYER });

    toucher(world, ENEMY_LAYER);
    for (int i = 0; i < 12; ++i) frame(world, 1.0f / 60); // a fifth of a second
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
        .on_death = [&](rmp::Object &) { ++deaths; },
        .on_damage = [&](rmp::Object &, int amount) { landed += amount; },
        .hurt_by = ENEMY_LAYER,
    });

    toucher(world, ENEMY_LAYER);
    for (int i = 0; i < 10; ++i) frame(world, 1.0f / 60);
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
    constexpr unsigned BULLETS = 1u << 3;
    int hits = 0;
    rmp::Object &alien =
        world.spawn({ .position = { 100, 100 }, .shape = rmp::rect({ 40, 40 }) });
    alien.add<rmp::behavior::Health>(
        { .hp = 2, .invulnerable_for = 0, .hurt_by = BULLETS });
    alien.on_collision([&hits](rmp::Object &, rmp::Object &) { ++hits; });
    // Asked through handles: frame() collects the dead, so after it a
    // reference to one is a reference to freed memory -- which is what this
    // test did, unnoticed until the sanitized build (tools/sanitize_check.sh).
    const rmp::Handle<rmp::Object> alien_handle = alien.handle();

    for (int order = 0; order < 2; ++order) {
        // Both orders of the pair, because that was the coin.
        rmp::Object &bullet =
            world.spawn({ .position = { 100, 100 }, .shape = rmp::rect({ 4, 4 }) });
        bullet.collision_layer = BULLETS;
        bullet.add<rmp::behavior::Projectile>({ .speed = 0 });
        const rmp::Handle<rmp::Object> bullet_handle = bullet.handle();
        frame(world, 1.0f / 60);
        CHECK_FALSE(bullet_handle);
        CHECK(hits == order + 1); // the killing blow is reported too
        if (order == 0) {
            REQUIRE(alien_handle);
            CHECK(alien_handle->get<rmp::behavior::Health>()->hp == 1);
        } else {
            CHECK_FALSE(alien_handle); // and it did kill
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

// ---------------------------------------------------------------------------
// What tests/property/behavior_prop.cpp found: each counterexample, named.
// Every one of these failed -- or stopped under a sanitizer, or never
// returned -- before the fix it holds.
// ---------------------------------------------------------------------------

namespace {

class Loose : public rmp::Object {};

constexpr float FRAME = 1.0f / 60;
constexpr float NOT_A_NUMBER = std::numeric_limits<float>::quiet_NaN();
constexpr float INFINITE = std::numeric_limits<float>::infinity();

bool finite(Vector2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

} // namespace

TEST_CASE_FIXTURE(Fixture, "a delta that is not a frame is no time, said once") {
    // A NaN was every position a behavior wrote from then on, a negative
    // delta ran a Tween backwards past `from`, an infinity a Timer forever.
    for (const float delta : { NOT_A_NUMBER, -1.0f, INFINITE, -INFINITE }) {
        CAPTURE(delta);
        Loose o;
        rmp::behavior::Lifespan life{ .seconds = 1 };
        life._update(o, delta);
        CHECK(life.ours.elapsed == 0.0f);
        CHECK(o.alive());
    }
    CHECK(rmp::detail::report_count() == 1);
}

TEST_CASE_FIXTURE(Fixture,
                  "a behavior never writes a NaN its fields make into the object") {
    Loose o;
    o.velocity = { 3, 4 };
    hold(KEY_D);
    rmp::input::detail::begin_frame();
    rmp::behavior::TopDown walker{ .speed = NOT_A_NUMBER };
    walker._update(o, FRAME);
    CHECK(o.velocity.x == 3.0f); // left as it was
    CHECK(o.velocity.y == 4.0f);
    // The field, by name, and the write that was not made.
    CHECK(rmp::detail::report_count() == 2);
}

TEST_CASE_FIXTURE(Fixture, "TopDown: a stick at infinity is no input") {
    // input::vector() of an infinite axis is a NaN, which was the facing, the
    // velocity and then sector()'s cast.
    fake.devices.axes[GAMEPAD_AXIS_LEFT_X] = INFINITE;
    rmp::input::detail::begin_frame();
    Loose o;
    rmp::behavior::TopDown walker;
    walker._update(o, FRAME);
    CHECK(finite(o.velocity));
    CHECK(walker.direction().x == 1.0f); // the facing it started with
    CHECK(walker.sector() == 0);
}

TEST_CASE_FIXTURE(Fixture, "TopDown: a facing that is no direction is east") {
    rmp::behavior::TopDown walker;
    walker.ours.facing = { NOT_A_NUMBER, NOT_A_NUMBER };
    CHECK(walker.sector() == 0); // a cast of NaN to int, before
    walker.ours.facing = { INFINITE, -INFINITE };
    CHECK(walker.sector() == 1); // north-east, as atan2 has it
}

TEST_CASE_FIXTURE(Fixture,
                  "Platformer and Runner: air_jumps = INT_MAX is not an overflow") {
    // `jumps_used < air_jumps + 1` was undefined for it, on the first update.
    Loose o;
    rmp::input::detail::begin_frame(); // the first frame has no edges
    hold(KEY_SPACE);
    rmp::input::detail::begin_frame();
    rmp::behavior::Platformer p{ .air_jumps = std::numeric_limits<int>::max() };
    p._update(o, FRAME);
    CHECK(p.ours.jumps_used == 2); // the ground's, spent by falling, and one in the air
    rmp::behavior::Runner r{ .air_jumps = std::numeric_limits<int>::max() };
    r._update(o, FRAME);
    CHECK(r.ours.jumps_used == 2);
}

TEST_CASE_FIXTURE(Fixture, "Platformer: a jump_buffer longer than 'never' jumps nobody") {
    // The buffer measured from 1000, what `ours` starts at: at 2000 a jump
    // nobody pressed fired on every frame, and a coyote_time as long never
    // closed, so the jumps never ran out.
    Loose o;
    rmp::behavior::Platformer p{ .jump = 700, .coyote_time = 2000, .jump_buffer = 2000 };
    for (int i = 0; i < 10; ++i) {
        rmp::input::detail::begin_frame();
        p._update(o, FRAME);
    }
    CHECK(p.ours.jumps_used == 1); // the ground's, spent by falling
    CHECK(o.velocity.y > 0); // falling, not jumping
}

TEST_CASE_FIXTURE(Fixture, "Runner: ducking is the duck action held") {
    Loose o;
    rmp::behavior::Runner r{ .duck_action = "move_down" };
    rmp::input::detail::begin_frame();
    r._update(o, FRAME);
    CHECK_FALSE(r.ducking());
    hold(KEY_S);
    rmp::input::detail::begin_frame();
    r._update(o, FRAME);
    CHECK(r.ducking());
    rmp::behavior::Runner never;
    never._update(o, FRAME);
    CHECK_FALSE(never.ducking()); // no duck_action: it never ducks
}

TEST_CASE_FIXTURE(Fixture, "Ball: a pace that would overflow stops climbing") {
    Loose ball;
    ball.shape = rmp::rect({ 10, 10 });
    ball.velocity = { 1, 0 };
    Loose wall;
    wall.shape = rmp::rect({ 10, 100 });
    wall.position = { 8, 0 };
    wall.solid = true;
    rmp::behavior::Ball b{ .speed = std::numeric_limits<float>::max(), .speed_up = 2 };
    b._collision(ball, wall);
    CHECK(b.speed == std::numeric_limits<float>::max());
    b._update(ball, FRAME);
    CHECK(finite(ball.velocity));
}

TEST_CASE_FIXTURE(Fixture,
                  "Tween: a frame longer than a looping pass stays between the ends") {
    // One pass was taken back however many the frame covered: a 0.03 s
    // tween on a 0.1 s frame went to 233% of the way, past `to`.
    Loose o;
    rmp::behavior::Tween loop{ .property = rmp::behavior::Property::POSITION_X,
                               .from = 0,
                               .to = 100,
                               .seconds = 0.03f,
                               .ease = rmp::behavior::Ease::LINEAR,
                               .loop = true };
    loop._update(o, 0.1f);
    CHECK(o.position.x == doctest::Approx(100.0 / 3).epsilon(1e-3));
    // Three passes and a third: back, forth, back -- a third of the way back.
    rmp::behavior::Tween pong = loop;
    pong.loop = false;
    pong.ping_pong = true;
    pong.restart();
    pong._update(o, 0.1f);
    CHECK(pong.ours.back);
    CHECK(o.position.x == doctest::Approx(200.0 / 3).epsilon(1e-3));
}

TEST_CASE_FIXTURE(Fixture,
                  "Tween: one of no time is finished on `to`, whatever it repeats") {
    Loose o;
    rmp::behavior::Tween instant{ .property = rmp::behavior::Property::ROTATION,
                                  .to = 90,
                                  .seconds = 0,
                                  .ping_pong = true };
    instant._update(o, FRAME);
    CHECK(o.rotation == 90.0f);
    CHECK(instant.finished());
}

TEST_CASE_FIXTURE(Fixture,
                  "Tween: restart() starts it again from `from`, going forwards") {
    Loose o;
    rmp::behavior::Tween t{ .property = rmp::behavior::Property::POSITION_Y,
                            .from = 10,
                            .to = 20,
                            .seconds = 1,
                            .ease = rmp::behavior::Ease::LINEAR,
                            .ping_pong = true };
    t._update(o, 1.5f); // into the way back
    CHECK(t.ours.back);
    t.restart();
    CHECK_FALSE(t.ours.back);
    CHECK(t.ours.elapsed == 0.0f);
    t._update(o, 0);
    CHECK(o.position.y == 10.0f);
}

TEST_CASE_FIXTURE(Fixture,
                  "GridSnap: a cell too small to count the position in leaves it") {
    // (1000 / 1e-40) is past float's range: the snap was an infinity.
    Loose o;
    o.position = { 1000, 1000 };
    rmp::behavior::GridSnap snap{ .cell = { 1e-40f, 32 } };
    snap._late_update(o, FRAME);
    CHECK(o.position.x == 1000.0f);
    CHECK(o.position.y == 992.0f); // 31 cells of 32: the other axis snaps
    CHECK(rmp::detail::report_count() == 1);
}

TEST_CASE_FIXTURE(Fixture, "Parallax: any shift and any screen tile in bounded copies") {
    using rmp::behavior::detail::MAX_PARALLAX_COPIES;
    using rmp::behavior::detail::parallax_tiling;
    // A NaN shift is none (a cast of NaN before); a screen of infinite width,
    // or one a thousand million textures wide, is the most a frame draws.
    const auto none = parallax_tiling(NOT_A_NUMBER, 64, 1280);
    CHECK(none.offset == 0.0f);
    CHECK(none.copies == 21);
    CHECK(parallax_tiling(0, 64, INFINITE).copies == MAX_PARALLAX_COPIES);
    CHECK(parallax_tiling(0, 1, 1e12f).copies == MAX_PARALLAX_COPIES);
    CHECK(parallax_tiling(0, 64, NOT_A_NUMBER).copies == 1);
    CHECK(parallax_tiling(0, INFINITE, 1280).copies == 0);
    // A hair past a whole number of widths is the same picture as none.
    CHECK(parallax_tiling(1e-10f, 100, 1280).offset == 0.0f);
}

TEST_CASE_FIXTURE(Fixture, "Spawner: a jitter that is not a finite number is none") {
    rmp::behavior::Spawner s{ .jitter = INFINITE };
    CHECK(s.jitter_amount() == 0.0f); // rmp::random::range of an infinity, before
    s.jitter = NOT_A_NUMBER;
    CHECK(s.jitter_amount() == 0.0f);
    s.jitter = std::numeric_limits<float>::max();
    for (int i = 0; i < 16; ++i) CHECK(std::isfinite(s.jitter_amount()));
}

TEST_CASE_FIXTURE(Fixture, "Timer: an interval below what it has counted still ends") {
    // Subtracting one interval at a time: in a float, 1e8 - 1 is 1e8, and
    // that loop never ended. At most MAX_TIMEOUTS calls a frame, the rest
    // of the time dropped, said once.
    Loose o;
    static int calls = 0;
    calls = 0;
    rmp::behavior::Timer t{ .seconds = 1, .on_timeout = [](rmp::Object &) { ++calls; } };
    t._update(o, 1e8f);
    CHECK(calls == rmp::behavior::detail::MAX_TIMEOUTS);
    CHECK(t.ours.elapsed >= 0.0f);
    CHECK(t.ours.elapsed < 1.0f);
    CHECK(rmp::detail::report_count() == 1);
    // Fewer than that owed is every one of them.
    calls = 0;
    rmp::behavior::Timer fast{ .seconds = 0.001f,
                               .on_timeout = [](rmp::Object &) { ++calls; } };
    fast._update(o, 0.5f);
    CHECK(calls >= 499);
    CHECK(calls <= 500);
}

TEST_CASE_FIXTURE(Fixture,
                  "Timer: one that does not repeat is spent before its one call") {
    // A call that destroyed the object returned before the line that spent
    // the timer, and a second update called it again.
    Loose o;
    static int calls = 0;
    calls = 0;
    rmp::behavior::Timer once{ .seconds = 1,
                               .repeat = false,
                               .on_timeout = [](rmp::Object &self) {
                                   ++calls;
                                   self.destroy();
                               } };
    once._update(o, 1);
    once._update(o, 1);
    CHECK(calls == 1);
    CHECK(once.ours.spent);
}

TEST_CASE_FIXTURE(Fixture, "Health: a window longer than int's range blinks, defined") {
    // The blink cast phase * 2 to an int: undefined from about 1e8 seconds at
    // 12 Hz -- an invulnerability meant as "forever" -- and for a NaN; a
    // negative delta grew the window into that range by itself.
    Loose o;
    rmp::behavior::Health h{ .invulnerable_for = 1e9f };
    h._ready(o);
    CHECK(h.damage(o, 1));
    h._update(o, FRAME);
    h._update(o, -100);
    CHECK(h.ours.invulnerable_left <= 1e9f);
    h._end(o);
    CHECK(o.visible);
}

TEST_CASE_FIXTURE(Fixture, "Health: heal(INT_MAX) is back to full, not an overflow") {
    Loose o;
    rmp::behavior::Health h{ .hp = 3, .invulnerable_for = 0 };
    h._ready(o);
    h.damage(o, 2);
    h.heal(std::numeric_limits<int>::max());
    CHECK(h.hp == 3);
}

TEST_CASE_FIXTURE(Fixture,
                  "Health: damage below INT_MIN from an hp a game set is death") {
    Loose o;
    static int deaths = 0;
    deaths = 0;
    rmp::behavior::Health h{ .hp = -35,
                             .destroy_on_death = false,
                             .on_death = [](rmp::Object &) { ++deaths; } };
    h._ready(o);
    CHECK(h.damage(o, std::numeric_limits<int>::max() - 1));
    CHECK(h.hp == 0);
    CHECK(deaths == 1);
}
