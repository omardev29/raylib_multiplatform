// ===========================================================================
// rmp::Object — the life cycle, the integrator, the handles and the edges.
//
// Not one InitWindow in this file. Everything here is arithmetic and callbacks,
// which is the whole reason the storage lives behind object_internal.h: the
// only part that needs a GL context is the drawing itself, and the ORDER it
// draws in — which is the part with the bugs — comes out of draw_order()
// without a single raylib call.
//
// The storage is global, so every case starts from reset_for_tests(). A suite
// whose result depends on its own order is worse than no suite.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/object_internal.h"

#include <rmp/object.h>
#include <rmp/scene.h>

#include <string>
#include <vector>

namespace {

std::string g_log;

void note(const std::string &event) {
    if (!g_log.empty()) g_log += ' ';
    g_log += event;
}

// A scene that is never on the stack. spawn() only needs `this` as a key, so a
// local one is a complete world with no window and no navigation.
class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

// Writes down everything the framework does to it.
class Probe : public rmp::Object {
public:
    std::string name = "?";
    int updates = 0;
    int ends = 0;
    int draws = 0;

    void _ready() override { note(name + ".ready"); }
    void _update(float delta) override {
        (void)delta;
        updates++;
        note(name + ".update");
    }
    void _draw() override {
        draws++;
        note(name + ".draw");
    }
    void _end() override {
        ends++;
        note(name + ".end");
    }
};

struct Fixture {
    Fixture() {
        rmp::objects::detail::reset_for_tests();
        g_log.clear();
    }
    ~Fixture() { rmp::objects::detail::reset_for_tests(); }
};

// One frame of the object half: update, then the deferred release. The scene
// stack does exactly this, either side of the drawing.
void frame(rmp::Scene &scene, float delta = 1.0f) {
    rmp::objects::detail::update(scene, delta);
    rmp::objects::detail::collect();
}

constexpr float kEps = 0.0001f;

} // namespace

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------

TEST_CASE("rect() and circle() say what they make") {
    const rmp::Shape r = rmp::rect({ 12, 80 });
    CHECK(r.kind == rmp::ShapeKind::RECTANGLE);
    CHECK(r.size.x == doctest::Approx(12));
    CHECK(r.size.y == doctest::Approx(80));
    CHECK(r.radius == doctest::Approx(0));

    const rmp::Shape c = rmp::circle(6);
    CHECK(c.kind == rmp::ShapeKind::CIRCLE);
    CHECK(c.radius == doctest::Approx(6));
    CHECK(c.size.x == doctest::Approx(0));

    SUBCASE("and a default Shape draws nothing, which is what NONE means") {
        const rmp::Shape none;
        CHECK(none.kind == rmp::ShapeKind::NONE);
        CHECK(none.filled);
        CHECK(none.thickness == doctest::Approx(1));
    }
}

TEST_CASE("a Shape can be written as a designated initialiser in field order") {
    // C++20 requires declaration order and rejects any other, and this project
    // has been bitten by that twice. If someone reorders the struct, this stops
    // compiling, which is the point.
    const rmp::Shape s{ .kind = rmp::ShapeKind::RECTANGLE,
                        .size = { 4, 5 },
                        .radius = 0,
                        .offset = { 1, 2 },
                        .color = RED,
                        .filled = false,
                        .thickness = 3 };
    CHECK(s.offset.x == doctest::Approx(1));
    CHECK(s.thickness == doctest::Approx(3));
}

// ---------------------------------------------------------------------------
// spawn
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "spawn puts an object in the scene and runs _ready") {
    World world;
    auto &probe = world.spawn<Probe>({ .position = { 10, 20 } });
    probe.name = "a";

    CHECK(world.object_count() == 1);
    CHECK(probe.position.x == doctest::Approx(10));
    CHECK(probe.position.y == doctest::Approx(20));
    CHECK(probe.alive());
    CHECK(probe.scene() == &world);
    // _ready() ran before spawn returned, so it ran before the name was set.
    CHECK(g_log == "?.ready");
}

TEST_CASE_FIXTURE(Fixture, "spawn applies every option it was given") {
    World world;
    auto &object = world.spawn({ .position = { 1, 2 },
                                 .shape = rmp::circle(7),
                                 .velocity = { 3, 4 },
                                 .scale = { 2, 3 },
                                 .rotation = 45,
                                 .layer = 9,
                                 .visible = false,
                                 .edges = rmp::Edge::WRAP,
                                 .bounds = { 0, 0, 100, 100 } });

    CHECK(object.position.x == doctest::Approx(1));
    CHECK(object.velocity.y == doctest::Approx(4));
    CHECK(object.scale.x == doctest::Approx(2));
    CHECK(object.rotation == doctest::Approx(45));
    CHECK(object.layer == 9);
    CHECK_FALSE(object.visible);
    CHECK(object.edges == rmp::Edge::WRAP);
    CHECK(object.bounds.width == doctest::Approx(100));
    CHECK(object.shape.kind == rmp::ShapeKind::CIRCLE);
    CHECK(object.shape.radius == doctest::Approx(7));
}

TEST_CASE_FIXTURE(Fixture, "size is shorthand for a rectangle, and shape wins over it") {
    World world;

    SUBCASE("size alone makes a rectangle") {
        auto &o = world.spawn({ .size = { 12, 80 } });
        CHECK(o.shape.kind == rmp::ShapeKind::RECTANGLE);
        CHECK(o.shape.size.y == doctest::Approx(80));
    }
    SUBCASE("shape alone is used as given") {
        auto &o = world.spawn({ .shape = rmp::circle(6) });
        CHECK(o.shape.kind == rmp::ShapeKind::CIRCLE);
    }
    SUBCASE("both is not an error: the one that says what it means wins") {
        auto &o = world.spawn({ .size = { 12, 80 }, .shape = rmp::circle(6) });
        CHECK(o.shape.kind == rmp::ShapeKind::CIRCLE);
        CHECK(o.shape.radius == doctest::Approx(6));
    }
    SUBCASE("neither leaves an object with no shape, which draws nothing") {
        auto &o = world.spawn();
        CHECK(o.shape.kind == rmp::ShapeKind::NONE);
    }
}

TEST_CASE_FIXTURE(Fixture, "spawn<T> keeps the derived type and its overrides") {
    World world;
    auto &probe = world.spawn<Probe>();
    probe.name = "p";
    frame(world);
    CHECK(probe.updates == 1);
    CHECK(g_log == "?.ready p.update");
}

TEST_CASE_FIXTURE(Fixture, "two scenes do not see each other's objects") {
    World a;
    World b;
    a.spawn();
    a.spawn();
    b.spawn();
    CHECK(a.object_count() == 2);
    CHECK(b.object_count() == 1);
    CHECK(rmp::objects::detail::live_count() == 3);
}

// ---------------------------------------------------------------------------
// destroy
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "destroy stops the object at once and frees it after the frame") {
    World world;
    auto &probe = world.spawn<Probe>();
    probe.name = "p";

    probe.destroy();
    CHECK_FALSE(probe.alive());
    CHECK(probe.ends == 1);
    // Gone from the count immediately: an object destroyed earlier in this
    // frame is not part of the scene any more, whatever the memory is doing.
    CHECK(world.object_count() == 0);
    // But still allocated, which is what makes the reference safe until the end
    // of the frame.
    CHECK(rmp::objects::detail::slot_count() == 1);

    rmp::objects::detail::collect();
    CHECK(world.object_count() == 0);
    CHECK(rmp::objects::detail::live_count() == 0);
    CHECK(g_log == "?.ready p.end");
}

TEST_CASE_FIXTURE(Fixture,
                  "a destroyed object is not updated, in the same frame or ever") {
    World world;
    auto &a = world.spawn<Probe>();
    a.name = "a";
    auto &b = world.spawn<Probe>();
    b.name = "b";

    a.destroy();
    frame(world);
    CHECK(a.ends == 1);
    CHECK(b.updates == 1);
    CHECK(g_log == "?.ready ?.ready a.end b.update");
}

TEST_CASE_FIXTURE(Fixture, "destroying twice is harmless and _end runs once") {
    World world;
    auto &probe = world.spawn<Probe>();
    probe.name = "p";
    probe.destroy();
    probe.destroy();
    probe.destroy();
    CHECK(probe.ends == 1);
    rmp::objects::detail::collect();
    CHECK(rmp::objects::detail::live_count() == 0);
}

TEST_CASE_FIXTURE(Fixture, "scene.destroy(object) is the same thing") {
    World world;
    auto &probe = world.spawn<Probe>();
    probe.name = "p";
    world.destroy(probe);
    CHECK_FALSE(probe.alive());
    CHECK(probe.ends == 1);
}

namespace {
// Destroys itself the first time it is updated. The classic way to find out
// whether a container is being walked while it is written to.
class SelfDestruct : public rmp::Object {
public:
    int updates = 0;
    void _update(float delta) override {
        (void)delta;
        updates++;
        destroy();
    }
};
} // namespace

TEST_CASE_FIXTURE(Fixture, "an object may destroy itself from its own _update") {
    World world;
    auto &object = world.spawn<SelfDestruct>();
    frame(world);
    CHECK(object.updates == 1);
    CHECK(rmp::objects::detail::live_count() == 0);
    frame(world);
    CHECK(rmp::objects::detail::live_count() == 0);
}

namespace {
class Spawner : public rmp::Object {
public:
    int made = 0;
    void _update(float delta) override {
        (void)delta;
        if (made < 3) {
            scene()->spawn();
            made++;
        }
    }
};
} // namespace

TEST_CASE_FIXTURE(Fixture, "an object spawned during _update waits for the next frame") {
    World world;
    auto &spawner = world.spawn<Spawner>();

    frame(world);
    CHECK(spawner.made == 1);
    // Two objects now, and the new one has NOT been updated: it got its
    // _ready() and its first _update() is next frame.
    CHECK(world.object_count() == 2);

    frame(world);
    CHECK(spawner.made == 2);
    CHECK(world.object_count() == 3);
}

// ---------------------------------------------------------------------------
// Handles — the part that matters most
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "a default handle points at nothing") {
    const rmp::Handle<rmp::Object> handle;
    CHECK_FALSE(static_cast<bool>(handle));
    CHECK(handle.get() == nullptr);
}

TEST_CASE_FIXTURE(Fixture, "a handle to a live object resolves to it") {
    World world;
    auto &object = world.spawn();
    const rmp::Handle<rmp::Object> handle = object.handle();

    REQUIRE(static_cast<bool>(handle));
    CHECK(handle.get() == &object);
    CHECK(&*handle == &object);
    object.position = { 5, 6 };
    CHECK(handle->position.x == doctest::Approx(5));
}

TEST_CASE_FIXTURE(Fixture, "a handle goes false the instant the object is destroyed") {
    World world;
    auto &object = world.spawn();
    const rmp::Handle<rmp::Object> handle = object.handle();
    REQUIRE(static_cast<bool>(handle));

    object.destroy();
    // BEFORE collect(): the memory is still there and the reference is still
    // usable, but the handle has to say no. Anything else lets `if (target)`
    // pass and hand back something that has already had its _end() run.
    CHECK_FALSE(static_cast<bool>(handle));
    CHECK(handle.get() == nullptr);

    rmp::objects::detail::collect();
    CHECK_FALSE(static_cast<bool>(handle));
}

TEST_CASE_FIXTURE(Fixture, "a handle STAYS dead after its slot is reused") {
    // The test the whole design exists for. Store an index alone and this is
    // the bug: the slot comes back as somebody else and the old handle starts
    // pointing at a stranger.
    World world;
    auto &first = world.spawn({ .position = { 1, 1 } });
    const rmp::Handle<rmp::Object> stale = first.handle();
    REQUIRE(static_cast<bool>(stale));

    first.destroy();
    rmp::objects::detail::collect();
    REQUIRE_FALSE(static_cast<bool>(stale));
    const int slots_before = rmp::objects::detail::slot_count();

    auto &second = world.spawn({ .position = { 2, 2 } });
    // The slot really was reused -- otherwise this test proves nothing, and it
    // would still pass.
    REQUIRE(rmp::objects::detail::slot_count() == slots_before);
    REQUIRE(static_cast<bool>(second.handle()));

    CHECK_FALSE(static_cast<bool>(stale));
    CHECK(stale.get() == nullptr);
    CHECK_FALSE(stale == second.handle());
}

TEST_CASE_FIXTURE(Fixture,
                  "a handle survives many rounds of reuse without ever resurrecting") {
    World world;
    std::vector<rmp::Handle<rmp::Object>> dead;

    for (int round = 0; round < 50; round++) {
        auto &object = world.spawn();
        dead.push_back(object.handle());
        object.destroy();
        rmp::objects::detail::collect();
    }
    // One slot, reused fifty times.
    CHECK(rmp::objects::detail::slot_count() == 1);
    for (const auto &handle : dead) {
        CHECK_FALSE(static_cast<bool>(handle));
    }

    // And the live one is fine while every dead one is not.
    auto &live = world.spawn();
    CHECK(static_cast<bool>(live.handle()));
    for (const auto &handle : dead) {
        CHECK_FALSE(static_cast<bool>(handle));
    }
}

TEST_CASE_FIXTURE(Fixture, "handles from different objects are never equal") {
    World world;
    auto &a = world.spawn();
    auto &b = world.spawn();
    CHECK_FALSE(a.handle() == b.handle());
    CHECK(a.handle() == a.handle());
}

TEST_CASE_FIXTURE(Fixture, "a handle to a typed object comes back typed") {
    World world;
    auto &probe = world.spawn<Probe>();
    probe.name = "p";

    const rmp::Handle<Probe> typed = probe.handle<Probe>();
    REQUIRE(static_cast<bool>(typed));
    CHECK(typed->name == "p");
    CHECK(typed.get() == &probe);

    SUBCASE("and it dies with the object, like any other") {
        probe.destroy();
        CHECK_FALSE(static_cast<bool>(typed));
    }
}

TEST_CASE_FIXTURE(Fixture, "every handle into a scene dies with the scene") {
    rmp::Handle<rmp::Object> handle;
    {
        World world;
        auto &object = world.spawn<Probe>();
        handle = object.handle();
        REQUIRE(static_cast<bool>(handle));
    } // ~World calls release_scene
    CHECK_FALSE(static_cast<bool>(handle));
    CHECK(rmp::objects::detail::live_count() == 0);
}

TEST_CASE_FIXTURE(Fixture, "releasing a scene runs every _end") {
    {
        World world;
        auto &a = world.spawn<Probe>();
        a.name = "a";
        auto &b = world.spawn<Probe>();
        b.name = "b";
        g_log.clear();
    }
    CHECK(g_log == "a.end b.end");
}

// ---------------------------------------------------------------------------
// The integrator
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "velocity moves the position by velocity times delta") {
    World world;
    auto &object = world.spawn({ .position = { 0, 0 }, .velocity = { 100, -50 } });

    rmp::objects::detail::update(world, 0.5f);
    CHECK(object.position.x == doctest::Approx(50));
    CHECK(object.position.y == doctest::Approx(-25));

    rmp::objects::detail::update(world, 0.5f);
    CHECK(object.position.x == doctest::Approx(100));
}

TEST_CASE_FIXTURE(Fixture, "a zero delta moves nothing") {
    World world;
    auto &object = world.spawn({ .position = { 7, 7 }, .velocity = { 100, 100 } });
    rmp::objects::detail::update(world, 0.0f);
    CHECK(object.position.x == doctest::Approx(7));
    CHECK(object.position.y == doctest::Approx(7));
}

TEST_CASE_FIXTURE(Fixture,
                  "gravity_scale is 0 by default, so nothing falls until it asks") {
    World world;
    CHECK(world.gravity.y == doctest::Approx(980));
    auto &object = world.spawn({ .position = { 0, 0 } });
    CHECK(object.gravity_scale == doctest::Approx(0));

    rmp::objects::detail::update(world, 1.0f);
    CHECK(object.position.y == doctest::Approx(0));
    CHECK(object.velocity.y == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture,
                  "gravity_scale scales the scene's gravity, in both directions") {
    World world;
    world.gravity = { 0, 100 };

    SUBCASE("1 is the scene's own") {
        auto &o = world.spawn();
        o.gravity_scale = 1;
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.velocity.y == doctest::Approx(100));
    }
    SUBCASE("a half is half") {
        auto &o = world.spawn();
        o.gravity_scale = 0.5f;
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.velocity.y == doctest::Approx(50));
    }
    SUBCASE("negative floats upwards") {
        auto &o = world.spawn();
        o.gravity_scale = -0.2f;
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.velocity.y == doctest::Approx(-20));
    }
    SUBCASE("and gravity is a vector, so sideways works") {
        world.gravity = { 40, 0 };
        auto &o = world.spawn();
        o.gravity_scale = 1;
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.velocity.x == doctest::Approx(40));
        CHECK(o.velocity.y == doctest::Approx(0));
    }
}

TEST_CASE_FIXTURE(Fixture,
                  "apply_impulse changes the velocity at once and divides by mass") {
    World world;
    auto &object = world.spawn();
    object.mass = 2;
    object.apply_impulse({ 10, 0 });
    // No update needed: that is what instant means.
    CHECK(object.velocity.x == doctest::Approx(5));

    object.apply_impulse({ 10, 0 });
    CHECK(object.velocity.x == doctest::Approx(10));
}

TEST_CASE_FIXTURE(Fixture, "apply_force is spent over the frame and divided by mass") {
    World world;
    auto &object = world.spawn();
    object.mass = 2;
    object.apply_force({ 10, 0 });
    CHECK(object.velocity.x == doctest::Approx(0)); // nothing until the frame runs

    rmp::objects::detail::update(world, 0.5f);
    CHECK(object.velocity.x == doctest::Approx(10.0f / 2 * 0.5f));
}

TEST_CASE_FIXTURE(Fixture, "a force applied once does not keep pushing") {
    // The accumulator has to be cleared every frame. If it is not, a single
    // apply_force turns into constant thrust, which is a bug that looks like
    // "my game speeds up" and takes an afternoon to find.
    World world;
    auto &object = world.spawn();
    object.apply_force({ 100, 0 });

    rmp::objects::detail::update(world, 1.0f);
    const float after_one = object.velocity.x;
    CHECK(after_one == doctest::Approx(100));

    rmp::objects::detail::update(world, 1.0f);
    CHECK(object.velocity.x == doctest::Approx(after_one));
}

TEST_CASE_FIXTURE(Fixture, "forces accumulate within one frame") {
    World world;
    auto &object = world.spawn();
    object.apply_force({ 10, 0 });
    object.apply_force({ 5, 0 });
    object.apply_force({ 0, 20 });
    rmp::objects::detail::update(world, 1.0f);
    CHECK(object.velocity.x == doctest::Approx(15));
    CHECK(object.velocity.y == doctest::Approx(20));
}

TEST_CASE_FIXTURE(Fixture, "gravity and apply_force land in the same accumulator") {
    // Pushing up against gravity does what it would in the world, because there
    // is no separate path for gravity.
    World world;
    world.gravity = { 0, 100 };
    auto &object = world.spawn();
    object.gravity_scale = 1;
    object.apply_force({ 0, -100 });
    rmp::objects::detail::update(world, 1.0f);
    CHECK(object.velocity.y == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture, "a mass of zero is treated as one instead of dividing by it") {
    World world;
    auto &object = world.spawn();
    object.mass = 0;
    object.apply_impulse({ 10, 0 });
    CHECK(object.velocity.x == doctest::Approx(10));
    object.apply_force({ 10, 0 });
    rmp::objects::detail::update(world, 1.0f);
    CHECK(object.velocity.x == doctest::Approx(20));
}

// ---------------------------------------------------------------------------
// world_bounds
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "world_bounds is centred on the position") {
    World world;

    SUBCASE("a rectangle") {
        auto &o =
            world.spawn({ .position = { 100, 100 }, .shape = rmp::rect({ 20, 10 }) });
        const Rectangle b = o.world_bounds();
        CHECK(b.x == doctest::Approx(90));
        CHECK(b.y == doctest::Approx(95));
        CHECK(b.width == doctest::Approx(20));
        CHECK(b.height == doctest::Approx(10));
    }
    SUBCASE("a circle is its diameter") {
        auto &o = world.spawn({ .position = { 100, 100 }, .shape = rmp::circle(5) });
        const Rectangle b = o.world_bounds();
        CHECK(b.x == doctest::Approx(95));
        CHECK(b.width == doctest::Approx(10));
        CHECK(b.height == doctest::Approx(10));
    }
    SUBCASE("scale multiplies it") {
        auto &o = world.spawn(
            { .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }), .scale = { 2, 3 } });
        const Rectangle b = o.world_bounds();
        CHECK(b.width == doctest::Approx(20));
        CHECK(b.height == doctest::Approx(30));
        CHECK(b.x == doctest::Approx(-10));
    }
    SUBCASE("offset moves it without moving the object") {
        auto &o = world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
        o.shape.offset = { 5, 0 };
        CHECK(o.world_bounds().x == doctest::Approx(0));
        CHECK(o.position.x == doctest::Approx(0));
    }
    SUBCASE("no shape and no sprite is a point, and a point can still be clamped") {
        auto &o = world.spawn({ .position = { 3, 4 } });
        const Rectangle b = o.world_bounds();
        CHECK(b.x == doctest::Approx(3));
        CHECK(b.width == doctest::Approx(0));
    }
}

// ---------------------------------------------------------------------------
// Edges. Every mode, and every one of them on BOTH axes and BOTH sides,
// because a fix applied to x and not to y is the likeliest mistake in the file.
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "Edge::NONE lets things leave, which is the default") {
    World world;
    auto &o = world.spawn({ .position = { 10, 10 },
                            .shape = rmp::rect({ 10, 10 }),
                            .velocity = { -1000, -1000 },
                            .bounds = { 0, 0, 100, 100 } });
    CHECK(o.edges == rmp::Edge::NONE);
    rmp::objects::detail::update(world, 1.0f);
    CHECK(o.position.x == doctest::Approx(-990));
    CHECK(o.position.y == doctest::Approx(-990));
}

TEST_CASE_FIXTURE(Fixture, "Edge::CLAMP stops the object at each of the four sides") {
    World world;
    const Rectangle area{ 0, 0, 100, 100 };

    SUBCASE("left") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { -1000, 0 },
                                .edges = rmp::Edge::CLAMP,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.world_bounds().x == doctest::Approx(0));
        CHECK(o.position.x == doctest::Approx(5));
    }
    SUBCASE("right") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { 1000, 0 },
                                .edges = rmp::Edge::CLAMP,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.position.x == doctest::Approx(95));
    }
    SUBCASE("top") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { 0, -1000 },
                                .edges = rmp::Edge::CLAMP,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.position.y == doctest::Approx(5));
    }
    SUBCASE("bottom") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { 0, 1000 },
                                .edges = rmp::Edge::CLAMP,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.position.y == doctest::Approx(95));
    }
    SUBCASE("clamping does not touch the velocity: a held key keeps pressing") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { -1000, 0 },
                                .edges = rmp::Edge::CLAMP,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.velocity.x == doctest::Approx(-1000));
    }
}

TEST_CASE_FIXTURE(Fixture, "Edge::BOUNCE flips the velocity on the axis that hit") {
    World world;
    const Rectangle area{ 0, 0, 100, 100 };

    SUBCASE("left, and only the x") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { -1000, 7 },
                                .edges = rmp::Edge::BOUNCE,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.velocity.x == doctest::Approx(1000));
        CHECK(o.velocity.y == doctest::Approx(7));
        CHECK(o.world_bounds().x == doctest::Approx(0));
    }
    SUBCASE("right") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { 1000, 0 },
                                .edges = rmp::Edge::BOUNCE,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.velocity.x == doctest::Approx(-1000));
    }
    SUBCASE("top") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { 0, -1000 },
                                .edges = rmp::Edge::BOUNCE,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.velocity.y == doctest::Approx(1000));
    }
    SUBCASE("bottom") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { 0, 1000 },
                                .edges = rmp::Edge::BOUNCE,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.velocity.y == doctest::Approx(-1000));
    }
    SUBCASE("a corner flips both") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { -1000, -1000 },
                                .edges = rmp::Edge::BOUNCE,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.velocity.x == doctest::Approx(1000));
        CHECK(o.velocity.y == doctest::Approx(1000));
    }
    SUBCASE("an object already at the wall and moving INWARDS is left alone") {
        // Otherwise it flips every frame and vibrates against the wall instead
        // of leaving it. This is the bug the "only when pointing out" rule is
        // there to prevent, and it is invisible without a test.
        auto &o = world.spawn({ .position = { 5, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { 10, 0 },
                                .edges = rmp::Edge::BOUNCE,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.velocity.x == doctest::Approx(10));
    }
    SUBCASE("a ball bounced twice ends up back where it started") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::circle(5),
                                .velocity = { 60, 0 },
                                .edges = rmp::Edge::BOUNCE,
                                .bounds = area });
        for (int i = 0; i < 20; i++) rmp::objects::detail::update(world, 1.0f);
        // It never left, whatever else it did.
        const Rectangle b = o.world_bounds();
        CHECK(b.x >= -kEps);
        CHECK(b.x + b.width <= 100 + kEps);
    }
}

TEST_CASE_FIXTURE(Fixture, "Edge::WRAP only fires once the object is COMPLETELY past") {
    World world;
    const Rectangle area{ 0, 0, 100, 100 };

    SUBCASE("touching the edge does not teleport") {
        auto &o = world.spawn({ .position = { 0, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .edges = rmp::Edge::WRAP,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.position.x == doctest::Approx(0)); // half in, half out: stays
    }
    SUBCASE("fully past the left comes back JUST OUTSIDE the right") {
        // Just outside, not somewhere inside: it slides back in the way it
        // slid out. Shifting by the width of the world put it back well within
        // the view, which reads as a jump, and only an exact assertion catches
        // that -- `position.x > 100` would have passed either way.
        auto &o = world.spawn({ .position = { -20, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .edges = rmp::Edge::WRAP,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(o.world_bounds().x == doctest::Approx(100)); // left edge on the right wall
        CHECK(o.position.x == doctest::Approx(105));
    }
    SUBCASE("fully past the right comes back JUST OUTSIDE the left") {
        auto &o = world.spawn({ .position = { 120, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .edges = rmp::Edge::WRAP,
                                .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        const Rectangle b = o.world_bounds();
        CHECK(b.x + b.width == doctest::Approx(0)); // right edge on the left wall
        CHECK(o.position.x == doctest::Approx(-5));
    }
    SUBCASE("and it keeps going round without drifting") {
        auto &o = world.spawn({ .position = { 50, 50 },
                                .shape = rmp::rect({ 10, 10 }),
                                .velocity = { 30, 0 },
                                .edges = rmp::Edge::WRAP,
                                .bounds = area });
        // Six laps at 30 a frame. The object must still be a sane distance
        // from the world, whatever it did in between.
        for (int i = 0; i < 120; i++) {
            rmp::objects::detail::update(world, 1.0f);
            const Rectangle b = o.world_bounds();
            CHECK(b.x + b.width >= 0 - kEps);
            CHECK(b.x <= 100 + kEps);
        }
    }
    SUBCASE("and the same on the vertical") {
        auto &top = world.spawn({ .position = { 50, -20 },
                                  .shape = rmp::rect({ 10, 10 }),
                                  .edges = rmp::Edge::WRAP,
                                  .bounds = area });
        auto &bottom = world.spawn({ .position = { 50, 120 },
                                     .shape = rmp::rect({ 10, 10 }),
                                     .edges = rmp::Edge::WRAP,
                                     .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(top.position.y == doctest::Approx(105));
        CHECK(bottom.position.y == doctest::Approx(-5));
    }
}

TEST_CASE_FIXTURE(Fixture, "Edge::DESTROY waits until the object is completely outside") {
    World world;
    const Rectangle area{ 0, 0, 100, 100 };

    SUBCASE("a bullet born on the edge is not killed at birth") {
        // The specific bug this rule exists for: a muzzle on the boundary.
        auto &bullet = world.spawn({ .position = { 0, 50 },
                                     .shape = rmp::rect({ 10, 10 }),
                                     .edges = rmp::Edge::DESTROY,
                                     .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK(bullet.alive());
    }
    SUBCASE("and it dies once it is all the way out") {
        auto &bullet = world.spawn({ .position = { 90, 50 },
                                     .shape = rmp::rect({ 10, 10 }),
                                     .velocity = { 100, 0 },
                                     .edges = rmp::Edge::DESTROY,
                                     .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK_FALSE(bullet.alive());
        rmp::objects::detail::collect();
        CHECK(world.object_count() == 0);
    }
    SUBCASE("on every side") {
        auto &left = world.spawn({ .position = { -50, 50 },
                                   .shape = rmp::rect({ 10, 10 }),
                                   .edges = rmp::Edge::DESTROY,
                                   .bounds = area });
        auto &up = world.spawn({ .position = { 50, -50 },
                                 .shape = rmp::rect({ 10, 10 }),
                                 .edges = rmp::Edge::DESTROY,
                                 .bounds = area });
        auto &down = world.spawn({ .position = { 50, 150 },
                                   .shape = rmp::rect({ 10, 10 }),
                                   .edges = rmp::Edge::DESTROY,
                                   .bounds = area });
        rmp::objects::detail::update(world, 1.0f);
        CHECK_FALSE(left.alive());
        CHECK_FALSE(up.alive());
        CHECK_FALSE(down.alive());
    }
}

TEST_CASE_FIXTURE(Fixture,
                  "empty bounds mean the view, which headless is the design size") {
    World world;
    auto &o = world.spawn({ .position = { 50, 50 },
                            .shape = rmp::rect({ 10, 10 }),
                            .velocity = { -1000, 0 },
                            .edges = rmp::Edge::CLAMP });
    CHECK(o.bounds.width == doctest::Approx(0));
    rmp::objects::detail::update(world, 1.0f);
    CHECK(o.world_bounds().x == doctest::Approx(0));

    SUBCASE("and the far side is the design width") {
        auto &right = world.spawn({ .position = { 50, 50 },
                                    .shape = rmp::rect({ 10, 10 }),
                                    .velocity = { 100000, 0 },
                                    .edges = rmp::Edge::CLAMP });
        rmp::objects::detail::update(world, 1.0f);
        const Rectangle b = right.world_bounds();
        CHECK(b.x + b.width == doctest::Approx(static_cast<float>(APP_WINDOW_WIDTH)));
    }
}

TEST_CASE_FIXTURE(Fixture, "the edges run after the integration, not before") {
    // If they ran first, an object would be clamped where it was and then move
    // out anyway, and the clamp would look like it works until something moves
    // fast enough. One frame at high speed is the whole test.
    World world;
    auto &o = world.spawn({ .position = { 50, 50 },
                            .shape = rmp::rect({ 10, 10 }),
                            .velocity = { 1000, 0 },
                            .edges = rmp::Edge::CLAMP,
                            .bounds = { 0, 0, 100, 100 } });
    rmp::objects::detail::update(world, 1.0f);
    CHECK(o.position.x == doctest::Approx(95));
}

// ---------------------------------------------------------------------------
// Draw order
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "the draw order is layer first, then creation order") {
    World world;
    auto &a = world.spawn({ .layer = 1 });
    auto &b = world.spawn({ .layer = 0 });
    auto &c = world.spawn({ .layer = 1 });
    auto &d = world.spawn({ .layer = -5 });

    const std::vector<rmp::Object *> &order = rmp::objects::detail::draw_order(world);
    REQUIRE(order.size() == 4);
    CHECK(order[0] == &d);
    CHECK(order[1] == &b);
    // a before c: same layer, and a was made first.
    CHECK(order[2] == &a);
    CHECK(order[3] == &c);
}

TEST_CASE_FIXTURE(Fixture, "invisible and destroyed objects are not drawn") {
    World world;
    auto &shown = world.spawn();
    auto &hidden = world.spawn({ .visible = false });
    auto &dead = world.spawn();
    dead.destroy();

    const std::vector<rmp::Object *> &order = rmp::objects::detail::draw_order(world);
    REQUIRE(order.size() == 1);
    CHECK(order[0] == &shown);
    CHECK_FALSE(hidden.visible);
}

TEST_CASE_FIXTURE(Fixture, "changing the layer changes the order next frame") {
    World world;
    auto &a = world.spawn({ .layer = 0 });
    auto &b = world.spawn({ .layer = 1 });
    CHECK(rmp::objects::detail::draw_order(world)[0] == &a);

    a.layer = 5;
    CHECK(rmp::objects::detail::draw_order(world)[0] == &b);
}

TEST_CASE_FIXTURE(Fixture, "a scene with no objects has an empty draw order") {
    World world;
    CHECK(rmp::objects::detail::draw_order(world).empty());
}

// ---------------------------------------------------------------------------
// The storage itself
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "freed slots are reused instead of growing the storage") {
    World world;
    for (int i = 0; i < 10; i++) {
        auto &o = world.spawn();
        o.destroy();
        rmp::objects::detail::collect();
    }
    CHECK(rmp::objects::detail::slot_count() == 1);
    CHECK(rmp::objects::detail::live_count() == 0);
}

TEST_CASE_FIXTURE(Fixture, "a wave of objects dying together leaves nothing behind") {
    World world;
    std::vector<rmp::Handle<rmp::Object>> handles;
    for (int i = 0; i < 200; i++) {
        auto &o = world.spawn();
        handles.push_back(o.handle());
    }
    CHECK(world.object_count() == 200);

    for (const auto &handle : handles) {
        if (handle) handle->destroy();
    }
    CHECK(world.object_count() == 0);
    rmp::objects::detail::collect();
    CHECK(rmp::objects::detail::live_count() == 0);
    for (const auto &handle : handles) {
        CHECK_FALSE(static_cast<bool>(handle));
    }

    // And the slots all come back.
    for (int i = 0; i < 200; i++) world.spawn();
    CHECK(rmp::objects::detail::slot_count() == 200);
}

TEST_CASE_FIXTURE(Fixture, "collect on a frame with nothing to collect is not an error") {
    World world;
    world.spawn();
    rmp::objects::detail::collect();
    rmp::objects::detail::collect();
    CHECK(world.object_count() == 1);
}

TEST_CASE_FIXTURE(Fixture, "updating a scene that has no objects is not an error") {
    World world;
    rmp::objects::detail::update(world, 1.0f);
    CHECK(world.object_count() == 0);
}
