// ===========================================================================
// Collision, resolution, the swept test, and raycasting.
//
// Not one InitWindow in this file either. All of it is arithmetic.
//
// The two tests that matter most here are DIFFERENTIAL, and they are worth
// more than every hand-written case around them: the uniform grid has to
// produce exactly the same pair set as the O(n^2) loop it replaces, over a
// thousand random objects, and the grid-walking raycast has to agree with
// testing every object in the scene. A broad phase that quietly drops one pair
// in a thousand is a bug that surfaces as "sometimes the bullet goes through"
// a month later, and no case anybody writes by hand would ever catch it.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/object_internal.h"
#include "../src/rmp/ui/internal.h"

#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>

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

struct Fixture {
    Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_pointer_for_tests();
    }
    ~Fixture() { rmp::objects::detail::reset_for_tests(); }
};

// One frame of the object half, the way the scene stack drives it.
void frame(rmp::Scene &scene, float delta = 1.0f) {
    rmp::objects::detail::update(scene, delta);
    rmp::objects::detail::collide(scene);
    rmp::objects::detail::collect();
}

// Counts what happened to it, and can be told to die on contact.
class Probe : public rmp::Object {
public:
    int hits = 0;
    rmp::Object *last = nullptr;
    bool die_on_hit = false;

    void _collision(rmp::Object &other) override {
        hits++;
        last = &other;
        if (die_on_hit) destroy();
    }
};

// A deterministic generator, so a failure is reproducible. std::mt19937 would
// do too; this is four lines and has no <random> to include.
struct Rng {
    unsigned state = 0x2545F491u;
    unsigned next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    float range(float lo, float hi) {
        return lo + (static_cast<float>(next() % 100000u) / 100000.0f) * (hi - lo);
    }
};

using PairList = std::vector<std::pair<rmp::Object *, rmp::Object *>>;

PairList pairs_of(const rmp::Scene &scene, bool use_grid) {
    constexpr std::size_t kMax = 4096;
    std::vector<rmp::Object *> raw(kMax * 2, nullptr);
    const int n = rmp::objects::detail::touching_pairs_for_tests(
        scene, use_grid, raw.data(), static_cast<int>(kMax));
    PairList out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; i++) {
        rmp::Object *a = raw[static_cast<std::size_t>(i) * 2];
        rmp::Object *b = raw[static_cast<std::size_t>(i) * 2 + 1];
        out.emplace_back(a < b ? a : b, a < b ? b : a);
    }
    std::ranges::sort(out);
    return out;
}

constexpr float kEps = 0.001f;

} // namespace

// ---------------------------------------------------------------------------
// world_collider
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "the collider falls back to the shape, and overrides it") {
    World world;

    SUBCASE("no collider: the shape is what collides") {
        auto &o = world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 20, 10 }) });
        const Rectangle c = o.world_collider();
        CHECK(c.width == doctest::Approx(20));
        CHECK(c.height == doctest::Approx(10));
    }
    SUBCASE("a collider wins, which is the step up everybody needs") {
        // The sprite is the hair and the cape and the air around them; the
        // collider is the body.
        auto &o = world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 64, 64 }) });
        o.collider = rmp::rect({ 20, 44 });
        const Rectangle c = o.world_collider();
        CHECK(c.width == doctest::Approx(20));
        CHECK(c.height == doctest::Approx(44));
        // And the drawn shape is untouched.
        CHECK(o.shape.size.x == doctest::Approx(64));
    }
    SUBCASE("a circle collider is a box this wide, and still collides as a circle") {
        auto &o = world.spawn({ .position = { 0, 0 } });
        o.collider = rmp::circle(5);
        const Rectangle c = o.world_collider();
        CHECK(c.width == doctest::Approx(10));
        CHECK(c.x == doctest::Approx(-5));
    }
    SUBCASE("nothing at all is a point with no area") {
        auto &o = world.spawn({ .position = { 3, 4 } });
        const Rectangle c = o.world_collider();
        CHECK(c.width == doctest::Approx(0));
    }
}

// ---------------------------------------------------------------------------
// The three shape pairs
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "rectangle against rectangle") {
    World world;
    auto &a =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    auto &b =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });

    SUBCASE("overlapping") {
        b.position = { 5, 0 };
        frame(world);
        CHECK(a.hits == 1);
        CHECK(b.hits == 1);
    }
    SUBCASE("touching exactly edge to edge does not count") {
        // Strict inequality, so two boxes laid side by side in a level are not
        // permanently in collision with each other.
        b.position = { 10, 0 };
        frame(world);
        CHECK(a.hits == 0);
    }
    SUBCASE("apart") {
        b.position = { 40, 0 };
        frame(world);
        CHECK(a.hits == 0);
        CHECK(b.hits == 0);
    }
    SUBCASE("apart on y only") {
        b.position = { 0, 40 };
        frame(world);
        CHECK(a.hits == 0);
    }
}

TEST_CASE_FIXTURE(Fixture, "circle against circle") {
    World world;
    auto &a = world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::circle(5) });
    auto &b = world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::circle(5) });

    SUBCASE("overlapping") {
        b.position = { 9, 0 };
        frame(world);
        CHECK(a.hits == 1);
    }
    SUBCASE("exactly touching does not count") {
        b.position = { 10, 0 };
        frame(world);
        CHECK(a.hits == 0);
    }
    SUBCASE("a diagonal gap that a box test would get wrong") {
        // The corners of the two bounding boxes overlap and the circles do not.
        // This is the case that says the narrow phase is really working with
        // circles rather than with their boxes.
        b.position = { 7.4f, 7.4f };
        CHECK(std::sqrt(7.4f * 7.4f * 2) > 10.0f);
        frame(world);
        CHECK(a.hits == 0);
    }
}

TEST_CASE_FIXTURE(Fixture, "circle against rectangle") {
    World world;
    auto &ball = world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::circle(5) });
    auto &box =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 20, 20 }) });

    SUBCASE("through a face") {
        ball.position = { 13, 0 };
        frame(world);
        CHECK(ball.hits == 1);
    }
    SUBCASE("past a face") {
        ball.position = { 16, 0 };
        frame(world);
        CHECK(ball.hits == 0);
    }
    SUBCASE("near a corner, diagonally, is the case a box test gets wrong") {
        // 3,3 beyond the corner: 4.24 away, inside a radius of 5.
        ball.position = { 13, 13 };
        frame(world);
        CHECK(ball.hits == 1);
        // 4,4 beyond it: 5.66 away, outside. A box test would say yes to both.
        ball.position = { 14, 14 };
        ball.hits = 0;
        box.hits = 0;
        frame(world);
        CHECK(ball.hits == 0);
    }
    SUBCASE("the circle's centre inside the rectangle") {
        ball.position = { 2, 2 };
        frame(world);
        CHECK(ball.hits == 1);
    }
    SUBCASE("and it works with the rectangle first, which is the other dispatch") {
        World other;
        auto &r =
            other.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 20, 20 }) });
        auto &c = other.spawn<Probe>({ .position = { 13, 0 }, .shape = rmp::circle(5) });
        frame(other);
        CHECK(r.hits == 1);
        CHECK(c.hits == 1);
    }
}

// ---------------------------------------------------------------------------
// Notification
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "both sides are told, once each, per frame") {
    World world;
    auto &a =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    auto &b =
        world.spawn<Probe>({ .position = { 5, 0 }, .shape = rmp::rect({ 10, 10 }) });

    frame(world);
    CHECK(a.hits == 1);
    CHECK(b.hits == 1);
    CHECK(a.last == &b);
    CHECK(b.last == &a);

    SUBCASE("and again next frame, because they are still overlapping") {
        frame(world);
        CHECK(a.hits == 2);
    }
}

TEST_CASE_FIXTURE(Fixture, "an object spanning several grid cells is reported once") {
    // The bug this is for: a long platform sits in ten buckets and meets the
    // same player in each of them, so without de-duplication _collision fires
    // ten times and a damage-on-touch enemy does ten times the damage.
    World world;
    auto &floor =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 4000, 20 }) });
    auto &player =
        world.spawn<Probe>({ .position = { 0, 5 }, .shape = rmp::rect({ 20, 20 }) });
    frame(world);
    CHECK(floor.hits == 1);
    CHECK(player.hits == 1);
}

TEST_CASE_FIXTURE(Fixture, "on_collision runs alongside _collision, not instead of it") {
    World world;
    int called = 0;
    rmp::Object *saw = nullptr;
    auto &a =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    auto &b =
        world.spawn<Probe>({ .position = { 5, 0 }, .shape = rmp::rect({ 10, 10 }) });
    a.on_collision([&](rmp::Object &self, rmp::Object &other) {
        called++;
        saw = &other;
        CHECK(&self == &a);
    });

    frame(world);
    CHECK(a.hits == 1); // the override still ran
    CHECK(called == 1); // and so did the lambda
    CHECK(saw == &b);
}

TEST_CASE_FIXTURE(Fixture, "setting a callback twice replaces it") {
    World world;
    int first = 0;
    int second = 0;
    auto &a = world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    world.spawn({ .position = { 5, 0 }, .shape = rmp::rect({ 10, 10 }) });
    a.on_collision([&](rmp::Object &, rmp::Object &) { first++; });
    a.on_collision([&](rmp::Object &, rmp::Object &) { second++; });
    frame(world);
    CHECK(first == 0);
    CHECK(second == 1);
}

TEST_CASE_FIXTURE(Fixture, "an object that dies on contact does not reach anybody else") {
    World world;
    auto &a =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    auto &b =
        world.spawn<Probe>({ .position = { 5, 0 }, .shape = rmp::rect({ 10, 10 }) });
    a.die_on_hit = true;

    frame(world);
    CHECK_FALSE(a.alive());
    // a was told, and destroyed itself while being told. b must NOT then be
    // handed a reference to something whose _end() has already run.
    CHECK(a.hits == 1);
    CHECK(b.hits == 0);
    CHECK(world.object_count() == 1);
}

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "by default everything collides with everything") {
    World world;
    auto &a =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    auto &b =
        world.spawn<Probe>({ .position = { 5, 0 }, .shape = rmp::rect({ 10, 10 }) });
    CHECK(a.collision_layer == 1u);
    CHECK(a.collision_mask == 1u);
    frame(world);
    CHECK(a.hits == 1);
    CHECK(b.hits == 1);
}

TEST_CASE_FIXTURE(Fixture, "the three things layers exist for") {
    constexpr unsigned kPlayer = 1u << 0;
    constexpr unsigned kEnemy = 1u << 1;
    constexpr unsigned kBullet = 1u << 2;
    constexpr unsigned kWorld = 1u << 3;

    SUBCASE("the player's bullet does not hit the player") {
        World world;
        auto &player =
            world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
        player.collision_layer = kPlayer;
        player.collision_mask = kEnemy | kWorld;
        auto &bullet =
            world.spawn<Probe>({ .position = { 2, 0 }, .shape = rmp::rect({ 4, 4 }) });
        bullet.collision_layer = kBullet;
        bullet.collision_mask = kEnemy | kWorld;
        frame(world);
        CHECK(player.hits == 0);
        CHECK(bullet.hits == 0);
    }
    SUBCASE("enemies pass through each other but not through walls") {
        World world;
        auto &e1 =
            world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
        auto &e2 =
            world.spawn<Probe>({ .position = { 5, 0 }, .shape = rmp::rect({ 10, 10 }) });
        auto &wall =
            world.spawn<Probe>({ .position = { 8, 0 }, .shape = rmp::rect({ 10, 10 }) });
        for (Probe *e : { &e1, &e2 }) {
            e->collision_layer = kEnemy;
            e->collision_mask = kWorld;
        }
        wall.collision_layer = kWorld;
        wall.collision_mask = 0;
        frame(world);
        CHECK(e1.hits == 1); // the wall
        CHECK(e2.hits == 1); // the wall
        CHECK(wall.hits == 2);
        CHECK(e1.last == &wall);
    }
    SUBCASE("a trigger sees the player and nothing else") {
        World world;
        auto &trigger =
            world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 40, 40 }) });
        trigger.collision_layer = 0;
        trigger.collision_mask = kPlayer;
        auto &player =
            world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
        player.collision_layer = kPlayer;
        player.collision_mask = kWorld;
        auto &enemy =
            world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
        enemy.collision_layer = kEnemy;
        enemy.collision_mask = kWorld;
        frame(world);
        CHECK(trigger.hits == 1);
        CHECK(trigger.last == &player);
        CHECK(enemy.hits == 0);
    }
}

TEST_CASE_FIXTURE(Fixture,
                  "one mask touching the other's layer is enough, either way round") {
    World world;
    auto &a =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    auto &b =
        world.spawn<Probe>({ .position = { 5, 0 }, .shape = rmp::rect({ 10, 10 }) });
    a.collision_layer = 1;
    a.collision_mask = 0; // a is looking for nothing
    b.collision_layer = 4;
    b.collision_mask = 1; // but b is looking for a
    frame(world);
    CHECK(a.hits == 1);
    CHECK(b.hits == 1);
}

TEST_CASE_FIXTURE(Fixture, "two objects with nothing in common never meet") {
    World world;
    auto &a =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    auto &b =
        world.spawn<Probe>({ .position = { 5, 0 }, .shape = rmp::rect({ 10, 10 }) });
    a.collision_layer = 1;
    a.collision_mask = 2;
    b.collision_layer = 4;
    b.collision_mask = 8;
    frame(world);
    CHECK(a.hits == 0);
    CHECK(b.hits == 0);
}

// ---------------------------------------------------------------------------
// solid and immovable
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "nothing is pushed apart unless both sides are solid") {
    World world;
    auto &a =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    auto &b =
        world.spawn<Probe>({ .position = { 5, 0 }, .shape = rmp::rect({ 10, 10 }) });

    SUBCASE("neither: told, and they pass through") {
        frame(world);
        CHECK(a.hits == 1);
        CHECK(a.position.x == doctest::Approx(0));
        CHECK(b.position.x == doctest::Approx(5));
    }
    SUBCASE("only one: still nothing moves, because solid is about the pair") {
        a.solid = true;
        frame(world);
        CHECK(a.hits == 1);
        CHECK(a.position.x == doctest::Approx(0));
        CHECK(b.position.x == doctest::Approx(5));
    }
    SUBCASE("both: separated, half each") {
        a.solid = true;
        b.solid = true;
        frame(world);
        CHECK(a.position.x == doctest::Approx(-2.5));
        CHECK(b.position.x == doctest::Approx(7.5));
        // And afterwards they are exactly touching, which is what "separated"
        // has to mean or they overlap again next frame.
        CHECK(b.position.x - a.position.x == doctest::Approx(10));
    }
}

TEST_CASE_FIXTURE(Fixture,
                  "an immovable object does not move, and the other one takes it all") {
    World world;
    auto &ground =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 100, 20 }) });
    ground.solid = true;
    ground.immovable = true;
    // ABOVE the ground: y grows downward, so -15 is overlapping it from on top.
    auto &player =
        world.spawn<Probe>({ .position = { 0, -15 }, .shape = rmp::rect({ 10, 20 }) });
    player.solid = true;

    frame(world);
    CHECK(ground.position.y == doctest::Approx(0));
    // Pushed up until its bottom rests on the ground's top: the ground's top is
    // -10, the player is 20 tall, so its centre lands at -20.
    CHECK(player.position.y == doctest::Approx(-20));
}

TEST_CASE_FIXTURE(Fixture, "two immovable objects overlapping are left alone") {
    // A level design mistake, not something to solve at runtime: moving one of
    // them would be inventing an answer.
    World world;
    auto &a =
        world.spawn<Probe>({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    auto &b =
        world.spawn<Probe>({ .position = { 5, 0 }, .shape = rmp::rect({ 10, 10 }) });
    for (Probe *o : { &a, &b }) {
        o->solid = true;
        o->immovable = true;
    }
    frame(world);
    CHECK(a.position.x == doctest::Approx(0));
    CHECK(b.position.x == doctest::Approx(5));
    CHECK(a.hits == 1); // still told
}

TEST_CASE_FIXTURE(Fixture, "separation takes the axis of least penetration") {
    // The property that makes a platformer work: a character overlapping the
    // top of a wide platform lands ON it rather than being shoved out of the
    // side, because up is the shorter way out.
    World world;
    auto &platform =
        world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 200, 20 }) });
    platform.solid = true;
    platform.immovable = true;
    auto &player =
        world.spawn({ .position = { 0, -14 }, .shape = rmp::rect({ 10, 20 }) });
    player.solid = true;

    frame(world);
    CHECK(player.position.x == doctest::Approx(0)); // not sideways
    CHECK(player.position.y == doctest::Approx(-20));
}

TEST_CASE_FIXTURE(Fixture, "a player falling onto the floor comes to rest on it") {
    World world;
    world.gravity = { 0, 1000 };
    auto &floor =
        world.spawn({ .position = { 0, 100 }, .shape = rmp::rect({ 400, 20 }) });
    floor.solid = true;
    floor.immovable = true;
    auto &player = world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 20 }) });
    player.solid = true;
    player.gravity_scale = 1;

    for (int i = 0; i < 30; i++) frame(world, 1.0f / 60);
    // Standing on the floor: floor top is 90, the player is 20 tall.
    CHECK(player.position.y == doctest::Approx(80).epsilon(0.05));
}

// ---------------------------------------------------------------------------
// The swept test
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "a bullet at 2000 u/s hits a wall 4 units thick at 1/30") {
    // The exact case from next_architecture/04-objects.md. Without the swept
    // test it fails every single time: 2000/30 is 66 units a frame and the wall
    // is 4 thick, so there is no frame on which the two overlap.
    World world;
    // The wall is at 170 and not 200 deliberately: at 2000 u/s and 1/30 the
    // bullet's sampled positions are 0, 66.7, 133.3, 200 -- and at 200 it would
    // overlap a wall at 200 by luck, so the test would pass with the swept test
    // deleted. At 170 no sampled position touches it and only the sweep can.
    auto &wall =
        world.spawn<Probe>({ .position = { 170, 0 }, .shape = rmp::rect({ 4, 100 }) });
    // BOTH solid, because the rewind is a resolution and `solid` is the field
    // that says a contact resolves. A bullet that stops at a wall is the solid
    // pair; the case where neither is has its own test below, and there the
    // bullet is told and carries on.
    wall.solid = true;
    wall.immovable = true;
    auto &bullet = world.spawn<Probe>(
        { .position = { 0, 0 }, .shape = rmp::rect({ 2, 2 }), .velocity = { 2000, 0 } });
    bullet.solid = true;
    for (int i = 0; i < 10 && bullet.hits == 0; i++) frame(world, 1.0f / 30);

    CHECK(bullet.hits == 1);
    CHECK(wall.hits == 1);
    // And it was put back where it met the wall, not left wherever the step
    // happened to end.
    CHECK(bullet.position.x == doctest::Approx(167).epsilon(0.02));
}

TEST_CASE_FIXTURE(Fixture, "the same bullet, as a circle, against a circle") {
    World world;
    auto &target =
        world.spawn<Probe>({ .position = { 300, 0 }, .shape = rmp::circle(3) });
    auto &bullet = world.spawn<Probe>(
        { .position = { 0, 0 }, .shape = rmp::circle(1), .velocity = { 2000, 0 } });
    for (int i = 0; i < 10 && bullet.hits == 0; i++) frame(world, 1.0f / 30);
    CHECK(bullet.hits == 1);
    CHECK(target.hits == 1);
}

TEST_CASE_FIXTURE(Fixture, "a slow object is not swept, and nothing changes for it") {
    World world;
    auto &a = world.spawn<Probe>(
        { .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }), .velocity = { 1, 0 } });
    world.spawn<Probe>({ .position = { 100, 0 }, .shape = rmp::rect({ 10, 10 }) });
    frame(world, 1.0f / 60);
    CHECK(a.hits == 0);
    CHECK(a.position.x == doctest::Approx(1.0f / 60));
}

TEST_CASE_FIXTURE(Fixture, "a teleport is not a sweep") {
    // Writing position in _update is a teleport, and a teleport has no segment
    // to test: a game that moves something across the map should not collide
    // with everything on the line between.
    World world;
    class Teleporter : public rmp::Object {
    public:
        void _update(float delta) override {
            (void)delta;
            position = { 400, 0 };
        }
    };
    auto &wall =
        world.spawn<Probe>({ .position = { 200, 0 }, .shape = rmp::rect({ 4, 100 }) });
    auto &mover =
        world.spawn<Teleporter>({ .position = { 0, 0 }, .shape = rmp::rect({ 2, 2 }) });

    frame(world, 1.0f / 60);
    CHECK(wall.hits == 0);
    CHECK(mover.position.x == doctest::Approx(400));
}

// ---------------------------------------------------------------------------
// THE DIFFERENTIAL TEST. The most valuable one in the file.
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "the grid and the O(n^2) loop agree, over a thousand objects") {
    World world;
    Rng rng;
    for (int i = 0; i < 1000; i++) {
        const bool round = (rng.next() % 2) == 0;
        const Vector2 at{ rng.range(-500, 500), rng.range(-500, 500) };
        if (round) {
            world.spawn({ .position = at, .shape = rmp::circle(rng.range(2, 20)) });
        } else {
            world.spawn({ .position = at,
                          .shape = rmp::rect({ rng.range(4, 40), rng.range(4, 40) }) });
        }
    }
    REQUIRE(world.object_count() == 1000);

    const PairList with_grid = pairs_of(world, true);
    const PairList brute = pairs_of(world, false);

    // Not "the same size": the same pairs. A grid that dropped one and invented
    // another would pass a count comparison.
    CHECK(with_grid == brute);
    // And the scene has to be dense enough for the test to mean something --
    // two identical empty lists agree about nothing.
    CHECK(brute.size() > 100);
}

TEST_CASE_FIXTURE(Fixture, "they agree when everything is piled in one place") {
    // The degenerate case for a grid: every object in one cell, so the grid is
    // the brute-force loop with extra steps. It still has to be right.
    World world;
    Rng rng;
    for (int i = 0; i < 200; i++) {
        world.spawn({ .position = { rng.range(-5, 5), rng.range(-5, 5) },
                      .shape = rmp::rect({ 10, 10 }) });
    }
    CHECK(pairs_of(world, true) == pairs_of(world, false));
}

TEST_CASE_FIXTURE(Fixture, "they agree when everything is far apart") {
    // The other degenerate case: a huge, nearly empty world, which is where a
    // grid is tempted to allocate millions of buckets and where the fallback
    // to the brute-force loop lives.
    World world;
    for (int i = 0; i < 100; i++) {
        world.spawn({ .position = { static_cast<float>(i) * 100000.0f, 0 },
                      .shape = rmp::rect({ 10, 10 }) });
    }
    const PairList with_grid = pairs_of(world, true);
    CHECK(with_grid == pairs_of(world, false));
    CHECK(with_grid.empty());
}

TEST_CASE_FIXTURE(Fixture, "they agree with objects of wildly different sizes") {
    // A grid sized for the average is wrong for both ends of this, which is
    // exactly when a broad phase starts dropping pairs.
    World world;
    Rng rng;
    world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 4000, 4000 }) });
    for (int i = 0; i < 300; i++) {
        world.spawn({ .position = { rng.range(-2000, 2000), rng.range(-2000, 2000) },
                      .shape = rmp::circle(rng.range(1, 3)) });
    }
    CHECK(pairs_of(world, true) == pairs_of(world, false));
}

TEST_CASE_FIXTURE(Fixture,
                  "and they agree with fast movers, where the swept boxes join in") {
    World world;
    Rng rng;
    for (int i = 0; i < 300; i++) {
        auto &o =
            world.spawn({ .position = { rng.range(-300, 300), rng.range(-300, 300) },
                          .shape = rmp::rect({ 6, 6 }) });
        o.velocity = { rng.range(-2000, 2000), rng.range(-2000, 2000) };
    }
    rmp::objects::detail::update(world, 1.0f / 30);
    CHECK(pairs_of(world, true) == pairs_of(world, false));
}

// ---------------------------------------------------------------------------
// Raycasting
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "a ray hits what is in front of it and reports where") {
    World world;
    auto &wall = world.spawn({ .position = { 100, 0 }, .shape = rmp::rect({ 20, 200 }) });

    const rmp::RayHit hit = world.raycast({ 0, 0 }, { 300, 0 });
    REQUIRE(static_cast<bool>(hit));
    CHECK(hit.object.get() == &wall);
    CHECK(hit.point.x == doctest::Approx(90)); // the near face
    CHECK(hit.distance == doctest::Approx(90));
    CHECK(hit.normal.x == doctest::Approx(-1)); // pointing back at the ray
    CHECK(hit.normal.y == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture, "a ray that reaches nothing is false, and false is the test") {
    World world;
    world.spawn({ .position = { 1000, 0 }, .shape = rmp::rect({ 20, 20 }) });
    const rmp::RayHit hit = world.raycast({ 0, 0 }, { 100, 0 });
    CHECK_FALSE(static_cast<bool>(hit));
    CHECK(!hit.object);
}

TEST_CASE_FIXTURE(Fixture, "the nearest hit is the one that comes back") {
    World world;
    auto &near = world.spawn({ .position = { 50, 0 }, .shape = rmp::rect({ 10, 100 }) });
    world.spawn({ .position = { 150, 0 }, .shape = rmp::rect({ 10, 100 }) });
    world.spawn({ .position = { 250, 0 }, .shape = rmp::rect({ 10, 100 }) });

    const rmp::RayHit hit = world.raycast({ 0, 0 }, { 400, 0 });
    REQUIRE(static_cast<bool>(hit));
    CHECK(hit.object.get() == &near);
}

TEST_CASE_FIXTURE(Fixture, "ignore is why this exists and not CheckCollisionLines") {
    // Without it the first thing every shot hits is the thing that fired it.
    World world;
    auto &shooter = world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 20, 20 }) });
    auto &target =
        world.spawn({ .position = { 200, 0 }, .shape = rmp::rect({ 20, 20 }) });

    const rmp::RayHit without = world.raycast({ 0, 0 }, { 400, 0 });
    CHECK(without.object.get() == &shooter);

    const rmp::RayHit with =
        world.raycast({ .from = { 0, 0 }, .to = { 400, 0 }, .ignore = &shooter });
    CHECK(with.object.get() == &target);
}

TEST_CASE_FIXTURE(Fixture, "the mask filters the same way the collision pass does") {
    constexpr unsigned kEnemy = 1u << 1;
    constexpr unsigned kScenery = 1u << 4;
    World world;
    auto &bush = world.spawn({ .position = { 50, 0 }, .shape = rmp::rect({ 20, 20 }) });
    bush.collision_layer = kScenery;
    auto &enemy = world.spawn({ .position = { 150, 0 }, .shape = rmp::rect({ 20, 20 }) });
    enemy.collision_layer = kEnemy;

    const rmp::RayHit shot =
        world.raycast({ .from = { 0, 0 }, .to = { 400, 0 }, .mask = kEnemy });
    CHECK(shot.object.get() == &enemy);

    const rmp::RayHit sight = world.raycast({ 0, 0 }, { 400, 0 });
    CHECK(sight.object.get() == &bush);
}

TEST_CASE_FIXTURE(Fixture, "a ray against a circle gets the circle's normal") {
    World world;
    world.spawn({ .position = { 100, 0 }, .shape = rmp::circle(10) });
    const rmp::RayHit hit = world.raycast({ 0, 0 }, { 300, 0 });
    REQUIRE(static_cast<bool>(hit));
    CHECK(hit.point.x == doctest::Approx(90));
    CHECK(hit.normal.x == doctest::Approx(-1));

    SUBCASE("and a glancing ray gets a slanted one, which is the point of it") {
        const rmp::RayHit side = world.raycast({ 0, 8 }, { 300, 8 });
        REQUIRE(static_cast<bool>(side));
        CHECK(side.normal.y > 0.5f); // mostly upward, not straight back
        const float len =
            std::sqrt(side.normal.x * side.normal.x + side.normal.y * side.normal.y);
        CHECK(len == doctest::Approx(1).epsilon(0.01));
    }
}

TEST_CASE_FIXTURE(Fixture,
                  "a ray that starts inside something hits it at zero distance") {
    World world;
    auto &box = world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 100, 100 }) });
    const rmp::RayHit hit = world.raycast({ 0, 0 }, { 300, 0 });
    REQUIRE(static_cast<bool>(hit));
    CHECK(hit.object.get() == &box);
    CHECK(hit.distance == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture, "a ray of zero length is not a crash") {
    World world;
    world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 10, 10 }) });
    const rmp::RayHit hit = world.raycast({ 0, 0 }, { 0, 0 });
    CHECK(hit.distance == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture, "raycast_all comes back nearest first") {
    World world;
    auto &a = world.spawn({ .position = { 50, 0 }, .shape = rmp::rect({ 10, 100 }) });
    auto &b = world.spawn({ .position = { 150, 0 }, .shape = rmp::rect({ 10, 100 }) });
    auto &c = world.spawn({ .position = { 250, 0 }, .shape = rmp::rect({ 10, 100 }) });

    rmp::RayHit hits[8];
    const int n = world.raycast_all({ .from = { 0, 0 }, .to = { 400, 0 } }, hits, 8);
    REQUIRE(n == 3);
    CHECK(hits[0].object.get() == &a);
    CHECK(hits[1].object.get() == &b);
    CHECK(hits[2].object.get() == &c);
    CHECK(hits[0].distance < hits[1].distance);
    CHECK(hits[1].distance < hits[2].distance);

    SUBCASE("and it stops at max rather than writing past the end") {
        rmp::RayHit few[2];
        const int capped =
            world.raycast_all({ .from = { 0, 0 }, .to = { 400, 0 } }, few, 2);
        CHECK(capped == 2);
        CHECK(few[0].object.get() == &a);
    }
    SUBCASE("max of zero writes nothing") {
        rmp::RayHit none[1];
        CHECK(world.raycast_all({ .from = { 0, 0 }, .to = { 400, 0 } }, none, 0) == 0);
    }
    SUBCASE("a null buffer is refused instead of dereferenced") {
        CHECK(world.raycast_all({ .from = { 0, 0 }, .to = { 400, 0 } }, nullptr, 8) == 0);
    }
}

TEST_CASE_FIXTURE(
    Fixture, "THE SECOND DIFFERENTIAL: the grid ray agrees with testing everything") {
    // The raycast walks the grid with a DDA, which means it can miss a cell and
    // therefore an object. Testing every object is the definition it has to
    // match, so: a thousand rays in random directions over a crowded scene, and
    // every single one has to find the same first object.
    World world;
    Rng rng;
    std::vector<rmp::Object *> all;
    for (int i = 0; i < 400; i++) {
        auto &o =
            world.spawn({ .position = { rng.range(-400, 400), rng.range(-400, 400) },
                          .shape = (rng.next() % 2) == 0
                              ? rmp::circle(rng.range(3, 15))
                              : rmp::rect({ rng.range(6, 30), rng.range(6, 30) }) });
        all.push_back(&o);
    }

    int checked = 0;
    int found = 0;
    for (int i = 0; i < 1000; i++) {
        const Vector2 from{ rng.range(-600, 600), rng.range(-600, 600) };
        const Vector2 to{ rng.range(-600, 600), rng.range(-600, 600) };
        const rmp::RayHit grid = world.raycast(from, to);

        // The definition: every object, nearest wins. Written here rather than
        // called, because a reference implementation that shares code with the
        // thing it checks is checking nothing.
        rmp::Object *best = nullptr;
        float best_t = 2;
        for (rmp::Object *o : all) {
            // The geometry is redone here rather than called: a reference
            // implementation that shares code with the thing it checks is
            // checking nothing. `all` is in creation order, and `<` rather than
            // `<=` keeps the first one on a tie -- the same rule the
            // implementation uses, and the tie is common because a ray starting
            // inside two overlapping objects hits both at distance zero.
            const Rectangle box = o->world_collider();
            const bool is_circle = o->shape.kind == rmp::ShapeKind::CIRCLE;
            const Vector2 d{ to.x - from.x, to.y - from.y };
            float t = 2;
            if (is_circle) {
                const float r = o->shape.radius * o->scale.x;
                const Vector2 m{ from.x - o->position.x, from.y - o->position.y };
                const float qa = d.x * d.x + d.y * d.y;
                if (qa > 0) {
                    const float qb = 2 * (m.x * d.x + m.y * d.y);
                    const float qc = m.x * m.x + m.y * m.y - r * r;
                    const float disc = qb * qb - 4 * qa * qc;
                    if (disc >= 0) {
                        const float root = std::sqrt(disc);
                        float h = (-qb - root) / (2 * qa);
                        if (h < 0) h = (-qb + root) / (2 * qa);
                        if (h >= 0 && h <= 1) t = h;
                    }
                }
            } else {
                float near = 0;
                float far = 1;
                bool ok = true;
                for (int axis = 0; axis < 2 && ok; axis++) {
                    const float o0 = axis == 0 ? from.x : from.y;
                    const float dd = axis == 0 ? d.x : d.y;
                    const float lo = axis == 0 ? box.x : box.y;
                    const float hi = axis == 0 ? box.x + box.width : box.y + box.height;
                    if (dd > -1e-4f && dd < 1e-4f) {
                        if (o0 < lo || o0 > hi) ok = false;
                        continue;
                    }
                    float t0 = (lo - o0) / dd;
                    float t1 = (hi - o0) / dd;
                    if (t0 > t1) std::swap(t0, t1);
                    if (t0 > near) near = t0;
                    if (t1 < far) far = t1;
                    if (near > far) ok = false;
                }
                if (ok) t = near;
            }
            if (t <= 1 && t < best_t) {
                best_t = t;
                best = o;
            }
        }

        checked++;
        if (best != nullptr) found++;
        if (grid.object.get() != best) {
            MESSAGE("ray " << i << " from (" << from.x << "," << from.y << ") to ("
                           << to.x << "," << to.y << ")");
            MESSAGE("  grid says "
                    << (!grid.object ? std::string("nothing")
                                     : std::string("object at ") +
                                std::to_string(grid.object->position.x) + "," +
                                std::to_string(grid.object->position.y)));
            MESSAGE("  brute says " << (best == nullptr ? std::string("nothing")
                                                        : std::string("object at ") +
                                                std::to_string(best->position.x) + "," +
                                                std::to_string(best->position.y))
                                    << "  t=" << best_t);
        }
        CHECK(grid.object.get() == best);
        if (grid.object.get() != best) break; // one message, not a thousand
    }
    CHECK(checked == 1000);
    // And the scene has to be crowded enough that most rays hit something, or
    // the test is a thousand agreements about nothing.
    CHECK(found > 500);
}

// ---------------------------------------------------------------------------
// What the swept test is allowed to do to an object it caught
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "a non-solid pair is told about the sweep and keeps going") {
    // `solid` decides whether a contact is RESOLVED; it has never decided
    // whether you are told. The rewind is a resolution, so a bullet crossing a
    // coin must be reported and must carry on -- rewinding it pins it at the
    // coin, where the next sweep starts inside and pins it again, forever.
    World world;
    auto &coin =
        world.spawn<Probe>({ .position = { 300, 0 }, .shape = rmp::rect({ 16, 16 }) });
    auto &bullet = world.spawn<Probe>({ .position = { 250, 0 },
                                        .shape = rmp::rect({ 4, 4 }),
                                        .velocity = { 6000, 0 } });

    frame(world, 1.0f / 60); // 100 units, clean over the coin
    CHECK(bullet.hits == 1);
    CHECK(coin.hits == 1);
    CHECK(bullet.position.x == doctest::Approx(350));

    frame(world, 1.0f / 60);
    CHECK(bullet.position.x == doctest::Approx(450));
    CHECK(bullet.hits == 1); // once, on the way past
}

TEST_CASE_FIXTURE(Fixture, "an object that starts inside a wall gets out of it") {
    // A muzzle inside the shooter, a player placed in a platform by a Tiled
    // object layer, an enemy spawned on a wall. The sweep from a point already
    // inside reports contact at t=0, and a rewind to t=0 is a rewind to where
    // it already was -- so it never leaves.
    World world;
    auto &wall = world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 40, 400 }) });
    wall.solid = true;
    wall.immovable = true;
    auto &escaping = world.spawn(
        { .position = { 0, 0 }, .shape = rmp::rect({ 8, 8 }), .velocity = { 3000, 0 } });
    escaping.solid = true;

    frame(world, 1.0f / 60);
    CHECK(escaping.position.x > 40);
    for (int i = 0; i < 4; i++) frame(world, 1.0f / 60);
    CHECK(escaping.position.x == doctest::Approx(250));
}

TEST_CASE_FIXTURE(Fixture, "a wrap is a teleport, and a teleport is not a sweep") {
    // Edge::WRAP moves the object by the width of the world after the
    // integration. Without telling the swept test, the segment it tests spans
    // the whole world and the asteroid collides with everything between the two
    // edges -- which is every solid thing in an Asteroids game.
    World world;
    const Rectangle area{ 0, 0, 800, 100 };
    auto &wall =
        world.spawn<Probe>({ .position = { 400, 50 }, .shape = rmp::rect({ 20, 200 }) });
    auto &rock = world.spawn<Probe>({ .position = { 795, 50 },
                                      .shape = rmp::rect({ 10, 10 }),
                                      .velocity = { 600, 0 },
                                      .edges = rmp::Edge::WRAP,
                                      .bounds = area });

    frame(world, 1.0f / 30);
    CHECK(rock.position.x == doctest::Approx(-5));
    CHECK(wall.hits == 0);
    CHECK(rock.hits == 0);
}

// ---------------------------------------------------------------------------
// solid_only
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "solid_only skips the trigger and finds the ground") {
    // The ground check every platformer needs. A mask cannot say this: layers
    // are about who collides with whom, and a pickup lying on the floor is on
    // the same layer as the floor.
    World world;
    auto &coin = world.spawn({ .position = { 0, 20 }, .shape = rmp::rect({ 16, 16 }) });
    auto &ground =
        world.spawn({ .position = { 0, 60 }, .shape = rmp::rect({ 200, 20 }) });
    ground.solid = true;
    ground.immovable = true;

    const rmp::RayHit any = world.raycast({ 0, 0 }, { 0, 100 });
    CHECK(any.object.get() == &coin);

    const rmp::RayHit floor =
        world.raycast({ .from = { 0, 0 }, .to = { 0, 100 }, .solid_only = true });
    REQUIRE(static_cast<bool>(floor));
    CHECK(floor.object.get() == &ground);
    CHECK(floor.point.y == doctest::Approx(50));
}

// ---------------------------------------------------------------------------
// The pointer capture
// ---------------------------------------------------------------------------

namespace {

rmp::input::detail::DeviceState g_devices;
void fake_sample(rmp::input::detail::DeviceState *out) { *out = g_devices; }

// The pointer pass reads rmp::input, so the test writes the devices and drives
// one frame of each. Split in two because the case being tested is a frame
// where the app sampled input and the pointer pass did NOT run -- a release
// lost to a lost window, a dropped up event, or a frame the UI owned.
struct Pointer {
    Pointer() {
        rmp::input::detail::reset();
        g_devices = rmp::input::detail::DeviceState{};
        rmp::input::detail::set_sample_provider(fake_sample);
        // A UI case that ran before this one may have left the pointer "over
        // the UI" -- the flag only clears at the next begin(), which this
        // fixture never calls -- and then consumed_pointer() would swallow
        // every press here. Start the frame the way the app does.
        rmp::ui::detail::begin_capture_frame();
        rmp::input::detail::begin_frame(); // frame one has no edges
    }
    ~Pointer() {
        rmp::input::detail::reset();
        g_devices = rmp::input::detail::DeviceState{};
    }

    static void sample(Vector2 at, bool down) {
        g_devices.pointer = at;
        g_devices.mouse[MOUSE_BUTTON_LEFT] = down;
        rmp::input::detail::begin_frame();
    }
    static void step(rmp::Scene &scene, Vector2 at, bool down) {
        sample(at, down);
        rmp::objects::detail::pointer(scene);
    }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "a press that hits nothing drops the capture") {
    World world;
    Pointer pointer;

    int clicks = 0;
    auto &button = world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 40, 40 }) });
    button.on_click([&clicks](rmp::Object &) { clicks++; });

    SUBCASE("the ordinary press and release still clicks") {
        Pointer::step(world, Vector2{ 0, 0 }, true);
        Pointer::step(world, Vector2{ 0, 0 }, false);
        CHECK(clicks == 1);
    }

    SUBCASE("a release the pointer pass never saw does not arrive later") {
        Pointer::step(world, Vector2{ 0, 0 }, true); // pressed on it
        Pointer::sample(Vector2{ 0, 0 }, false); // released, and nobody ran

        // A press over empty space: it found nothing, so it holds nothing.
        Pointer::step(world, Vector2{ 500, 500 }, true);
        // And the release that follows it is over the button again.
        Pointer::step(world, Vector2{ 0, 0 }, false);
        CHECK(clicks == 0);
    }
}

// ---------------------------------------------------------------------------
// The budget. Not a benchmark: a gate against the broad phase going quadratic
// or allocating per cell again, which is what it used to do -- one ray over a
// 500-object scene cost as much as the whole collision pass.
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "a hundred raycasts over two thousand objects are cheap") {
    World world;
    Rng rng;
    for (int i = 0; i < 2000; i++) {
        world.spawn({ .position = { rng.range(-2000, 2000), rng.range(-2000, 2000) },
                      .shape = rmp::rect({ 4, 4 }) });
    }
    REQUIRE(world.object_count() == 2000);

    const auto started = std::chrono::steady_clock::now();
    int hits = 0;
    for (int i = 0; i < 100; i++) {
        const Vector2 from{ rng.range(-2000, 2000), rng.range(-2000, 2000) };
        const Vector2 to{ rng.range(-2000, 2000), rng.range(-2000, 2000) };
        if (world.raycast(from, to)) hits++;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    const double ms = static_cast<double>(elapsed.count()) / 1000.0;
    MESSAGE("100 raycasts over 2000 objects: " << ms << " ms");
    CHECK(ms < 20.0);
}
