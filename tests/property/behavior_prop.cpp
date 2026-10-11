// ---------------------------------------------------------------------------
// The catalogue of rmp/behavior.h, every one of the thirteen, held to the
// campaign's rules for any fields a game can set and any delta a hook can be
// handed:
//
//   - no undefined behaviour and no hang: the sanitizers watch every hook,
//     and a Timer's callback counts its calls;
//   - what a behavior writes into its object -- position, velocity, scale,
//     rotation -- is a finite number whenever the object's was: a field that
//     is NaN or an infinity, or one so large it overflows, leaves the object
//     as it was rather than spreading a NaN into the world;
//   - what a behavior keeps (`ours`, and the fields it changes itself: a
//     Runner's and a Ball's speed) is never NaN;
//   - with ordinary fields and any delta, each one does what its comment
//     says: a Tween stays between `from` and `to` and stops exactly on `to`,
//     a Ball and a Projectile keep their pace, a Runner its max_speed, a
//     Timer fires once per interval it covered, a Lifespan ends when its time
//     is up, a GridSnap lands on its grid, a Spawner keeps under its cap;
//   - and the pure halves -- TopDown's sector, Parallax's tiling, Spawner's
//     jitter -- hold for any input.
//
// The behavior ENGINE (src/rmp/behavior.cpp: attach, detach, the walks) has
// its own property at the end: any sequence of add, remove, destroy and
// update over a few objects keeps what a model of it says. Its public face is
// rmp::Object's (object.h), whose waivers are another package's, so it covers
// nothing here.
//
// A counterexample one of these finds becomes a named case in
// tests/behavior_test.cpp or tests/behaviors_test.cpp.
//
// covers: rmp::behavior::TopDown::_update(Object &, float)
// covers: rmp::behavior::TopDown::direction() const
// covers: rmp::behavior::TopDown::sector() const
// covers: rmp::behavior::Platformer::_ready(Object &)
// covers: rmp::behavior::Platformer::_update(Object &, float)
// covers: rmp::behavior::Platformer::grounded() const
// covers: rmp::behavior::Runner::_ready(Object &)
// covers: rmp::behavior::Runner::_update(Object &, float)
// covers: rmp::behavior::Runner::distance() const
// covers: rmp::behavior::Runner::ducking() const
// covers: rmp::behavior::Ball::_ready(Object &)
// covers: rmp::behavior::Ball::_collision(Object &, Object &)
// covers: rmp::behavior::Ball::_update(Object &, float)
// covers: rmp::behavior::Projectile::_ready(Object &)
// covers: rmp::behavior::Projectile::_collision(Object &, Object &)
// covers: rmp::behavior::Projectile::_update(Object &, float)
// covers: rmp::behavior::Follow::_update(Object &, float)
// covers: rmp::behavior::Tween::_update(Object &, float)
// covers: rmp::behavior::Tween::finished() const
// covers: rmp::behavior::Tween::restart()
// covers: rmp::behavior::GridSnap::_late_update(Object &, float)
// covers: rmp::behavior::Parallax::_ready(Object &)
// covers: rmp::behavior::Parallax::_update(Object &, float)
// covers: rmp::behavior::Parallax::_draw(Object &)
// covers: rmp::behavior::Spawner::_update(Object &, float)
// covers: rmp::behavior::Spawner::alive() const
// covers: rmp::behavior::Spawner::jitter_amount() const
// covers: rmp::behavior::Timer::_update(Object &, float)
// covers: rmp::behavior::Lifespan::_update(Object &, float)
// covers: rmp::behavior::Health::_ready(Object &)
// covers: rmp::behavior::Health::_update(Object &, float)
// covers: rmp::behavior::Health::_collision(Object &, Object &)
// covers: rmp::behavior::Health::_end(Object &)
// covers: rmp::behavior::Health::damage(Object &, int)
// covers: rmp::behavior::Health::heal(int)
// covers: rmp::behavior::Health::dead() const
// covers: rmp::behavior::Health::invulnerable() const
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/behavior_internal.h"
#include "../../src/rmp/internal.h"
#include "../../src/rmp/object_internal.h"

#include <rmp/assets.h> // rmp::detail::release_all
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/random.h>
#include <rmp/scene.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

// The behaviors below are interface implementations, like the catalogue's:
// their signatures belong to the contract.
// NOLINTBEGIN(readability-make-member-function-const,readability-convert-member-functions-to-static,readability-named-parameter)
namespace {

using rmp::test::gen::NOMINAL_SIZE;

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

// An object with no scene behind it: what most behaviors need, and all a
// hook is handed.
class Loose : public rmp::Object {};

// The devices the input seam reads: written by the property, not by a
// keyboard. The dot says "file state".
struct {
    rmp::input::detail::DeviceState devices;
} fake;
void fake_sample(rmp::input::detail::DeviceState &out) { out = fake.devices; }

// Everything a case may touch, fresh, and put back afterwards.
struct Session {
    Session() { reset(); }
    ~Session() {
        reset();
        rmp::detail::release_all();
        SetTraceLogLevel(LOG_INFO);
    }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    static void reset() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
        rmp::input::detail::reset();
        fake.devices = rmp::input::detail::DeviceState{};
        rmp::input::detail::set_sample_provider(fake_sample);
        rmp::detail::reset_reports_for_tests();
        rmp::random::seed(7);
        SetTraceLogLevel(LOG_NONE);
    }
};

// --- generators ---------------------------------------------------------------

// A number in [lo, hi], over the whole range from the first case: inRange
// alone shrinks with the run's size, and the early cases would only ever
// see `lo`.
float between(float lo, float hi) {
    const int k = *rc::gen::resize(NOMINAL_SIZE, rc::gen::inRange(0, 1001));
    return lo + (hi - lo) * static_cast<float>(k) / 1000.0f;
}

// A field a game set: an ordinary number in [lo, hi] -- or, when the case is
// hostile, one time in three anything at all.
float field(bool hostile, float lo, float hi) {
    if (hostile && *rc::gen::inRange(0, 3) == 0) return *rmp::test::gen::hostile_float();
    return between(lo, hi);
}

int whole(bool hostile, int lo, int hi) {
    if (hostile && *rc::gen::inRange(0, 3) == 0) {
        return *rc::gen::weightedOneOf<int>({
            { 1, rc::gen::element(INT_MIN, INT_MIN + 1, -1, 0, 1, INT_MAX - 1, INT_MAX) },
            { 1, rc::gen::arbitrary<int>() },
        });
    }
    return *rc::gen::resize(NOMINAL_SIZE, rc::gen::inRange(lo, hi + 1));
}

bool ordinary_delta(float delta) { return delta >= 0.0f && delta <= 0.1f; }

// What a case hands the behaviors: ordinary fields or any at all, ordinary
// frames or any delta at all -- and whether a delta that was not a frame has
// been handed yet. The dot says "file state".
struct {
    bool hostile_fields = false;
    bool hostile_deltas = false;
    bool odd_delta = false;
} mode;

// The seconds a hook is handed: a frame, or -- when the case's deltas are
// hostile -- one time in three anything at all.
float any_delta() {
    float delta = between(0.0f, 0.1f);
    if (mode.hostile_deltas && *rc::gen::inRange(0, 3) == 0) {
        delta = *rmp::test::gen::hostile_float();
    }
    if (!ordinary_delta(delta)) mode.odd_delta = true;
    return delta;
}

// Where an object starts: finite and ordinary, so whatever is not finite
// afterwards is the behavior's doing.
void place(rmp::Object &o) {
    o.position = { between(-1000, 1000), between(-1000, 1000) };
    o.velocity = { between(-500, 500), between(-500, 500) };
    o.scale = { between(0.25f, 4), between(0.25f, 4) };
    o.rotation = between(-360, 360);
    o.shape = rmp::rect({ between(1, 64), between(1, 64) });
}

// --- what every hook keeps ---------------------------------------------------

void finite_object(const rmp::Object &o) {
    RC_ASSERT(std::isfinite(o.position.x));
    RC_ASSERT(std::isfinite(o.position.y));
    RC_ASSERT(std::isfinite(o.velocity.x));
    RC_ASSERT(std::isfinite(o.velocity.y));
    RC_ASSERT(std::isfinite(o.scale.x));
    RC_ASSERT(std::isfinite(o.scale.y));
    RC_ASSERT(std::isfinite(o.rotation));
}

void a_number(float value) { RC_ASSERT(!std::isnan(value)); }

float length(Vector2 v) { return std::hypot(v.x, v.y); }

// Two speeds the same to float's rounding, over a normalise and a multiply.
bool same_pace(float a, float b) {
    return std::fabs(a - b) <= 1e-3f * std::max(1.0f, std::fabs(b));
}

// The keys the factory actions read, pressed at random: move_* and ui_accept.
void press_some_keys(bool hostile) {
    for (const int key : { KEY_A, KEY_D, KEY_W, KEY_S, KEY_SPACE }) {
        fake.devices.keys.at(static_cast<std::size_t>(key)) = *rc::gen::arbitrary<bool>();
    }
    for (const int axis : { GAMEPAD_AXIS_LEFT_X, GAMEPAD_AXIS_LEFT_Y }) {
        fake.devices.axes.at(static_cast<std::size_t>(axis)) =
            hostile ? *rmp::test::gen::hostile_float() : between(-1, 1);
    }
    rmp::input::detail::begin_frame();
}

// --- the thirteen ----------------------------------------------------------------

enum class Kind {
    TOP_DOWN,
    PLATFORMER,
    RUNNER,
    BALL,
    PROJECTILE,
    FOLLOW,
    TWEEN,
    GRID_SNAP,
    PARALLAX,
    SPAWNER,
    TIMER,
    LIFESPAN,
    HEALTH,
};
constexpr int KINDS = 13;

constexpr int STEPS = 24;
constexpr float FRAME_STEP = 1.0f / 60;

void top_down() {
    const bool hostile = mode.hostile_fields;
    Loose o;
    place(o);
    rmp::behavior::TopDown b;
    b.speed = field(hostile, 0, 600);
    b.eight_way = *rc::gen::arbitrary<bool>();
    b.acceleration = *rc::gen::arbitrary<bool>() ? 0.0f : field(hostile, 0, 4000);
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        press_some_keys(hostile);
        b._update(o, any_delta());
        finite_object(o);
        const Vector2 facing = b.direction();
        RC_ASSERT(std::isfinite(facing.x));
        RC_ASSERT(std::isfinite(facing.y));
        RC_ASSERT(std::fabs(length(facing) - 1.0f) < 1e-3f);
        const int sector = b.sector();
        RC_ASSERT(sector >= 0);
        RC_ASSERT(sector < 8);
    }
}

void platformer() {
    const bool hostile = mode.hostile_fields;
    World world;
    rmp::Object &o =
        world.spawn({ .position = { 0, 80 }, .shape = rmp::rect({ 10, 20 }) });
    if (*rc::gen::arbitrary<bool>()) {
        rmp::Object &floor =
            world.spawn({ .position = { 0, 100 }, .shape = rmp::rect({ 400, 20 }) });
        floor.solid = true;
        floor.immovable = true;
    }
    rmp::behavior::Platformer b;
    b.speed = field(hostile, 0, 600);
    b.acceleration = *rc::gen::arbitrary<bool>() ? 0.0f : field(hostile, 0, 4000);
    b.gravity = field(hostile, 0, 4000);
    b.jump = field(hostile, 0, 1200);
    b.air_jumps = whole(hostile, 0, 3);
    b.coyote_time = field(hostile, 0, 0.3f);
    b.jump_buffer = field(hostile, 0, 0.3f);
    b._ready(o);
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        press_some_keys(false);
        b._update(o, any_delta());
        finite_object(o);
        a_number(b.ours.since_grounded);
        a_number(b.ours.since_jump_pressed);
        RC_ASSERT(b.ours.jumps_used >= 0);
        if (b.air_jumps >= 0 && b.air_jumps < INT_MAX) {
            RC_ASSERT(b.ours.jumps_used <= b.air_jumps + 1);
        }
        (void)b.grounded();
    }
}

void runner() {
    const bool hostile = mode.hostile_fields;
    World world;
    rmp::Object &o =
        world.spawn({ .position = { 0, 80 }, .shape = rmp::rect({ 10, 20 }) });
    rmp::behavior::Runner b;
    b.speed = field(hostile, 0, 900);
    b.accelerate = field(hostile, 0, 50);
    b.max_speed = field(hostile, 0, 2000);
    b.gravity = field(hostile, 0, 4000);
    b.jump = field(hostile, 0, 1200);
    b.air_jumps = whole(hostile, 0, 3);
    b.duck_action = *rc::gen::arbitrary<bool>() ? "move_down" : "";
    const bool speed_set_nan = std::isnan(b.speed); // the game's, not the runner's
    b._ready(o);
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        press_some_keys(false);
        const float before = b.distance();
        const float delta = any_delta();
        b._update(o, delta);
        finite_object(o);
        if (!speed_set_nan) a_number(b.speed);
        a_number(b.distance());
        RC_ASSERT(b.ducking() ==
                  (!b.duck_action.empty() && rmp::input::pressed(b.duck_action)));
        RC_ASSERT(b.ours.jumps_used >= 0);
        if (!hostile) {
            RC_ASSERT(b.speed <= b.max_speed);
            RC_ASSERT(b.distance() >= before);
            RC_ASSERT(o.velocity.x == b.speed);
        }
    }
}

void ball() {
    const bool hostile = mode.hostile_fields;
    Loose o;
    place(o);
    Loose other;
    place(other);
    rmp::behavior::Ball b;
    b.speed = field(hostile, 0, 800);
    b.max_bounce_deg = field(hostile, 0, 89);
    b.speed_up = field(hostile, 0.9f, 1.2f);
    const bool speed_set_nan = std::isnan(b.speed); // the game's, not the ball's
    b._ready(o);
    RC_ASSERT(o.edges == rmp::Edge::BOUNCE);
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        if (*rc::gen::arbitrary<bool>()) {
            other.solid = *rc::gen::arbitrary<bool>();
            other.position = { o.position.x + between(-40, 40),
                               o.position.y + between(-40, 40) };
            b._collision(o, other);
        } else {
            b._update(o, any_delta());
            if (!hostile && length(o.velocity) > 1e-3f) {
                RC_ASSERT(same_pace(length(o.velocity), b.speed));
            }
        }
        finite_object(o);
        if (!speed_set_nan) a_number(b.speed);
    }
}

void projectile() {
    const bool hostile = mode.hostile_fields;
    Loose o;
    place(o);
    Loose other;
    place(other);
    rmp::behavior::Projectile b;
    b.speed = *rc::gen::arbitrary<bool>() ? 0.0f : field(hostile, 0, 1000);
    b.destroy_on_hit = *rc::gen::arbitrary<bool>();
    b._ready(o);
    RC_ASSERT(o.edges == rmp::Edge::DESTROY);
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        if (*rc::gen::inRange(0, 4) == 0) {
            b._collision(o, other);
            if (b.destroy_on_hit) RC_ASSERT(!o.alive());
        } else {
            const Vector2 was = o.velocity;
            b._update(o, any_delta());
            if (!hostile && b.speed > 0 && length(was) > 1e-3f) {
                RC_ASSERT(same_pace(length(o.velocity), b.speed));
            }
            if (!hostile && b.speed <= 0) {
                RC_ASSERT(o.velocity.x == was.x);
                RC_ASSERT(o.velocity.y == was.y);
            }
        }
        finite_object(o);
    }
}

void follow() {
    const bool hostile = mode.hostile_fields;
    World world;
    rmp::Object &o = world.spawn();
    place(o);
    rmp::Object &target = world.spawn();
    place(target);
    rmp::behavior::Follow b;
    b.target = target.handle();
    b.speed = field(hostile, 0, 600);
    b.stop_distance = field(hostile, 0, 200);
    b.acceleration = *rc::gen::arbitrary<bool>() ? 0.0f : field(hostile, 0, 4000);
    bool gone = false;
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        switch (*rc::gen::inRange(0, 6)) {
            case 0:
                // `target` is a reference into the scene: once it is gone,
                // only the handle may be asked about it.
                if (!gone)
                    target.position = { between(-1000, 1000), between(-1000, 1000) };
                break;
            case 1:
                if (!gone) {
                    target.destroy();
                    rmp::objects::detail::collect();
                    gone = true;
                }
                break;
            default:
                break;
        }
        const Vector2 was = o.velocity;
        b._update(o, any_delta());
        finite_object(o);
        if (gone) {
            // The target died: it coasts.
            RC_ASSERT(o.velocity.x == was.x);
            RC_ASSERT(o.velocity.y == was.y);
        } else if (!hostile && b.acceleration == 0) {
            const float distance = length(
                { target.position.x - o.position.x, target.position.y - o.position.y });
            if (distance <= b.stop_distance) {
                RC_ASSERT(length(o.velocity) == 0.0f);
            } else if (distance > 1e-3f) {
                RC_ASSERT(same_pace(length(o.velocity), b.speed));
            }
        }
    }
}

// The field a Tween writes, as it reads back.
float tweened(const rmp::Object &o, rmp::behavior::Property property) {
    using P = rmp::behavior::Property;
    switch (property) {
        case P::POSITION_X:
            return o.position.x;
        case P::POSITION_Y:
            return o.position.y;
        case P::SCALE_X:
            return o.scale.x;
        case P::SCALE_Y:
            return o.scale.y;
        case P::ROTATION:
            return o.rotation;
        case P::ALPHA:
            return o.shape.color.a;
    }
    return 0;
}

void tween() {
    const bool hostile = mode.hostile_fields;
    Loose o;
    place(o);
    rmp::behavior::Tween b;
    b.property = static_cast<rmp::behavior::Property>(*rc::gen::inRange(0, 6));
    const bool alpha = b.property == rmp::behavior::Property::ALPHA;
    b.from = alpha ? field(hostile, 0, 255) : field(hostile, -1000, 1000);
    b.to = alpha ? field(hostile, 0, 255) : field(hostile, -1000, 1000);
    b.seconds = *rc::gen::inRange(0, 8) == 0 ? 0.0f : field(hostile, 0.01f, 2);
    b.ease = static_cast<rmp::behavior::Ease>(*rc::gen::inRange(0, 4));
    b.loop = *rc::gen::arbitrary<bool>();
    b.ping_pong = *rc::gen::arbitrary<bool>();
    // Within the two ends, to a few units of rounding: the alpha is an
    // unsigned char, written from the float.
    const float low = std::min(b.from, b.to) - (alpha ? 1.0f : 1e-3f);
    const float high = std::max(b.from, b.to) + (alpha ? 1.0f : 1e-3f);
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        if (*rc::gen::inRange(0, 10) == 0) {
            b.restart();
            RC_ASSERT(!b.finished());
            RC_ASSERT(b.ours.elapsed == 0.0f);
        }
        b._update(o, any_delta());
        finite_object(o);
        a_number(b.ours.elapsed);
        if (!hostile) {
            const float value = tweened(o, b.property);
            RC_ASSERT(value >= low);
            RC_ASSERT(value <= high);
            if (b.finished()) {
                // Only a tween that ends does, and one of no time at all.
                RC_ASSERT((!b.loop && !b.ping_pong) || b.seconds == 0);
                if (!alpha) RC_ASSERT(value == b.to);
            }
        }
    }
}

void grid_snap() {
    const bool hostile = mode.hostile_fields;
    Loose o;
    place(o);
    rmp::behavior::GridSnap b;
    b.cell = { *rc::gen::inRange(0, 6) == 0 ? 0.0f : field(hostile, 0.5f, 64),
               *rc::gen::inRange(0, 6) == 0 ? 0.0f : field(hostile, 0.5f, 64) };
    b.offset = { field(hostile, -100, 100), field(hostile, -100, 100) };
    const int steps = *rc::gen::inRange(1, 6);
    for (int i = 0; i < steps; ++i) {
        o.position = { between(-1000, 1000), between(-1000, 1000) };
        const Vector2 was = o.position;
        b._late_update(o, any_delta());
        finite_object(o);
        if (!hostile) {
            for (const auto &[before, after, cell, offset] :
                 { std::array{ was.x, o.position.x, b.cell.x, b.offset.x },
                   std::array{ was.y, o.position.y, b.cell.y, b.offset.y } }) {
                if (cell <= 0) {
                    RC_ASSERT(after == before);
                    continue;
                }
                // To the nearest line of the grid: never more than half a cell.
                RC_ASSERT(std::fabs(after - before) <= cell / 2 + 1e-3f);
                const float steps_along = (after - offset) / cell;
                RC_ASSERT(std::fabs(steps_along - std::round(steps_along)) < 1e-3f);
            }
            // Snapped is snapped: a second pass moves nothing.
            const Vector2 snapped = o.position;
            b._late_update(o, 0);
            RC_ASSERT(std::fabs(o.position.x - snapped.x) <= 1e-3f);
            RC_ASSERT(std::fabs(o.position.y - snapped.y) <= 1e-3f);
        }
    }
}

void parallax() {
    const bool hostile = mode.hostile_fields;
    Loose o;
    place(o);
    rmp::behavior::Parallax b;
    b.factor = field(hostile, 0, 1);
    b.speed = field(hostile, -200, 200);
    b.y = field(hostile, -500, 500);
    b._ready(o); // no texture: nothing is loaded, and _draw draws nothing
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        b._update(o, any_delta());
        b._draw(o);
        finite_object(o);
        a_number(b.ours.scroll);
    }
}

void spawner() {
    const bool hostile = mode.hostile_fields;
    World world;
    rmp::Object &o = world.spawn();
    place(o);
    rmp::Object &tracked = world.spawn();
    place(tracked);
    rmp::behavior::Spawner b;
    b.every_seconds = field(hostile, 0, 0.5f);
    b.every_distance = *rc::gen::arbitrary<bool>() ? 0.0f : field(hostile, 0, 300);
    b.jitter = *rc::gen::arbitrary<bool>() ? 0.0f : field(hostile, 0, 0.1f);
    b.max_alive =
        hostile ? whole(true, 0, 8) : whole(false, 0, rmp::behavior::MAX_SPAWNED);
    if (*rc::gen::arbitrary<bool>()) b.track = tracked.handle();
    int made = 0;
    b.on_spawn = [&made](rmp::Scene &scene, Vector2 at) {
        RC_ASSERT(std::isfinite(at.x));
        RC_ASSERT(std::isfinite(at.y));
        scene.spawn({ .position = at });
        ++made;
    };
    const int cap = std::min(b.max_alive, rmp::behavior::MAX_SPAWNED);
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        if (*rc::gen::inRange(0, 3) == 0) {
            tracked.position.x += between(0, 200);
            o.position.x += between(0, 200);
        }
        b._update(o, any_delta());
        a_number(b.ours.countdown);
        a_number(b.ours.next_at);
        a_number(b.ours.travelled);
        if (b.max_alive > 0) RC_ASSERT(b.alive() <= cap);
        RC_ASSERT(b.alive() <= made);
        finite_object(o);
    }
}

void timer() {
    const bool hostile = mode.hostile_fields;
    Loose o;
    place(o);
    rmp::behavior::Timer b;
    b.seconds = *rc::gen::inRange(0, 8) == 0 ? 0.0f : field(hostile, 0.01f, 2);
    b.repeat = *rc::gen::arbitrary<bool>();
    const bool dies = *rc::gen::inRange(0, 4) == 0;
    // A hang is a failure, not a test that never ends: the callback counts.
    static int calls = 0;
    static bool destroys = false;
    calls = 0;
    destroys = dies;
    b.on_timeout = [](rmp::Object &self) {
        ++calls;
        RC_ASSERT(calls <= 100000);
        if (destroys) self.destroy();
    };
    double covered = 0; // the seconds it was handed, as it counts them
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        const float delta = any_delta();
        const int before = calls;
        b._update(o, delta);
        if (std::isfinite(delta) && delta >= 0) covered += delta;
        a_number(b.ours.elapsed);
        finite_object(o);
        if (b.seconds > 0 && std::isfinite(b.seconds) && !b.ours.spent && o.alive()) {
            RC_ASSERT(b.ours.elapsed >= 0.0f);
            RC_ASSERT(b.ours.elapsed < b.seconds);
        }
        if (!b.repeat) RC_ASSERT(calls <= 1);
        if (!hostile && ordinary_delta(delta) && b.seconds > 0) {
            // A frame of a tenth of a second owes at most ten of the
            // shortest interval, and one more for the rounding.
            RC_ASSERT(calls - before <= static_cast<int>(0.1f / b.seconds) + 1);
        }
    }
    if (!hostile && !mode.odd_delta && b.repeat && !dies && b.seconds > 0) {
        // Once for every interval it covered, give or take the last one.
        const auto owed = static_cast<int>(covered / b.seconds);
        RC_ASSERT(std::abs(calls - owed) <= 1);
    }
}

void lifespan() {
    const bool hostile = mode.hostile_fields;
    Loose o;
    place(o);
    rmp::behavior::Lifespan b;
    b.seconds = field(hostile, 0, 2);
    double lived = 0;
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        const float delta = any_delta();
        b._update(o, delta);
        if (std::isfinite(delta) && delta >= 0) lived += delta;
        a_number(b.ours.elapsed);
        finite_object(o);
        if (!hostile && std::fabs(lived - b.seconds) > 1e-4) {
            RC_ASSERT(o.alive() == (lived < b.seconds));
        }
    }
}

void health() {
    const bool hostile = mode.hostile_fields;
    Loose o;
    place(o);
    Loose other;
    place(other);
    rmp::behavior::Health b;
    b.hp = whole(hostile, 1, 10);
    b.max_hp = whole(hostile, 0, 10);
    b.invulnerable_for = field(hostile, 0, 1);
    b.blink_hz = field(hostile, 0, 30);
    b.destroy_on_death = *rc::gen::arbitrary<bool>();
    b.hurt_by = *rc::gen::arbitrary<unsigned>();
    b.damage_on_hit = whole(hostile, 0, 3);
    static int deaths = 0;
    deaths = 0;
    b.on_death = [](rmp::Object &) { ++deaths; };
    b.on_damage = [](rmp::Object &, int amount) { RC_ASSERT(amount > 0); };
    b._ready(o);
    const int steps = *rc::gen::inRange(1, STEPS);
    for (int i = 0; i < steps; ++i) {
        const bool died_before = b.ours.died;
        const int hp_before = b.hp;
        switch (*rc::gen::inRange(0, 4)) {
            case 0: {
                const int amount = whole(hostile, -2, 5);
                const bool landed = b.damage(o, amount);
                if (landed) {
                    RC_ASSERT(amount > 0);
                    RC_ASSERT(b.hp >= 0);
                    // Down by the amount, to 0 at the least -- and up to 0
                    // from an hp a game set below it.
                    if (hp_before > 0) RC_ASSERT(b.hp < hp_before);
                }
                break;
            }
            case 1: {
                const int amount = whole(hostile, -2, 5);
                b.heal(amount);
                if (died_before || amount <= 0) RC_ASSERT(b.hp == hp_before);
                if (!died_before && amount > 0) {
                    RC_ASSERT(b.hp >= std::min(hp_before, b.max_hp));
                    RC_ASSERT(b.hp <= std::max(hp_before, b.max_hp));
                }
                break;
            }
            case 2:
                other.collision_layer = *rc::gen::arbitrary<unsigned>();
                b._collision(o, other);
                break;
            default:
                b._update(o, any_delta());
                break;
        }
        a_number(b.ours.invulnerable_left);
        RC_ASSERT(deaths <= 1);
        if (b.ours.died) RC_ASSERT(b.dead());
        if (died_before) RC_ASSERT(b.hp == hp_before);
        (void)b.invulnerable();
        finite_object(o);
    }
    b._end(o);
    RC_ASSERT(o.visible || !b.ours.hid);
}

void run(Kind kind) {
    switch (kind) {
        case Kind::TOP_DOWN:
            top_down();
            return;
        case Kind::PLATFORMER:
            platformer();
            return;
        case Kind::RUNNER:
            runner();
            return;
        case Kind::BALL:
            ball();
            return;
        case Kind::PROJECTILE:
            projectile();
            return;
        case Kind::FOLLOW:
            follow();
            return;
        case Kind::TWEEN:
            tween();
            return;
        case Kind::GRID_SNAP:
            grid_snap();
            return;
        case Kind::PARALLAX:
            parallax();
            return;
        case Kind::SPAWNER:
            spawner();
            return;
        case Kind::TIMER:
            timer();
            return;
        case Kind::LIFESPAN:
            lifespan();
            return;
        case Kind::HEALTH:
            health();
            return;
    }
}

rc::Gen<Kind> any_kind() {
    return rc::gen::map(rc::gen::resize(NOMINAL_SIZE, rc::gen::inRange(0, KINDS)),
                        [](int k) { return static_cast<Kind>(k); });
}

// --- the engine -------------------------------------------------------------------

// What the behaviors of the engine's property said, in order: a letter for
// the behavior and one for the hook. The dot says "file state".
struct {
    std::string said;
} journal;
void note(char who, char hook) {
    journal.said += who;
    journal.said += hook;
    journal.said += ' ';
}

// Plain behaviors, one per letter, that say every hook they are given.
template <char NAME> struct Plain {
    void _ready(rmp::Object &) { note(NAME, 'r'); }
    void _update(rmp::Object &, float) { note(NAME, 'u'); }
    void _late_update(rmp::Object &, float) { note(NAME, 'l'); }
    void _draw(rmp::Object &) { note(NAME, 'd'); }
    void _end(rmp::Object &) { note(NAME, 'e'); }
};
using PlainA = Plain<'a'>;
using PlainB = Plain<'b'>;

// One that removes itself from its own _update: a one-shot intro.
struct Quitter {
    void _ready(rmp::Object &) { note('q', 'r'); }
    void _update(rmp::Object &self, float) {
        note('q', 'u');
        self.remove<Quitter>();
    }
    void _end(rmp::Object &) { note('q', 'e'); }
};

// One that adds a b from its own _update, when the object has none.
struct Adder {
    void _ready(rmp::Object &) { note('+', 'r'); }
    void _update(rmp::Object &self, float) {
        note('+', 'u');
        if (!self.has<PlainB>()) self.add<PlainB>();
    }
    void _end(rmp::Object &) { note('+', 'e'); }
};

// One that destroys its object from its own _update.
struct Killer {
    void _ready(rmp::Object &) { note('k', 'r'); }
    void _update(rmp::Object &self, float) {
        note('k', 'u');
        self.destroy();
    }
    void _end(rmp::Object &) { note('k', 'e'); }
};

constexpr std::array<char, 5> LETTERS = { 'a', 'b', 'q', '+', 'k' };

// What the engine should have attached to one object, in order, and what it
// says each hook does -- written down a second way.
struct Attached {
    char who;
    bool dead = false;
};
struct Expected {
    std::vector<Attached> list;
    bool alive = true;
    std::string said;

    void say(char who, char hook) {
        said += who;
        said += hook;
        said += ' ';
    }
    [[nodiscard]] bool has(char who) const {
        return std::ranges::any_of(
            list, [who](const Attached &a) { return a.who == who && !a.dead; });
    }
    void detach(char who) {
        const auto it = std::ranges::find_if(
            list, [who](const Attached &a) { return a.who == who && !a.dead; });
        if (it == list.end()) return;
        list.erase(it);
        say(who, 'e');
    }
    void attach(char who) {
        detach(who); // a second add replaces the first
        list.push_back({ who });
        say(who, 'r');
    }
    void end_all() {
        for (const Attached &a : list) {
            if (!a.dead) say(a.who, 'e');
        }
        list.clear();
        alive = false;
    }
    // One pass of a hook over what was attached when it started.
    void walk(char hook) {
        const std::size_t count = list.size();
        for (std::size_t i = 0; i < count && alive; ++i) {
            if (list[i].dead) continue;
            const char who = list[i].who;
            const bool late_or_draw = hook != 'u';
            if (late_or_draw && who != 'a' && who != 'b') continue; // no such hook
            say(who, hook);
            if (hook != 'u') continue;
            if (who == 'q') {
                list[i].dead = true;
                say('q', 'e');
            } else if (who == '+') {
                if (!has('b')) {
                    list.push_back({ 'b' });
                    say('b', 'r');
                }
            } else if (who == 'k') {
                end_all();
            }
        }
        std::erase_if(list, [](const Attached &a) { return a.dead; });
    }
};

void attach_by_letter(rmp::Object &o, char who) {
    switch (who) {
        case 'a':
            o.add<PlainA>();
            break;
        case 'b':
            o.add<PlainB>();
            break;
        case 'q':
            o.add<Quitter>();
            break;
        case '+':
            o.add<Adder>();
            break;
        default:
            o.add<Killer>();
            break;
    }
}

void remove_by_letter(rmp::Object &o, char who) {
    switch (who) {
        case 'a':
            o.remove<PlainA>();
            break;
        case 'b':
            o.remove<PlainB>();
            break;
        case 'q':
            o.remove<Quitter>();
            break;
        case '+':
            o.remove<Adder>();
            break;
        default:
            o.remove<Killer>();
            break;
    }
}

bool has_by_letter(const rmp::Object &o, char who) {
    switch (who) {
        case 'a':
            return o.has<PlainA>();
        case 'b':
            return o.has<PlainB>();
        case 'q':
            return o.has<Quitter>();
        case '+':
            return o.has<Adder>();
        default:
            return o.has<Killer>();
    }
}

} // namespace

TEST_SUITE("property: behavior") {
    TEST_CASE(
        "every behavior with ordinary fields does what it says, whatever the delta") {
        rmp::test::check(
            "each of the thirteen, ordinary fields and any delta, keeps its promise", [] {
                const Session session;
                mode.hostile_fields = false;
                mode.hostile_deltas = true;
                mode.odd_delta = false;
                run(*any_kind());
                // Nothing a game did here was a mistake, unless it
                // handed a hook a delta that is not a frame.
                if (!mode.odd_delta) RC_ASSERT(rmp::detail::report_count() == 0);
            });
    }

    TEST_CASE("every behavior with any fields and any delta keeps the world finite") {
        rmp::test::check(
            "each of the thirteen, any fields and any delta: no UB, no hang, no NaN kept",
            [] {
                const Session session;
                mode.hostile_fields = true;
                mode.hostile_deltas = true;
                mode.odd_delta = false;
                run(*any_kind());
            });
    }

    TEST_CASE("TopDown's sector is one of the eight for any facing") {
        rmp::test::check("sector() is in 0..7 whatever ours.facing holds", [] {
            rmp::behavior::TopDown b;
            b.ours.facing = { *rmp::test::gen::hostile_float(),
                              *rmp::test::gen::hostile_float() };
            const int sector = b.sector();
            RC_ASSERT(sector >= 0);
            RC_ASSERT(sector < 8);
        });
    }

    TEST_CASE("Parallax's tiling covers the screen, from any shift, in bounded copies") {
        rmp::test::check(
            "parallax_tiling starts in (-width, 0] and covers the screen", [] {
                const float shift = *rmp::test::gen::hostile_float();
                const float width = *rmp::test::gen::hostile_float();
                const float screen = *rmp::test::gen::hostile_float();
                const rmp::behavior::detail::ParallaxTiling t =
                    rmp::behavior::detail::parallax_tiling(shift, width, screen);
                RC_ASSERT(t.copies >= 0);
                RC_ASSERT(t.copies <= rmp::behavior::detail::MAX_PARALLAX_COPIES);
                RC_ASSERT(std::isfinite(t.offset));
                if (!(width > 0) || !std::isfinite(width)) {
                    RC_ASSERT(t.copies == 0);
                    return;
                }
                RC_ASSERT(t.copies >= 1);
                RC_ASSERT(t.offset <= 0.0f);
                RC_ASSERT(t.offset > -width);
                const double covered = static_cast<double>(t.offset) +
                    static_cast<double>(t.copies) * static_cast<double>(width);
                if (std::isfinite(screen) &&
                    t.copies < rmp::behavior::detail::MAX_PARALLAX_COPIES) {
                    RC_ASSERT(covered >= static_cast<double>(screen) - 1e-3 * width);
                }
            });
    }

    TEST_CASE("a Spawner's jitter stays inside +/- jitter for any jitter") {
        rmp::test::check("jitter_amount() is in [-jitter, jitter], and 0 for none", [] {
            const Session session;
            rmp::behavior::Spawner b;
            b.jitter = *rmp::test::gen::hostile_float();
            for (int i = 0; i < 8; ++i) {
                const float j = b.jitter_amount();
                RC_ASSERT(std::isfinite(j));
                if (b.jitter > 0 && std::isfinite(b.jitter)) {
                    RC_ASSERT(j >= -b.jitter);
                    RC_ASSERT(j <= b.jitter);
                } else {
                    RC_ASSERT(j == 0.0f);
                }
            }
        });
    }

    TEST_CASE("the engine keeps what a model of it says, over any sequence") {
        rmp::test::check(
            "add, remove, destroy and the walks over a few objects match the model", [] {
                const Session session;
                World world;
                constexpr int OBJECTS = 3;
                std::array<rmp::Handle<rmp::Object>, OBJECTS> handles{};
                std::array<Expected, OBJECTS> models{};
                for (rmp::Handle<rmp::Object> &h : handles) h = world.spawn().handle();
                const int steps = *rc::gen::inRange(1, 40);
                for (int i = 0; i < steps; ++i) {
                    const auto which =
                        static_cast<std::size_t>(*rc::gen::inRange(0, OBJECTS));
                    const char who = *rc::gen::elementOf(LETTERS);
                    Expected &model = models.at(which);
                    const rmp::Ref<rmp::Object> object = handles.at(which).get();
                    RC_ASSERT(static_cast<bool>(object) == model.alive);
                    if (!object) continue;
                    journal.said.clear();
                    model.said.clear();
                    // Mostly adds and updates: a list several behaviors long, walked, is
                    // where a removal mid-walk can cost a neighbour its turn.
                    switch (*rc::gen::weightedElement<int>(
                        { { 4, 0 }, { 1, 1 }, { 1, 2 }, { 1, 3 }, { 1, 4 }, { 4, 5 } })) {
                        case 0:
                            attach_by_letter(*object, who);
                            model.attach(who);
                            break;
                        case 1:
                            remove_by_letter(*object, who);
                            model.detach(who);
                            break;
                        case 2:
                            object->destroy();
                            model.end_all();
                            break;
                        case 3:
                            rmp::objects::detail::late_update_behaviors(*object,
                                                                        FRAME_STEP);
                            model.walk('l');
                            break;
                        case 4:
                            rmp::objects::detail::draw_behaviors(*object);
                            model.walk('d');
                            break;
                        default:
                            rmp::objects::detail::update_behaviors(*object, FRAME_STEP);
                            model.walk('u');
                            break;
                    }
                    RC_ASSERT(journal.said == model.said);
                    rmp::objects::detail::collect();
                    if (const rmp::Ref<rmp::Object> still = handles.at(which).get()) {
                        RC_ASSERT(rmp::objects::detail::behavior_count(*still) ==
                                  static_cast<int>(model.list.size()));
                        for (const char letter : LETTERS) {
                            RC_ASSERT(has_by_letter(*still, letter) == model.has(letter));
                        }
                    } else {
                        RC_ASSERT(!model.alive);
                    }
                }
                // Every behavior that was readied is ended, once, on the way out.
                journal.said.clear();
                std::string ends;
                for (std::size_t k = 0; k < handles.size(); ++k) {
                    if (const rmp::Ref<rmp::Object> object = handles.at(k).get()) {
                        Expected &model = models.at(k);
                        model.said.clear();
                        model.end_all();
                        ends += model.said;
                        object->destroy();
                    }
                }
                RC_ASSERT(journal.said == ends);
            });
    }
}
// NOLINTEND(readability-make-member-function-const,readability-convert-member-functions-to-static,readability-named-parameter)
