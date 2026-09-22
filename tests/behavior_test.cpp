// ===========================================================================
// The behavior engine and the catalogue.
//
// The absence of inheritance pays a dividend here that was not the reason for
// it: a behavior needs no scene, no window and no real object. It is a struct
// with a function, so most of this file hands one an rmp::Object built on the
// stack and a fixed delta, and reads the arithmetic back.
//
// The ones that need a scene say so, and they need it for the scene and not for
// a window -- spawning, raycasting for the ground, and the pass order.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/object_internal.h"

#include <rmp/behavior.h>
#include <rmp/object.h>
#include <rmp/scene.h>

#include <chrono>
#include <cmath>
#include <new>
#include <string>
#include <vector>

// The behaviors in this file are interface implementations like the ones in
// src/rmp/behaviors.cpp, and get the same treatment for the same reason: the
// signature belongs to the contract, not to what one test needs.
//
// NOLINTBEGIN(readability-make-member-function-const,readability-named-parameter,readability-convert-member-functions-to-static)
namespace {

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

struct Fixture {
    Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
        rmp::objects::detail::reset_pointer_for_tests();
    }
    ~Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
    }
};

// An object with no scene behind it: the cheapest thing a behavior can be
// tested against, and the point of the model.
class Loose : public rmp::Object {};

void frame(rmp::Scene &scene, float delta) {
    rmp::objects::detail::update(scene, delta);
    rmp::objects::detail::collide(scene);
    rmp::objects::detail::collect();
}

std::string g_log;
void note(const std::string &what) {
    if (!g_log.empty()) g_log += ' ';
    g_log += what;
}

// Behaviors of our own, which is the claim the model makes: one of these and
// one of rmp::behavior:: are the same kind of thing.
struct Spin {
    float degrees_per_second = 90;
    void _update(rmp::Object &self, float delta) {
        self.rotation += degrees_per_second * delta;
    }
};

struct Chatty {
    const char *name = "?";
    int updates = 0;
    void _ready(rmp::Object &) { note(std::string(name) + ".ready"); }
    void _update(rmp::Object &, float) {
        updates++;
        note(std::string(name) + ".update");
    }
    void _draw(rmp::Object &) { note(std::string(name) + ".draw"); }
    void _collision(rmp::Object &, rmp::Object &) {
        note(std::string(name) + ".collision");
    }
    void _end(rmp::Object &) { note(std::string(name) + ".end"); }
};

// Only the required hook. Everything else must cost nothing and not be called.
struct Minimal {
    int updates = 0;
    void _update(rmp::Object &, float) { updates++; }
};

// Bigger than any inline buffer would be, to prove the storage does not care.
struct Fat {
    double padding[64] = {};
    int updates = 0;
    void _update(rmp::Object &, float) { updates++; }
};

constexpr float kEps = 0.001f;

} // namespace

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "a behavior is any struct with _update, and yours is ours") {
    Loose object;
    auto &spin = object.add<Spin>({ .degrees_per_second = 180 });
    CHECK(spin.degrees_per_second == doctest::Approx(180));

    rmp::objects::detail::update_behaviors(object, 0.5f);
    CHECK(object.rotation == doctest::Approx(90));
}

TEST_CASE_FIXTURE(Fixture, "get, has and remove") {
    Loose object;
    CHECK_FALSE(object.has<Spin>());
    CHECK(object.get<Spin>() == nullptr);

    object.add<Spin>();
    CHECK(object.has<Spin>());
    REQUIRE(object.get<Spin>() != nullptr);

    SUBCASE("a type that is not there is nullptr, not a crash") {
        CHECK(object.get<Minimal>() == nullptr);
        CHECK_FALSE(object.has<Minimal>());
    }
    SUBCASE("remove takes it out") {
        object.remove<Spin>();
        CHECK_FALSE(object.has<Spin>());
        rmp::objects::detail::update_behaviors(object, 1.0f);
        CHECK(object.rotation == doctest::Approx(0));
    }
    SUBCASE("removing one that is not there is harmless") {
        object.remove<Minimal>();
        object.remove<Minimal>();
        CHECK(object.has<Spin>());
    }
}

TEST_CASE_FIXTURE(Fixture,
                  "the reference add returns stays valid, which is the contract") {
    Loose object;
    auto &first = object.add<Spin>({ .degrees_per_second = 10 });
    // Adding more must not move the first one: a game keeps that reference.
    object.add<Minimal>();
    object.add<Fat>();
    first.degrees_per_second = 99;
    CHECK(object.get<Spin>()->degrees_per_second == doctest::Approx(99));
    CHECK(&first == object.get<Spin>());
}

TEST_CASE_FIXTURE(Fixture,
                  "a behavior too big for any inline buffer behaves identically") {
    Loose object;
    auto &fat = object.add<Fat>();
    rmp::objects::detail::update_behaviors(object, 0.1f);
    rmp::objects::detail::update_behaviors(object, 0.1f);
    CHECK(fat.updates == 2);
    CHECK(object.get<Fat>() == &fat);
}

TEST_CASE_FIXTURE(Fixture, "they run in the order they were added") {
    g_log.clear();
    Loose object;
    object.add<Chatty>({ .name = "a" });
    object.add<Chatty>({ .name = "b" });
    // Two of the same TYPE replace rather than stack, so there is exactly one --
    // and replacing runs the first one's _end, which is the part worth pinning:
    // a behavior being thrown away gets to clean up.
    CHECK(rmp::objects::detail::behavior_count(object) == 1);
    CHECK(g_log == "a.ready a.end b.ready");

    SUBCASE("two DIFFERENT types both stay, in the order they went in") {
        g_log.clear();
        object.add<Spin>();
        CHECK(rmp::objects::detail::behavior_count(object) == 2);
        rmp::objects::detail::update_behaviors(object, 0.1f);
        CHECK(g_log == "b.update");
    }
}

TEST_CASE_FIXTURE(Fixture, "the optional hooks are only called when they exist") {
    Loose object;
    auto &minimal = object.add<Minimal>();
    // Nothing declares _ready, _draw, _collision or _end here. Driving all four
    // must be silent rather than a null call.
    rmp::objects::detail::draw_behaviors(object);
    Loose other;
    rmp::objects::detail::collide_behaviors(object, other);
    rmp::objects::detail::update_behaviors(object, 0.1f);
    CHECK(minimal.updates == 1);
}

TEST_CASE_FIXTURE(Fixture, "every hook fires, in the right place") {
    g_log.clear();
    Loose object;
    Loose other;
    object.add<Chatty>({ .name = "c" });
    rmp::objects::detail::update_behaviors(object, 0.1f);
    rmp::objects::detail::draw_behaviors(object);
    rmp::objects::detail::collide_behaviors(object, other);
    object.remove<Chatty>();
    CHECK(g_log == "c.ready c.update c.draw c.collision c.end");
}

namespace {
struct Adder {
    bool done = false;
    void _update(rmp::Object &self, float) {
        if (done) return;
        done = true;
        self.add<Minimal>();
    }
};
} // namespace

TEST_CASE_FIXTURE(Fixture, "adding one during an update defers it to the next frame") {
    Loose object;
    object.add<Adder>();
    rmp::objects::detail::update_behaviors(object, 0.1f);
    // It is attached and findable immediately -- that is what a caller expects
    // right after add -- but it has not been updated yet.
    REQUIRE(object.has<Minimal>());
    CHECK(object.get<Minimal>()->updates == 0);

    rmp::objects::detail::update_behaviors(object, 0.1f);
    CHECK(object.get<Minimal>()->updates == 1);
}

namespace {
struct SelfRemover {
    void _update(rmp::Object &self, float) { self.remove<SelfRemover>(); }
};
} // namespace

TEST_CASE_FIXTURE(Fixture, "a behavior may remove itself from its own update") {
    Loose object;
    object.add<SelfRemover>();
    rmp::objects::detail::update_behaviors(object, 0.1f);
    CHECK_FALSE(object.has<SelfRemover>());
    CHECK(rmp::objects::detail::behavior_count(object) == 0);
}

TEST_CASE_FIXTURE(Fixture, "behaviors run BEFORE the object's own _update") {
    // So that the user's code always has the last word over the framework's.
    class Last : public rmp::Object {
    public:
        void _update(float) override { note("object"); }
    };
    g_log.clear();
    World world;
    auto &object = world.spawn<Last>();
    object.add<Chatty>({ .name = "b" });
    g_log.clear();
    frame(world, 0.1f);
    CHECK(g_log == "b.update object");
}

TEST_CASE_FIXTURE(Fixture, "destroying the object runs every _end once") {
    g_log.clear();
    World world;
    auto &object = world.spawn();
    object.add<Chatty>({ .name = "d" });
    g_log.clear();
    object.destroy();
    CHECK(g_log == "d.end");
    rmp::objects::detail::collect();
    CHECK(g_log == "d.end");
}

// ---------------------------------------------------------------------------
// Ball
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "a ball starts still and stays still until something pushes it") {
    Loose ball;
    ball.shape = rmp::circle(5);
    auto &b = ball.add<rmp::behavior::Ball>({ .speed = 320 });
    CHECK(b.speed == doctest::Approx(320));
    CHECK(ball.edges == rmp::Edge::BOUNCE); // _ready set it
    CHECK_FALSE(ball.solid);

    for (int i = 0; i < 10; i++) rmp::objects::detail::update_behaviors(ball, 1.0f / 60);
    CHECK(ball.velocity.x == doctest::Approx(0));
    CHECK(ball.velocity.y == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture, "only the direction of the serve counts, not its size") {
    Loose a;
    Loose b;
    a.add<rmp::behavior::Ball>({ .speed = 320 });
    b.add<rmp::behavior::Ball>({ .speed = 320 });

    a.apply_force({ 1, 0 }); // a nudge
    b.apply_force({ 5000, 0 }); // a shove
    // apply_force lands in the integrator, so for this test the velocity is
    // written the way the integrator would have.
    a.velocity = { 0.016f, 0 };
    b.velocity = { 83.0f, 0 };
    rmp::objects::detail::update_behaviors(a, 1.0f / 60);
    rmp::objects::detail::update_behaviors(b, 1.0f / 60);

    CHECK(a.velocity.x == doctest::Approx(320));
    CHECK(b.velocity.x == doctest::Approx(320));
}

TEST_CASE_FIXTURE(Fixture, "the pace is kept exactly, whatever the direction") {
    Loose ball;
    ball.add<rmp::behavior::Ball>({ .speed = 200 });
    ball.velocity = { 3, 4 }; // length 5
    rmp::objects::detail::update_behaviors(ball, 1.0f / 60);
    const float len =
        std::sqrt(ball.velocity.x * ball.velocity.x + ball.velocity.y * ball.velocity.y);
    CHECK(len == doctest::Approx(200));
    // And the direction is untouched.
    CHECK(ball.velocity.x / ball.velocity.y == doctest::Approx(3.0f / 4));
}

TEST_CASE_FIXTURE(Fixture, "stopping a ball between points is one assignment") {
    Loose ball;
    ball.add<rmp::behavior::Ball>({ .speed = 200 });
    ball.velocity = { 200, 0 };
    ball.velocity = {};
    for (int i = 0; i < 5; i++) rmp::objects::detail::update_behaviors(ball, 1.0f / 60);
    CHECK(ball.velocity.x == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture, "where it hits the paddle decides the angle") {
    // Half of why Pong feels like Pong: the end of the paddle sends the ball
    // off at an angle, and the player learns that without being told.
    World world;
    // Bounds given explicitly and well away from them. The first version of
    // this test put everything at y = 0, which is the TOP EDGE of the default
    // view -- so Edge::BOUNCE, which Ball::_ready turns on, was flipping the
    // ball before the paddle ever touched it, and the "flat bounce" assertion
    // was reading the wall's answer.
    const Rectangle arena{ -500, -500, 1000, 1000 };
    auto &paddle =
        world.spawn({ .position = { 100, 0 }, .shape = rmp::rect({ 10, 80 }) });
    (void)paddle;

    auto make_ball = [&](float y) -> rmp::Object & {
        auto &ball = world.spawn(
            { .position = { 90, y }, .shape = rmp::circle(5), .bounds = arena });
        ball.add<rmp::behavior::Ball>({ .speed = 300, .max_bounce_deg = 60 });
        ball.velocity = { 300, 0 };
        return ball;
    };

    auto &middle = make_ball(0);
    auto &edge = make_ball(36);
    frame(world, 1.0f / 600);

    // Dead centre comes back flat; the end comes back steeply.
    CHECK(std::fabs(middle.velocity.y) < 20);
    CHECK(std::fabs(edge.velocity.y) > 100);
    // Both reversed, and both kept their pace.
    CHECK(middle.velocity.x < 0);
    CHECK(std::sqrt(edge.velocity.x * edge.velocity.x +
                    edge.velocity.y * edge.velocity.y) ==
          doctest::Approx(300).epsilon(0.02));
}

TEST_CASE_FIXTURE(Fixture, "two bounces in a corner do not leave it stuck") {
    World world;
    auto &ball = world.spawn({ .position = { 20, 20 },
                               .shape = rmp::circle(5),
                               .edges = rmp::Edge::BOUNCE,
                               .bounds = { 0, 0, 200, 200 } });
    ball.add<rmp::behavior::Ball>({ .speed = 400 });
    ball.velocity = { -400, -400 };

    for (int i = 0; i < 120; i++) frame(world, 1.0f / 60);
    // It is still inside, still moving, and still at its pace.
    const Rectangle box = ball.world_collider();
    CHECK(box.x >= -kEps);
    CHECK(box.y >= -kEps);
    const float len =
        std::sqrt(ball.velocity.x * ball.velocity.x + ball.velocity.y * ball.velocity.y);
    CHECK(len == doctest::Approx(400).epsilon(0.02));
}

// ---------------------------------------------------------------------------
// Lifespan and Timer — the exact frame, not one either side
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "lifespan destroys on the frame it says and not before") {
    World world;
    auto &object = world.spawn();
    object.add<rmp::behavior::Lifespan>({ .seconds = 1.0f });
    // A handle, because frame() ends in collect() and the reference does not
    // survive the frame the object dies in.
    const rmp::Handle<rmp::Object> alive = object.handle();

    // A quarter of a second, four times, and NOT sixty frames of 1/60: 1.0f/60
    // is not representable, so summing it sixty times lands a hair under 1.0
    // and the object survived a frame longer. That is float arithmetic and not
    // a fault in the behavior -- asking a test to prove "the exact frame" with
    // a step that cannot sum exactly is asking it to prove something false.
    // 0.25 is exact in binary, so this really is the frame it says.
    for (int i = 0; i < 3; i++) frame(world, 0.25f);
    CHECK(static_cast<bool>(alive)); // 0.75
    frame(world, 0.25f);
    CHECK_FALSE(static_cast<bool>(alive)); // exactly 1.0
    CHECK(world.object_count() == 0);

    SUBCASE("and at an ordinary frame rate it is within one frame either way") {
        // The guarantee that actually matters to a game, stated as a game would
        // experience it.
        World other;
        auto &particle = other.spawn();
        particle.add<rmp::behavior::Lifespan>({ .seconds = 1.0f });
        const rmp::Handle<rmp::Object> live = particle.handle();
        for (int i = 0; i < 59; i++) frame(other, 1.0f / 60);
        CHECK(static_cast<bool>(live));
        for (int i = 0; i < 2; i++) frame(other, 1.0f / 60);
        CHECK_FALSE(static_cast<bool>(live));
    }
}

TEST_CASE_FIXTURE(Fixture, "a timer fires on the exact frame, and repeats") {
    Loose object;
    int fired = 0;
    object.add<rmp::behavior::Timer>({ .seconds = 0.25f,
                                       .repeat = true,
                                       .on_timeout = [&](rmp::Object &) { fired++; } });

    for (int i = 0; i < 14; i++)
        rmp::objects::detail::update_behaviors(object, 1.0f / 60);
    CHECK(fired == 0); // 0.2333
    rmp::objects::detail::update_behaviors(object, 1.0f / 60);
    CHECK(fired == 1); // 0.25 exactly

    for (int i = 0; i < 15; i++)
        rmp::objects::detail::update_behaviors(object, 1.0f / 60);
    CHECK(fired == 2);
}

TEST_CASE_FIXTURE(Fixture, "a timer that does not repeat fires once, ever") {
    Loose object;
    int fired = 0;
    object.add<rmp::behavior::Timer>({ .seconds = 0.1f,
                                       .repeat = false,
                                       .on_timeout = [&](rmp::Object &) { fired++; } });
    for (int i = 0; i < 100; i++)
        rmp::objects::detail::update_behaviors(object, 1.0f / 60);
    CHECK(fired == 1);
}

TEST_CASE_FIXTURE(Fixture, "one long frame owes several ticks, and pays them") {
    // A countdown that dropped ticks would run slow on a slow machine, which is
    // the same class of bug [app] max_delta exists for -- and max_delta is what
    // bounds how many can ever be owed at once.
    Loose object;
    int fired = 0;
    object.add<rmp::behavior::Timer>(
        { .seconds = 0.1f, .on_timeout = [&](rmp::Object &) { fired++; } });
    rmp::objects::detail::update_behaviors(object, 0.55f);
    CHECK(fired == 5);
}

// ---------------------------------------------------------------------------
// Runner — speed, acceleration and distance
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "speed climbs by exactly accelerate per second") {
    Loose object;
    auto &runner = object.add<rmp::behavior::Runner>(
        { .speed = 300, .accelerate = 60, .max_speed = 900 });
    for (int i = 0; i < 60; i++)
        rmp::objects::detail::update_behaviors(object, 1.0f / 60);
    CHECK(runner.speed == doctest::Approx(360).epsilon(0.001));
}

TEST_CASE_FIXTURE(Fixture, "and stops at max_speed") {
    Loose object;
    auto &runner = object.add<rmp::behavior::Runner>(
        { .speed = 300, .accelerate = 1000, .max_speed = 500 });
    for (int i = 0; i < 120; i++)
        rmp::objects::detail::update_behaviors(object, 1.0f / 60);
    CHECK(runner.speed == doctest::Approx(500));
}

TEST_CASE_FIXTURE(Fixture, "distance is the integral of the speed, not the frame count") {
    Loose object;
    auto &runner = object.add<rmp::behavior::Runner>({ .speed = 100, .accelerate = 0 });
    for (int i = 0; i < 60; i++)
        rmp::objects::detail::update_behaviors(object, 1.0f / 60);
    CHECK(runner.distance() == doctest::Approx(100).epsilon(0.001));

    SUBCASE("and with acceleration it is the area under the ramp") {
        Loose other;
        auto &ramp = other.add<rmp::behavior::Runner>({ .speed = 0, .accelerate = 100 });
        for (int i = 0; i < 60; i++) {
            rmp::objects::detail::update_behaviors(other, 1.0f / 60);
        }
        // v goes 0 -> 100 over a second, so the distance is about 50, not 100
        // and not 60. A count of frames would give neither.
        CHECK(ramp.distance() == doctest::Approx(50).epsilon(0.02));
    }
}

// ---------------------------------------------------------------------------
// Spawner — the test that justifies it existing
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "by distance keeps the spacing even as the speed climbs") {
    // THE reason this is not a Timer. With the speed rising, "every N seconds"
    // spaces the obstacles further and further apart and the game gets EASIER
    // the faster you go, which is the opposite of the intention.
    World world;
    std::vector<float> by_distance;
    std::vector<float> by_time;

    auto run = [&](bool distance_mode, std::vector<float> &at) {
        World local;
        auto &mover = local.spawn({ .position = { 0, 0 } });
        auto &source = local.spawn();
        auto &spawner = source.add<rmp::behavior::Spawner>({
            .every_seconds = distance_mode ? 0.0f : 0.5f,
            .every_distance = distance_mode ? 200.0f : 0.0f,
            .track = mover.handle(),
            .on_spawn = [&](rmp::Scene &, Vector2 where) { at.push_back(where.x); },
        });
        (void)spawner;
        float speed = 100;
        for (int i = 0; i < 600; i++) {
            speed += 200 * (1.0f / 60); // the runner accelerating
            mover.position.x += speed * (1.0f / 60);
            rmp::objects::detail::update(local, 1.0f / 60);
        }
    };
    run(true, by_distance);
    run(false, by_time);

    REQUIRE(by_distance.size() > 4);
    REQUIRE(by_time.size() > 4);

    auto gaps = [](const std::vector<float> &at) {
        std::vector<float> out;
        for (std::size_t i = 1; i < at.size(); i++) out.push_back(at[i] - at[i - 1]);
        return out;
    };
    const std::vector<float> d_gaps = gaps(by_distance);
    const std::vector<float> t_gaps = gaps(by_time);

    // The property is NO DRIFT, and the average is what says so. An individual
    // gap wobbles by up to one frame of travel -- the threshold is crossed
    // somewhere inside a step, and the step gets longer as the speed climbs --
    // but the SCHEDULE does not move, so the wobbles cancel and the mean stays
    // put. Asserting each gap to within 10 % failed on 225 followed by 172,
    // which is two halves of a correct 200 rather than a fault.
    auto mean = [](const std::vector<float> &v) {
        float total = 0;
        for (float x : v) total += x;
        return total / static_cast<float>(v.size());
    };
    CHECK(mean(d_gaps) == doctest::Approx(200).epsilon(0.02));

    // And it does not drift: the second half averages the same as the first.
    const std::size_t half = d_gaps.size() / 2;
    const std::vector<float> first(d_gaps.begin(),
                                   d_gaps.begin() + static_cast<long>(half));
    const std::vector<float> second(d_gaps.begin() + static_cast<long>(half),
                                    d_gaps.end());
    CHECK(mean(second) == doctest::Approx(mean(first)).epsilon(0.1));

    // By time, with the same speed climbing, it drifts hard -- and that is the
    // whole reason this behavior has a distance mode at all.
    CHECK(mean(std::vector<float>(t_gaps.begin() + static_cast<long>(t_gaps.size() / 2),
                                  t_gaps.end())) >
          mean(std::vector<float>(
              t_gaps.begin(), t_gaps.begin() + static_cast<long>(t_gaps.size() / 2))) *
              2);
}

TEST_CASE_FIXTURE(Fixture, "max_alive is a cap on how many it has made") {
    World world;
    int made = 0;
    auto &source = world.spawn();
    source.add<rmp::behavior::Spawner>({
        .every_seconds = 0.1f,
        .max_alive = 3,
        .on_spawn =
            [&](rmp::Scene &scene, Vector2 where) {
                scene.spawn({ .position = where });
                made++;
            },
    });
    for (int i = 0; i < 300; i++) frame(world, 1.0f / 60);
    CHECK(made == 3);
}

TEST_CASE_FIXTURE(Fixture, "jitter stays inside its range") {
    World world;
    std::vector<float> at;
    auto &mover = world.spawn();
    auto &source = world.spawn();
    source.add<rmp::behavior::Spawner>({
        .every_distance = 100,
        .jitter = 20,
        .track = mover.handle(),
        .on_spawn = [&](rmp::Scene &, Vector2 where) { at.push_back(where.x); },
    });
    for (int i = 0; i < 2000; i++) {
        mover.position.x += 5;
        rmp::objects::detail::update(world, 1.0f / 60);
    }
    REQUIRE(at.size() > 10);
    for (std::size_t i = 2; i < at.size(); i++) {
        const float gap = at[i] - at[i - 1];
        CHECK(gap >= 100 - 20 - 6); // the jitter, plus one step of granularity
        CHECK(gap <= 100 + 20 + 6);
    }
}

// ---------------------------------------------------------------------------
// Tween
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "every easing is 0 at the start and 1 at the end") {
    using rmp::behavior::Ease;
    for (Ease kind : { Ease::LINEAR, Ease::IN, Ease::OUT, Ease::IN_OUT }) {
        Loose object;
        object.add<rmp::behavior::Tween>(
            { .property = rmp::behavior::Property::POSITION_X,
              .from = 0,
              .to = 100,
              .seconds = 1,
              .ease = kind });
        CHECK(object.position.x == doctest::Approx(0));
        rmp::objects::detail::update_behaviors(object, 0.5f);
        // Halfway is different per curve, but always strictly between.
        CHECK(object.position.x > 0);
        CHECK(object.position.x < 100);
        rmp::objects::detail::update_behaviors(object, 0.5f);
        // EXACTLY the destination. A platform that stops a hundredth short of
        // where the level says it should be is a gap to fall through.
        CHECK(object.position.x == 100.0f);
    }
}

TEST_CASE_FIXTURE(Fixture, "a finished tween stays finished") {
    Loose object;
    auto &tween =
        object.add<rmp::behavior::Tween>({ .property = rmp::behavior::Property::ROTATION,
                                           .from = 0,
                                           .to = 90,
                                           .seconds = 0.5f,
                                           .ease = rmp::behavior::Ease::LINEAR });
    for (int i = 0; i < 100; i++) rmp::objects::detail::update_behaviors(object, 0.1f);
    CHECK(tween.finished());
    CHECK(object.rotation == doctest::Approx(90));
}

TEST_CASE_FIXTURE(Fixture, "loop starts over and ping_pong comes back") {
    SUBCASE("loop") {
        Loose object;
        object.add<rmp::behavior::Tween>(
            { .property = rmp::behavior::Property::POSITION_X,
              .from = 0,
              .to = 100,
              .seconds = 1,
              .ease = rmp::behavior::Ease::LINEAR,
              .loop = true });
        rmp::objects::detail::update_behaviors(object, 0.75f);
        CHECK(object.position.x == doctest::Approx(75));
        rmp::objects::detail::update_behaviors(object, 0.5f); // past the end
        CHECK(object.position.x == doctest::Approx(25));
    }
    SUBCASE("ping_pong") {
        Loose object;
        object.add<rmp::behavior::Tween>(
            { .property = rmp::behavior::Property::POSITION_X,
              .from = 0,
              .to = 100,
              .seconds = 1,
              .ease = rmp::behavior::Ease::LINEAR,
              .ping_pong = true });
        rmp::objects::detail::update_behaviors(object, 0.75f);
        CHECK(object.position.x == doctest::Approx(75));
        rmp::objects::detail::update_behaviors(object, 0.5f);
        // Turned round: a quarter of the way back from the far end.
        CHECK(object.position.x == doctest::Approx(75));
        rmp::objects::detail::update_behaviors(object, 0.5f);
        CHECK(object.position.x == doctest::Approx(25));
    }
}

TEST_CASE_FIXTURE(Fixture, "a tween of zero seconds is the destination, not a NaN") {
    Loose object;
    object.add<rmp::behavior::Tween>({ .property = rmp::behavior::Property::SCALE_X,
                                       .from = 1,
                                       .to = 3,
                                       .seconds = 0 });
    rmp::objects::detail::update_behaviors(object, 0.1f);
    CHECK(object.scale.x == doctest::Approx(3));
    CHECK_FALSE(std::isnan(object.scale.x));
}

TEST_CASE_FIXTURE(Fixture, "alpha writes both the shape's colour and the sprite's tint") {
    Loose object;
    object.add<rmp::behavior::Tween>({ .property = rmp::behavior::Property::ALPHA,
                                       .from = 255,
                                       .to = 0,
                                       .seconds = 1,
                                       .ease = rmp::behavior::Ease::LINEAR });
    rmp::objects::detail::update_behaviors(object, 1.0f);
    CHECK(static_cast<int>(object.shape.color.a) == 0);
    CHECK(static_cast<int>(object.sprite.tint.a) == 0);
}

// ---------------------------------------------------------------------------
// GridSnap
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "grid_snap runs AFTER the movement, not before it") {
    // The property the whole _late_update hook exists for. Running before the
    // integrator would round the position the object had on its way in, and the
    // piece would still land on half a pixel.
    World world;
    auto &piece = world.spawn({ .position = { 0, 0 }, .velocity = { 100, 0 } });
    piece.add<rmp::behavior::GridSnap>({ .cell = { 32, 32 } });

    rmp::objects::detail::update(world, 0.5f); // would move it to x = 50
    CHECK(piece.position.x == doctest::Approx(64)); // snapped after moving
}

TEST_CASE_FIXTURE(Fixture, "it rounds to the nearest cell, not down to it") {
    World world;
    auto &piece = world.spawn({ .position = { 0, 0 } });
    piece.add<rmp::behavior::GridSnap>({ .cell = { 10, 10 } });

    SUBCASE("just past the middle goes up") {
        piece.position = { 6, 0 };
        rmp::objects::detail::update(world, 0);
        CHECK(piece.position.x == doctest::Approx(10));
    }
    SUBCASE("just short of it goes down") {
        piece.position = { 4, 0 };
        rmp::objects::detail::update(world, 0);
        CHECK(piece.position.x == doctest::Approx(0));
    }
    SUBCASE("and it works on the negative side, where flooring would drift") {
        piece.position = { -6, 0 };
        rmp::objects::detail::update(world, 0);
        CHECK(piece.position.x == doctest::Approx(-10));
    }
}

TEST_CASE_FIXTURE(Fixture, "a non-square cell uses its own size per axis") {
    World world;
    auto &piece = world.spawn({ .position = { 0, 0 } });
    piece.add<rmp::behavior::GridSnap>({ .cell = { 40, 10 } });
    piece.position = { 25, 6 };
    rmp::objects::detail::update(world, 0);
    CHECK(piece.position.x == doctest::Approx(40));
    CHECK(piece.position.y == doctest::Approx(10));
}

TEST_CASE_FIXTURE(Fixture, "the offset moves the grid, not the object") {
    World world;
    auto &piece = world.spawn({ .position = { 0, 0 } });
    piece.add<rmp::behavior::GridSnap>({ .cell = { 10, 10 }, .offset = { 5, 0 } });
    piece.position = { 8, 0 };
    rmp::objects::detail::update(world, 0);
    CHECK(piece.position.x == doctest::Approx(5));
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "damage during the invulnerable window does not land") {
    // One spike must not cost three hearts in three frames.
    Loose object;
    auto &health = object.add<rmp::behavior::Health>(
        { .hp = 3, .max_hp = 3, .invulnerable_for = 0.5f, .destroy_on_death = false });

    CHECK(health.damage(object));
    CHECK(health.hp == 2);
    CHECK(health.invulnerable());

    CHECK_FALSE(health.damage(object));
    CHECK_FALSE(health.damage(object));
    CHECK(health.hp == 2);
}

TEST_CASE_FIXTURE(Fixture, "and the window lasts exactly as long as it says") {
    Loose object;
    auto &health = object.add<rmp::behavior::Health>(
        { .hp = 3, .invulnerable_for = 0.5f, .destroy_on_death = false });
    health.damage(object);

    for (int i = 0; i < 29; i++)
        rmp::objects::detail::update_behaviors(object, 1.0f / 60);
    CHECK(health.invulnerable()); // 0.483
    CHECK_FALSE(health.damage(object));

    rmp::objects::detail::update_behaviors(object, 1.0f / 60);
    CHECK_FALSE(health.invulnerable());
    CHECK(health.damage(object));
    CHECK(health.hp == 1);
}

TEST_CASE_FIXTURE(Fixture, "on_death runs once even if two lethal hits land together") {
    Loose object;
    int deaths = 0;
    auto &health = object.add<rmp::behavior::Health>(
        { .hp = 1,
          .invulnerable_for = 0,
          .destroy_on_death = false,
          .on_death = [&](rmp::Object &) { deaths++; } });

    CHECK(health.damage(object, 5));
    CHECK_FALSE(health.damage(object, 5));
    CHECK(deaths == 1);
    CHECK(health.hp == 0);
    CHECK(health.dead());
}

TEST_CASE_FIXTURE(Fixture, "healing does not pass max_hp, and cannot revive") {
    Loose object;
    auto &health = object.add<rmp::behavior::Health>(
        { .hp = 1, .max_hp = 3, .invulnerable_for = 0, .destroy_on_death = false });
    health.heal(10);
    CHECK(health.hp == 3);

    health.damage(object, 3);
    CHECK(health.dead());
    health.heal(5);
    CHECK(health.hp == 0);
}

TEST_CASE_FIXTURE(Fixture, "the blink leaves the object visible when the window ends") {
    Loose object;
    auto &health = object.add<rmp::behavior::Health>(
        { .hp = 3, .invulnerable_for = 0.2f, .blink_hz = 20, .destroy_on_death = false });
    health.damage(object);

    bool ever_hidden = false;
    for (int i = 0; i < 20; i++) {
        rmp::objects::detail::update_behaviors(object, 1.0f / 60);
        if (!object.visible) ever_hidden = true;
    }
    CHECK(ever_hidden); // it really blinked
    CHECK(object.visible); // and it did not go out invisible
}

TEST_CASE_FIXTURE(Fixture,
                  "destroy_on_death is the default, and it is the object that goes") {
    World world;
    auto &object = world.spawn();
    auto &health = object.add<rmp::behavior::Health>({ .hp = 1, .invulnerable_for = 0 });
    health.damage(object);
    CHECK_FALSE(object.alive());
}

// ---------------------------------------------------------------------------
// Follow, Projectile
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "follow chases the handle it was given") {
    World world;
    auto &prey = world.spawn({ .position = { 300, 0 } });
    auto &hunter = world.spawn({ .position = { 0, 0 } });
    hunter.add<rmp::behavior::Follow>({ .target = prey.handle(), .speed = 100 });

    rmp::objects::detail::update_behaviors(hunter, 1.0f / 60);
    CHECK(hunter.velocity.x == doctest::Approx(100));
    CHECK(hunter.velocity.y == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture, "it stops at stop_distance rather than sitting inside") {
    World world;
    auto &prey = world.spawn({ .position = { 50, 0 } });
    auto &hunter = world.spawn({ .position = { 0, 0 } });
    hunter.add<rmp::behavior::Follow>(
        { .target = prey.handle(), .speed = 100, .stop_distance = 60 });
    rmp::objects::detail::update_behaviors(hunter, 1.0f / 60);
    CHECK(hunter.velocity.x == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture,
                  "a follow whose target dies coasts rather than stopping dead") {
    World world;
    auto &prey = world.spawn({ .position = { 300, 0 } });
    auto &hunter = world.spawn({ .position = { 0, 0 } });
    hunter.add<rmp::behavior::Follow>({ .target = prey.handle(), .speed = 100 });
    rmp::objects::detail::update_behaviors(hunter, 1.0f / 60);

    prey.destroy();
    rmp::objects::detail::collect();
    rmp::objects::detail::update_behaviors(hunter, 1.0f / 60);
    CHECK(hunter.velocity.x == doctest::Approx(100)); // still flying
}

TEST_CASE_FIXTURE(Fixture, "a projectile is discarded at the edge and on contact") {
    World world;
    auto &bullet = world.spawn({ .position = { 0, 0 }, .shape = rmp::rect({ 4, 4 }) });
    bullet.add<rmp::behavior::Projectile>({ .speed = 500 });
    CHECK(bullet.edges == rmp::Edge::DESTROY);

    SUBCASE("it keeps the pace it was given") {
        bullet.velocity = { 1, 0 };
        rmp::objects::detail::update_behaviors(bullet, 1.0f / 60);
        CHECK(bullet.velocity.x == doctest::Approx(500));
    }
    SUBCASE("and it dies on contact") {
        auto &wall =
            world.spawn({ .position = { 2, 0 }, .shape = rmp::rect({ 10, 10 }) });
        (void)wall;
        // A HANDLE and not the reference: frame() ends with collect(), which
        // frees what died, and reading `bullet.alive()` afterwards is reading
        // freed memory. It even answered correctly for a while, which is the
        // worst way for a use-after-free to behave.
        const rmp::Handle<rmp::Object> alive = bullet.handle();
        frame(world, 1.0f / 600);
        CHECK_FALSE(static_cast<bool>(alive));
        CHECK(world.object_count() == 1);
    }
}

// ---------------------------------------------------------------------------
// TopDown — direction, sector and the diagonal
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "the eight sectors map to the eight suffixes") {
    // The table in full. Getting one of these wrong shows up as a character
    // facing the wrong way in one direction out of eight, which is exactly the
    // kind of thing that survives a play test.
    struct Case {
        Vector2 facing;
        const char *suffix;
    };
    const Case cases[] = {
        { { 1, 0 }, "e" },  { { 1, -1 }, "ne" }, { { 0, -1 }, "n" }, { { -1, -1 }, "nw" },
        { { -1, 0 }, "w" }, { { -1, 1 }, "sw" }, { { 0, 1 }, "s" },  { { 1, 1 }, "se" },
    };
    for (const Case &c : cases) {
        Loose object;
        auto &top = object.add<rmp::behavior::TopDown>();
        top.ours.facing = c.facing;
        CAPTURE(c.suffix);
        CHECK(std::string(top.suffixes[top.sector()]) == std::string(c.suffix));
    }
}

TEST_CASE_FIXTURE(Fixture, "flip_x follows the facing, and only on the horizontal") {
    Loose object;
    auto &top = object.add<rmp::behavior::TopDown>();

    top.ours.facing = { -1, 0 };
    rmp::objects::detail::update_behaviors(object, 1.0f / 60);
    // No input is bound in this test, so `wanted` is zero and the facing stays
    // where it was put -- which is what makes this test about the flip alone.
    CHECK(object.flip_x);

    top.ours.facing = { 1, 0 };
    rmp::objects::detail::update_behaviors(object, 1.0f / 60);
    CHECK_FALSE(object.flip_x);
}

TEST_CASE_FIXTURE(Fixture, "acceleration of zero is instant, and of N is a ramp") {
    Loose object;
    auto &top = object.add<rmp::behavior::TopDown>({ .speed = 200, .acceleration = 100 });
    (void)top;
    // With no bound actions the target is zero, so a velocity put there by hand
    // has to ramp DOWN rather than snap.
    object.velocity = { 200, 0 };
    rmp::objects::detail::update_behaviors(object, 0.5f);
    CHECK(object.velocity.x == doctest::Approx(150));

    object.get<rmp::behavior::TopDown>()->acceleration = 0;
    rmp::objects::detail::update_behaviors(object, 0.5f);
    CHECK(object.velocity.x == doctest::Approx(0));
}
// ---------------------------------------------------------------------------
// Who owns a behavior, and for how long
// ---------------------------------------------------------------------------

namespace {

struct Tagged {
    int tag = 0;
    void _update(rmp::Object &, float) {}
};

// Two of these are two DIFFERENT behaviors, which is what the list needs:
// add<B>() twice replaces rather than stacks, so a test that wanted three
// entries and used one type twice would quietly be testing two.
template <int N> struct Runner {
    const char *name = "?";
    void _update(rmp::Object &, float) { note(name); }
};

struct Leaver {
    const char *name = "?";
    void _update(rmp::Object &self, float) {
        note(name);
        self.remove<Leaver>();
    }
};

} // namespace

TEST_CASE_FIXTURE(Fixture,
                  "an object that goes away without destroy() takes its behaviors") {
    // The owner table used to be keyed by the object's ADDRESS and nothing
    // released it from ~Object. A stack object -- which is how a behavior is
    // meant to be tested -- left a live entry keyed by freed memory, and the
    // next object at that address inherited it: somebody else's state, updated
    // as if it were its own, with its _end eventually run against the wrong
    // self.
    //
    // Placement new into one buffer rather than two heap objects and a hope:
    // the second object is AT the first one's address by construction, on every
    // allocator on all seventeen targets.
    alignas(Loose) unsigned char storage[sizeof(Loose)];

    auto *first = new (static_cast<void *>(storage)) Loose();
    first->add<Tagged>({ .tag = 42 });
    REQUIRE(rmp::objects::detail::behavior_count(*first) == 1);
    first->~Loose();

    auto *second = new (static_cast<void *>(storage)) Loose();
    REQUIRE(static_cast<const void *>(second) == static_cast<const void *>(first));
    CHECK(rmp::objects::detail::behavior_count(*second) == 0);
    CHECK(second->get<Tagged>() == nullptr);
    second->~Loose();
}

TEST_CASE_FIXTURE(Fixture,
                  "a behavior removed during the pass does not cost its neighbour") {
    // [A, B, C] where A removes itself from its own _update. Erasing from the
    // list being walked shifts B into A's index and the loop's i++ steps over
    // it -- so B silently misses the frame, and two self-removing behaviors
    // drop several.
    Loose object;
    object.add<Leaver>({ .name = "A" });
    object.add<Runner<1>>({ .name = "B" });
    object.add<Runner<2>>({ .name = "C" });

    g_log.clear();
    rmp::objects::detail::update_behaviors(object, 0.1f);
    CHECK(g_log == "A B C");
    CHECK_FALSE(object.has<Leaver>());
    CHECK(rmp::objects::detail::behavior_count(object) == 2);

    // And the compaction afterwards left the two survivors, in order.
    g_log.clear();
    rmp::objects::detail::update_behaviors(object, 0.1f);
    CHECK(g_log == "B C");
}

// ---------------------------------------------------------------------------
// The budget. The engine used to scan a list of owners on every lookup and it
// does one per behavior per pass, so a frame grew with the SQUARE of the
// object count. These two are the shape of the cost rather than a benchmark,
// and the second is the one that cannot be argued with.
// ---------------------------------------------------------------------------

namespace {

// Microseconds for one update pass over `count` objects, each carrying one
// behavior. The objects are built once and the pass is run `passes` times, so
// what is timed is the engine and not the spawning.
double pass_micros(int count, int passes) {
    World world;
    for (int i = 0; i < count; i++) world.spawn().add<Tagged>({ .tag = i });
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < passes; i++) rmp::objects::detail::update(world, 1.0f / 60);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    return static_cast<double>(elapsed.count()) / passes;
}

} // namespace

TEST_CASE_FIXTURE(Fixture,
                  "two thousand objects with a behavior each are a fraction "
                  "of a frame") {
    const double micros = pass_micros(2000, 5);
    MESSAGE("2000 objects, one behavior each: " << micros / 1000.0 << " ms per pass");
    CHECK(micros < 20000.0); // 20 ms, which is generous for a debug build
}

TEST_CASE_FIXTURE(Fixture, "and four times the objects cost about four times as much") {
    // The assertion that matters, because it does not depend on the machine: a
    // linear engine gives a ratio near 4 and the quadratic one gave 15.
    const double small = pass_micros(500, 20);
    const double large = pass_micros(2000, 5);
    const double ratio = large / (small > 1.0 ? small : 1.0);
    MESSAGE("500: " << small << " us, 2000: " << large << " us, ratio " << ratio);
    CHECK(ratio < 8.0);
}

// NOLINTEND(readability-make-member-function-const,readability-named-parameter,readability-convert-member-functions-to-static)
