// ---------------------------------------------------------------------------
// The catalogue's implementations. The surface, and the argument for each one
// being in the catalogue at all, is include/rmp/behavior.h.
//
// Nearly all of this is ordinary code against the public API: the behaviors
// read the same rmp::input a game reads, write the same position and velocity
// a game writes, and call the same scene()->raycast(). That is the claim the
// model makes -- one of yours and one of ours are the same thing -- and this
// file is the evidence for it, with two exceptions that mark where the public
// API stops:
//
//   Spawner asks for the scene's live objects (objects::detail::live_objects),
//   to tell which ones its on_spawn made; a game is handed the object it
//   spawns and has no need to list them.
//
//   TopDown asks a sheet whether it has a tag (animation::detail::tag_index)
//   before playing it, because Sprite::play() warns about a tag that is not
//   there and trying "walk_ne" on a sheet without one is not a mistake.
//
// The rest -- Parallax's tiling, the warn-once -- is this file's own.
// ---------------------------------------------------------------------------

#include <rmp/behavior.h>

#include <rmp/assets.h>
#include <rmp/input.h>
#include <rmp/random.h>

#include "animation_internal.h"
#include "behavior_internal.h"
#include "internal.h"
#include "object_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

// Every function below is one half of an INTERFACE: the engine finds it with
// `requires` and calls it through a pointer, so its signature is fixed by the
// contract and not by what this particular implementation happens to need.
// clang-tidy is right on its own terms about several of them -- a hook that
// only writes to `self` could be static, one that reads no state could be
// const -- and acting on that would break the contract for the ones that do,
// and flip back the first time a field is added. Turned off for the file, with
// the reason, rather than ten separate NOLINTs saying the same thing.
//
// NOLINTBEGIN(readability-convert-member-functions-to-static,readability-make-member-function-const,readability-named-parameter)
namespace rmp::behavior {

namespace {

// What a behavior writes -- into its object, and into what it keeps -- is a
// finite number, or nothing. A value that is not one (a field that is NaN or
// an infinity, a speed so large the arithmetic overflowed, an object already
// at NaN) leaves the old value where it was and is said once per behavior:
// written, it would be every position the object touches from then on.
void not_written(std::string_view who, float value) {
    RMP_REPORT_ONCE_KEYED(
        who,
        "BEHAVIOR: %.*s did not write %g: a behavior writes only finite "
        "numbers, and left the old value. Look at its fields, and at the "
        "object's position.",
        static_cast<int>(who.size()), who.data(), static_cast<double>(value));
}

} // namespace

namespace detail {

float length_of(Vector2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

bool finite(Vector2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

// The direction of `v`, or none: {0, 0} for a vector too short to have one,
// and for one that is not a number. A vector whose parts are finite and whose
// length is not -- each part near float's end -- has its direction worked out
// in double, where the length fits.
Vector2 normalised(Vector2 v) {
    const float len = length_of(v);
    if (std::isfinite(len)) {
        if (len < NEAR_ZERO) return Vector2{ 0, 0 };
        return Vector2{ v.x / len, v.y / len };
    }
    if (!finite(v)) return Vector2{ 0, 0 };
    const double big = std::hypot(static_cast<double>(v.x), static_cast<double>(v.y));
    return Vector2{ static_cast<float>(v.x / big), static_cast<float>(v.y / big) };
}

// The seconds a hook was handed, as a behavior counts them: a finite number,
// 0 or more. The framework's clock never hands anything else (clamp_delta, in
// src/rmp/clock.cpp); a game that calls a hook itself might, and a NaN there
// was a NaN in every position the behavior wrote, an infinity a Timer that
// never returned. Anything else is a frame in which no time passed, said once.
float seconds_of(float delta) {
    if (std::isfinite(delta) && delta >= 0) return delta;
    RMP_REPORT_ONCE(
        "BEHAVIOR: a behavior's hook was handed delta = %g. A frame is a finite "
        "number of seconds, 0 or more, and this one counts as 0.",
        static_cast<double>(delta));
    return 0.0f;
}

void put(float &slot, float value, std::string_view who) {
    if (std::isfinite(value)) {
        slot = value;
        return;
    }
    not_written(who, value);
}

void put(Vector2 &slot, Vector2 value, std::string_view who) {
    if (finite(value)) {
        slot = value;
        return;
    }
    not_written(who, std::isfinite(value.x) ? value.y : value.x);
}

// A field that is NaN is said once, by name. Every comparison with one is
// false, so it quietly turns a behavior off -- a Timer that never fires, a
// Lifespan that never ends -- or would reach the object through what the
// behavior writes, which put() refuses.
void say_nan(std::string_view who, std::initializer_list<Field> fields) {
    for (const Field &f : fields) {
        if (!std::isnan(f.value)) continue;
        const std::string key = std::string(who) + "::" + std::string(f.name);
        RMP_REPORT_ONCE_KEYED(std::string_view(key),
                              "BEHAVIOR: %s is NaN. Every comparison with it is false, "
                              "and nothing the behavior works out from it is written.",
                              key.c_str());
    }
}

// Move `current` towards `target` at `rate` per second, or jump straight there
// when the rate is 0 -- which is what "acceleration = 0 means instant" is.
float approach(float current, float target, float rate, float delta) {
    if (rate <= 0) return target;
    const float step = rate * delta;
    if (current < target) return current + step > target ? target : current + step;
    if (current > target) return current - step < target ? target : current - step;
    return current;
}

} // namespace detail

namespace {

using detail::approach;
using detail::finite;
using detail::length_of;
using detail::NEAR_ZERO;
using detail::normalised;
using detail::put;
using detail::say_nan;
using detail::seconds_of;

// The action name a behavior was given, or the standard one when it was left
// empty. Empty means "the defaults", and the defaults are what rmp::input ships.
std::string_view or_default(std::string_view given, std::string_view fallback) {
    return given.empty() ? fallback : given;
}

bool held(std::string_view action) { return rmp::input::pressed(action); }

// Whether `used` jumps leave one: the ground's and `air_jumps` more. Not
// `used < air_jumps + 1`, which was undefined for an air_jumps of INT_MAX.
bool has_air_jump(int used, int air_jumps) { return used <= air_jumps; }

// How long ago the ground was, or the jump press, when it never was: what
// Platformer's `ours` starts at and what a jump sets them to. A coyote_time or
// a jump_buffer that long or longer -- a field nobody means -- does not make
// "never" recent: at 2000 the buffer fired a jump nobody pressed, every frame,
// and the coyote window never closed (the behaviours' property found both).
constexpr float LONG_AGO = 1000;

bool within(float since, float window) { return since < LONG_AGO && since <= window; }

// One more jump, and never past INT_MAX: a count with no ceiling can be asked
// to pass it.
void count_jump(int &used) {
    if (used < std::numeric_limits<int>::max()) ++used;
}
bool pressed(std::string_view action) { return rmp::input::just_pressed(action); }

// Is there something SOLID directly below? A raycast rather than a flag on the
// object, because the collision pass does not keep a contact list -- and a ray
// is what a game would write anyway, only correct the first time.
//
// The nearest SOLID hit, not the nearest hit. Asking for the nearest and then
// whether THAT one was solid meant any non-solid collider between the feet and
// the floor hid the floor completely: the player could not jump and velocity.y
// kept accumulating while it stood still. Coins, pickups, damage triggers and
// decoration on the floor are the ordinary contents of a platformer level, so
// the ray skips them inside cast() (RayQuery::solid_only). Both Platformer and
// Runner come through here.
bool standing_on_something(const Object &self, float reach) {
    const Ref<Scene> scene = Storage::spawned_in(self);
    if (!scene) return false;
    const Rectangle box = self.world_collider();
    if (box.height <= 0) return false;
    const Vector2 feet{ box.x + box.width / 2, box.y + box.height };
    // The map's solid cells first, across the whole width of the feet: the
    // map holds up a box any part of which is over a solid cell (see
    // move_through_map in object.cpp), so the ground has to be the same, or a
    // player half over a ledge is held up and cannot jump.
    if (scene->map.valid() && self.solid && !self.immovable &&
        scene->map.solid_in(Rectangle{ box.x, feet.y, box.width, reach })) {
        return true;
    }
    const RayQuery query{ .from = Vector2{ feet.x, feet.y - 1 },
                          .to = Vector2{ feet.x, feet.y + reach },
                          .mask = self.collision_mask,
                          .ignore = self.handle(),
                          .solid_only = true };
    return static_cast<bool>(scene->raycast(query));
}

} // namespace

// ---------------------------------------------------------------------------
// TopDown
// ---------------------------------------------------------------------------

int TopDown::sector() const {
    // atan2 gives -pi..pi with 0 pointing right and y growing DOWN, so a
    // positive angle is downward on screen. The suffix table starts at "e" and
    // goes anticlockwise in screen terms, which is why the angle is negated.
    float angle = std::atan2(-ours.facing.y, ours.facing.x);
    // A facing that is no direction -- `ours` is public, and a NaN written
    // there made the cast below undefined -- is east, where the table starts.
    if (!std::isfinite(angle)) return 0;
    if (angle < 0) angle += 2 * std::numbers::pi_v<float>;
    const float eighth = 2 * std::numbers::pi_v<float> / 8;
    auto index = static_cast<int>((angle + eighth / 2) / eighth);
    return index % 8;
}

void TopDown::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("TopDown", { { "speed", speed }, { "acceleration", acceleration } });
    Vector2 wanted =
        rmp::input::vector(or_default(left, "move_left"), or_default(right, "move_right"),
                           or_default(up, "move_up"), or_default(down, "move_down"));
    // A reading that is not a number is no input: a stick that reported an
    // infinity came back from vector() as a NaN, and that was the facing, the
    // velocity and the sprite's sector.
    if (!finite(wanted)) wanted = Vector2{ 0, 0 };

    if (!eight_way && wanted.x != 0 && wanted.y != 0) {
        // Four directions: whichever axis is being pushed harder wins, and a
        // dead heat keeps the horizontal, because that is what a keyboard
        // pressing both at once means to a player.
        if (std::fabs(wanted.x) >= std::fabs(wanted.y)) {
            wanted = Vector2{ wanted.x < 0 ? -1.0f : 1.0f, 0 };
        } else {
            wanted = Vector2{ 0, wanted.y < 0 ? -1.0f : 1.0f };
        }
    }

    if (wanted.x != 0 || wanted.y != 0) {
        const Vector2 facing = normalised(wanted);
        if (facing.x != 0 || facing.y != 0) ours.facing = facing;
    }

    const Vector2 target{ wanted.x * speed, wanted.y * speed };
    put(self.velocity,
        Vector2{ approach(self.velocity.x, target.x, acceleration, seconds),
                 approach(self.velocity.y, target.y, acceleration, seconds) },
        "TopDown");

    // The flip first, because it is the half that works with no sheet at all:
    // a one-direction sheet mirrored for the left. Which assumes the art faces
    // RIGHT, east, where the suffix table starts; a tag with a suffix is drawn
    // for its own direction and is played unflipped, below.
    if (ours.facing.x < -NEAR_ZERO) self.flip_x = true;
    if (ours.facing.x > NEAR_ZERO) self.flip_x = false;

    // And then the tag. The rule is: try "<walk>_<suffix>", and if the sheet
    // has no such tag, fall back to "<walk>" and let flip_x cover the left.
    // With nothing configured, a two-, four- or eight-direction sheet all work
    // -- and WE INVENT NO NAMES: the ones tried are the ones you put in your
    // .aseprite, with suffixes you can change.
    if (!self.sprite.sheet.valid()) return;
    const bool moving = wanted.x != 0 || wanted.y != 0;
    const std::string &base = moving ? walk : idle;
    if (base.empty()) return;

    if (moving) {
        const std::string &suffix = suffixes[static_cast<std::size_t>(sector())];
        const std::string tag = suffix.empty() ? base : base + "_" + suffix;
        if (rmp::animation::detail::tag_index(self.sprite.sheet.raw(), tag) >= 0) {
            // "walk_w" is drawn facing west; mirroring it would face it east.
            self.flip_x = false;
            self.sprite.play(tag);
            return;
        }
    }
    self.sprite.play(base);
}

// ---------------------------------------------------------------------------
// Platformer
// ---------------------------------------------------------------------------

void Platformer::_ready(Object &self) {
    // Solid, so the map and the platforms stop it. And gravity_scale 0,
    // because this behavior integrates its OWN gravity -- the `gravity` field
    // above, applied only while off the ground -- so that `jump` reaches the
    // same height whatever the scene's gravity is. Heavier is
    // `get<Platformer>()->gravity = 3000`, after an `if`; setting gravity_scale afterwards
    // adds the scene's gravity on top of this one, twice the fall.
    self.solid = true;
    self.gravity_scale = 0;
}

void Platformer::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("Platformer",
            { { "speed", speed },
              { "acceleration", acceleration },
              { "gravity", gravity },
              { "jump", jump },
              { "coyote_time", coyote_time },
              { "jump_buffer", jump_buffer } });
    const float dir = (held(or_default(right, "move_right")) ? 1.0f : 0.0f) -
        (held(or_default(left, "move_left")) ? 1.0f : 0.0f);
    put(self.velocity.x, approach(self.velocity.x, dir * speed, acceleration, seconds),
        "Platformer");

    const bool was_grounded = ours.grounded;
    ours.grounded = standing_on_something(self, 2.0f) && self.velocity.y >= -NEAR_ZERO;
    if (ours.grounded) {
        ours.since_grounded = 0;
        if (!was_grounded) ours.jumps_used = 0;
    } else {
        ours.since_grounded += seconds;
        // LEAVING THE GROUND SPENDS THE GROUND JUMP, once the coyote window has
        // closed. Without this line `jumps_used < air_jumps + 1` hands a free
        // jump to anything that has not jumped yet -- a player long past the
        // ledge, or one that has never touched the ground at all -- and that
        // makes both of the features below meaningless: coyote_time cannot be
        // felt, because there was always a jump available anyway, and the
        // buffered jump is never observed, because the press fires the moment
        // it happens instead of waiting for the landing.
        if (!within(ours.since_grounded, coyote_time) && ours.jumps_used == 0) {
            ours.jumps_used = 1;
        }
    }

    if (pressed(or_default(jump_action, "ui_accept")))
        ours.since_jump_pressed = 0;
    else
        ours.since_jump_pressed += seconds;

    // Coyote time and the buffered jump, which are the two things nobody can
    // name and everybody feels: a jump pressed just after the ledge still
    // fires, and one pressed just before landing is remembered.
    const bool may_jump = within(ours.since_grounded, coyote_time) ||
        has_air_jump(ours.jumps_used, air_jumps);
    if (within(ours.since_jump_pressed, jump_buffer) && may_jump) {
        put(self.velocity.y, -jump, "Platformer");
        ours.since_jump_pressed = LONG_AGO;
        ours.since_grounded = LONG_AGO;
        count_jump(ours.jumps_used);
        ours.grounded = false;
    }

    if (!ours.grounded)
        put(self.velocity.y, self.velocity.y + gravity * seconds, "Platformer");
    else if (self.velocity.y > 0)
        self.velocity.y = 0;
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

void Runner::_ready(Object &self) {
    // As Platformer: solid, and its own gravity rather than the scene's.
    self.solid = true;
    self.gravity_scale = 0;
}

void Runner::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("Runner",
            { { "speed", speed },
              { "accelerate", accelerate },
              { "max_speed", max_speed },
              { "gravity", gravity },
              { "jump", jump } });
    // Its own field, and so written like the object's: only ever a number.
    float faster = speed + accelerate * seconds;
    if (faster > max_speed) faster = max_speed;
    put(speed, faster, "Runner");
    const float ran = speed * seconds;
    if (std::isfinite(ran)) ours.distance += ran;

    ours.ducking = !duck_action.empty() && held(duck_action);

    const bool was_grounded = ours.grounded;
    ours.grounded = standing_on_something(self, 2.0f) && self.velocity.y >= -NEAR_ZERO;
    if (ours.grounded) {
        if (!was_grounded) ours.jumps_used = 0;
    } else if (ours.jumps_used == 0) {
        // Leaving the ground spends the ground jump, exactly as in Platformer:
        // otherwise `jumps_used < air_jumps + 1` hands a free mid-air jump to a
        // runner that has never stood on anything.
        ours.jumps_used = 1;
    }

    if (pressed(or_default(jump_action, "ui_accept")) &&
        (ours.grounded || has_air_jump(ours.jumps_used, air_jumps))) {
        put(self.velocity.y, -jump, "Runner");
        count_jump(ours.jumps_used);
        ours.grounded = false;
    }

    if (!ours.grounded)
        put(self.velocity.y, self.velocity.y + gravity * seconds, "Runner");
    else if (self.velocity.y > 0)
        self.velocity.y = 0;

    // The x is the runner's, always: this is the behavior that runs forward.
    put(self.velocity.x, speed, "Runner");
}

// ---------------------------------------------------------------------------
// Ball
// ---------------------------------------------------------------------------

void Ball::_ready(Object &self) {
    self.edges = Edge::BOUNCE;
    // Not solid on purpose: a ball that pushed the paddle out of the way would
    // be a different game. It is told about the contact and answers it itself.
    self.solid = false;
}

// cppcheck-suppress constParameterReference ; a hook's signature is the API's
void Ball::_collision(Object &self, Object &other) {
    const Rectangle mine = self.world_collider();
    const Rectangle theirs = other.world_collider();
    const float my_x = mine.x + mine.width / 2;
    const float my_y = mine.y + mine.height / 2;
    const float their_x = theirs.x + theirs.width / 2;
    const float their_y = theirs.y + theirs.height / 2;

    // Which face was hit: whichever overlap is shallower. The same "axis of
    // least penetration" the solid separation uses, for the same reason.
    const float overlap_x = (mine.width + theirs.width) / 2 - std::fabs(my_x - their_x);
    const float overlap_y = (mine.height + theirs.height) / 2 - std::fabs(my_y - their_y);

    say_nan("Ball",
            { { "speed", speed },
              { "max_bounce_deg", max_bounce_deg },
              { "speed_up", speed_up } });
    // Worked out on a copy, and written once: what comes out of a collider
    // that is not a number is not written at all.
    Vector2 velocity = self.velocity;
    if (overlap_x < overlap_y) {
        velocity.x = my_x < their_x ? -std::fabs(velocity.x) : std::fabs(velocity.x);
        // Where along the face it landed, -1..1, tilts the bounce. This is half
        // of why Pong feels like Pong: hitting with the end of the paddle sends
        // the ball off at an angle, and the player learns it without being told.
        if (theirs.height > 0 && max_bounce_deg > 0) {
            const float along = (my_y - their_y) / (theirs.height / 2);
            const float clamped = along < -1 ? -1 : (along > 1 ? 1 : along);
            const float angle =
                clamped * max_bounce_deg * std::numbers::pi_v<float> / 180;
            const float sign = velocity.x < 0 ? -1.0f : 1.0f;
            velocity = Vector2{ sign * std::cos(angle) * speed, std::sin(angle) * speed };
        }
    } else {
        velocity.y = my_y < their_y ? -std::fabs(velocity.y) : std::fabs(velocity.y);
        if (theirs.width > 0 && max_bounce_deg > 0) {
            const float along = (my_x - their_x) / (theirs.width / 2);
            const float clamped = along < -1 ? -1 : (along > 1 ? 1 : along);
            const float angle =
                clamped * max_bounce_deg * std::numbers::pi_v<float> / 180;
            const float sign = velocity.y < 0 ? -1.0f : 1.0f;
            velocity = Vector2{ std::sin(angle) * speed, sign * std::cos(angle) * speed };
        }
    }
    put(self.velocity, velocity, "Ball");

    // Its own field: a pace that overflowed would be a velocity of NaN on the
    // next update, so it stops climbing at float's end instead.
    if (other.solid && speed_up != 1.0f) put(speed, speed * speed_up, "Ball");
}

void Ball::_update(Object &self, float delta) {
    (void)seconds_of(delta);
    say_nan("Ball", { { "speed", speed } });
    // ONLY when it is not zero. That one condition is what makes the serve a
    // single apply_force -- the direction is all that survives the normalise --
    // and what makes `ball.velocity = {}` a stable "stopped" rather than
    // something the next frame undoes.
    const Vector2 dir = normalised(self.velocity);
    if (dir.x == 0 && dir.y == 0) return;
    put(self.velocity, Vector2{ dir.x * speed, dir.y * speed }, "Ball");
}

// ---------------------------------------------------------------------------
// Projectile
// ---------------------------------------------------------------------------

void Projectile::_ready(Object &self) { self.edges = Edge::DESTROY; }

// cppcheck-suppress constParameterReference ; a hook's signature is the API's
void Projectile::_collision(Object &self, Object &other) {
    (void)other;
    if (destroy_on_hit) self.destroy();
}

void Projectile::_update(Object &self, float delta) {
    (void)seconds_of(delta);
    say_nan("Projectile", { { "speed", speed } });
    if (destroy_on_wall) {
        const Ref<Scene> scene = Storage::spawned_in(self);
        if (scene && scene->map.valid() && scene->map.solid_at(self.position)) {
            self.destroy();
            return;
        }
    }
    if (!(speed > 0)) return; // keep whatever it was given
    const Vector2 dir = normalised(self.velocity);
    if (dir.x == 0 && dir.y == 0) return;
    put(self.velocity, Vector2{ dir.x * speed, dir.y * speed }, "Projectile");
}

// ---------------------------------------------------------------------------
// Follow
// ---------------------------------------------------------------------------

void Follow::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("Follow",
            { { "speed", speed },
              { "stop_distance", stop_distance },
              { "acceleration", acceleration } });
    const Ref<Object> goal = target.get();
    if (!goal) {
        // The target died. Coasting is the right answer rather than stopping
        // dead: a homing missile whose target explodes keeps flying.
        return;
    }
    const Vector2 to{ goal->position.x - self.position.x,
                      goal->position.y - self.position.y };
    const float distance = length_of(to);
    if (distance <= stop_distance) {
        put(self.velocity,
            Vector2{ approach(self.velocity.x, 0, acceleration, seconds),
                     approach(self.velocity.y, 0, acceleration, seconds) },
            "Follow");
        return;
    }
    const Vector2 dir = normalised(to);
    put(self.velocity,
        Vector2{ approach(self.velocity.x, dir.x * speed, acceleration, seconds),
                 approach(self.velocity.y, dir.y * speed, acceleration, seconds) },
        "Follow");
}

// ---------------------------------------------------------------------------
// Tween
// ---------------------------------------------------------------------------

namespace {

float eased(Ease kind, float t) {
    switch (kind) {
        case Ease::IN:
            return t * t;
        case Ease::OUT:
            return 1 - (1 - t) * (1 - t);
        case Ease::IN_OUT:
            return t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t);
        case Ease::LINEAR:
        default:
            return t;
    }
}

void write_property(Object &self, Property property, float value) {
    if (!std::isfinite(value)) {
        // From a field that is not a number -- and for ALPHA, a cast to an
        // unsigned char with no defined result.
        not_written("Tween", value);
        return;
    }
    switch (property) {
        case Property::POSITION_X:
            self.position.x = value;
            break;
        case Property::POSITION_Y:
            self.position.y = value;
            break;
        case Property::SCALE_X:
            self.scale.x = value;
            break;
        case Property::SCALE_Y:
            self.scale.y = value;
            break;
        case Property::ROTATION:
            self.rotation = value;
            break;
        case Property::ALPHA: {
            const float clamped = value < 0 ? 0 : (value > 255 ? 255 : value);
            const auto a = static_cast<unsigned char>(clamped);
            self.shape.color.a = a;
            self.sprite.tint.a = a;
            break;
        }
    }
}

} // namespace

void Tween::_update(Object &self, float delta) {
    const float step = seconds_of(delta);
    say_nan("Tween", { { "from", from }, { "to", to }, { "seconds", seconds } });
    // A NaN `seconds` goes the way 0 does: there is no pass to divide by it.
    const bool instant = !(seconds > 0);
    if (ours.done || instant) {
        if (instant && !ours.done) {
            // A zero-length tween is the destination, immediately. Dividing by
            // it would be a NaN that spreads to the position and then to
            // everything the position touches.
            write_property(self, property, to);
            ours.done = true;
        }
        return;
    }

    ours.elapsed += step;
    float t = ours.elapsed / seconds;
    if (t >= 1) {
        if (ping_pong || loop) {
            // Every pass the frame covered, and not one: a frame longer than
            // `seconds` -- a fast tween on a slow frame -- was taken one pass
            // back and put the object past `to`, by the passes it missed. The
            // remainder exactly, and a ping_pong turned once per pass.
            const float passes = std::floor(t);
            ours.elapsed =
                std::isfinite(ours.elapsed) ? std::fmod(ours.elapsed, seconds) : 0.0f;
            if (ping_pong && std::fmod(passes, 2.0f) == 1.0f) ours.back = !ours.back;
            t = ours.elapsed / seconds;
        } else {
            // EXACTLY the destination, not an epsilon short of it. A platform
            // that stops one hundredth of a unit from where the level says it
            // should be is a gap the player can fall through.
            write_property(self, property, to);
            ours.done = true;
            return;
        }
    }

    const float shaped = eased(ease, ours.back ? 1 - t : t);
    float value = from + (to - from) * shaped;
    // `to - from` past float's range, the two ends finite: std::lerp, which
    // never forms the difference.
    if (!std::isfinite(value)) value = std::lerp(from, to, shaped);
    write_property(self, property, value);
}

// ---------------------------------------------------------------------------
// GridSnap
// ---------------------------------------------------------------------------

void GridSnap::_late_update(Object &self, float delta) {
    (void)seconds_of(delta);
    say_nan("GridSnap",
            { { "cell.x", cell.x },
              { "cell.y", cell.y },
              { "offset.x", offset.x },
              { "offset.y", offset.y } });
    // Round, not floor: a piece has to land on the CENTRE of its cell, and
    // flooring would drift it half a cell towards the origin every time. And
    // through put(): a cell so small the position is no number of them -- the
    // count past float's range -- came back as an infinity.
    if (cell.x > 0) {
        put(self.position.x,
            offset.x + std::round((self.position.x - offset.x) / cell.x) * cell.x,
            "GridSnap");
    }
    if (cell.y > 0) {
        put(self.position.y,
            offset.y + std::round((self.position.y - offset.y) / cell.y) * cell.y,
            "GridSnap");
    }
}

// ---------------------------------------------------------------------------
// Parallax
// ---------------------------------------------------------------------------

// cppcheck-suppress constParameterReference ; a hook's signature is the API's
void Parallax::_ready(Object &self) {
    (void)self;
    if (!texture.empty()) ours.art = rmp::assets::load_texture(texture);
}

// cppcheck-suppress constParameterReference ; a hook's signature is the API's
void Parallax::_update(Object &self, float delta) {
    (void)self;
    say_nan("Parallax", { { "factor", factor }, { "speed", speed }, { "y", y } });
    // Only its own drift. Where the CAMERA is gets read at draw time and
    // multiplied by the factor there, so a camera that follows the player
    // scrolls every layer at its own rate with no bookkeeping in between.
    put(ours.scroll, ours.scroll + speed * seconds_of(delta), "Parallax");
}

// cppcheck-suppress constParameterReference ; a hook's signature is the API's
void Parallax::_draw(Object &self) {
    if (!ours.art.valid()) return;
    const Texture2D &tex = ours.art;
    if (tex.width <= 0) return;

    // The layer is drawn INSIDE the camera like everything else, so it is
    // placed relative to the view: a layer with factor 0 is pinned to the
    // screen because it moves exactly as much as the view does, and factor 1
    // stands still in the world. The object's own x shifts it on top of that.
    const Ref<Scene> scene = Storage::spawned_in(self);
    const Rectangle view = scene ? scene->camera.view()
                                 : Rectangle{ 0, 0, static_cast<float>(RMP_WINDOW_WIDTH),
                                              static_cast<float>(RMP_WINDOW_HEIGHT) };
    // The arithmetic is next door, in detail::parallax_shift and
    // detail::parallax_tiling, and these calls are the only thing between it
    // and the GPU. That is not tidiness: everything below this line needs a
    // render batch InitWindow() creates, and everything above it is the part
    // that gets written wrong -- so the part that gets written wrong is the
    // part a headless test can reach.
    const float shift =
        detail::parallax_shift(self.position.x, view.x, factor, ours.scroll);
    const detail::ParallaxTiling tiling =
        detail::parallax_tiling(shift, static_cast<float>(tex.width), view.width);

    for (int i = 0; i < tiling.copies; ++i) {
        const float at = view.x + tiling.offset +
            static_cast<float>(i) * static_cast<float>(tex.width);
        DrawTextureV(tex, Vector2{ at, view.y + y }, tint);
    }
}

namespace detail {

float parallax_shift(float x, float view_x, float factor, float scroll) {
    return (x - view_x) * factor + scroll;
}

ParallaxTiling parallax_tiling(float shift, float width, float screen) {
    if (!(width > 0) || !std::isfinite(width)) return ParallaxTiling{};
    // Modulo the texture's width, and drawn MORE THAN ONCE. This is the whole
    // behavior: one copy leaves a gap the moment the offset passes the width,
    // and getting the next copy's position wrong by a pixel is a seam that
    // crawls across the screen. std::fmod returns a NEGATIVE for a negative
    // argument and a positive for a positive one, and a positive offset is a
    // gap down the left-hand side -- which is why it is nudged back into range
    // rather than trusted.
    // A shift that is not a number is none: fmod() of it is a NaN, and the
    // cast below of a NaN had no defined result.
    float offset = std::isfinite(shift) ? std::fmod(shift, width) : 0.0f;
    if (offset > 0) offset -= width;
    // A shift a hair past a whole number of widths rounds to exactly -width
    // there, which is the same picture as 0.
    if (offset <= -width) offset = 0;

    // An integer count, with each copy's position derived from it. Stepping a
    // float by `width` accumulates error across the copies, and error between
    // two copies of a scrolling background IS the seam this behavior exists to
    // remove: a gap of a third of a pixel that crawls sideways. One for a
    // screen of no width or of none at all, and at most MAX_PARALLAX_COPIES: a
    // view thousands of textures wide -- a camera zoomed far out -- was a cast
    // past int's range, and then a loop of that many draws.
    const float fit = (screen - offset) / width;
    if (!(fit >= 0)) return ParallaxTiling{ offset, 1 };
    if (fit >= static_cast<float>(MAX_PARALLAX_COPIES - 1)) {
        return ParallaxTiling{ offset, MAX_PARALLAX_COPIES };
    }
    return ParallaxTiling{ offset, static_cast<int>(fit) + 1 };
}

} // namespace detail

// ---------------------------------------------------------------------------
// Spawner
// ---------------------------------------------------------------------------

namespace {

// The cap this spawner can actually hold to. A live cap needs a handle per live
// object, and there are MAX_SPAWNED of them -- so a larger max_alive is a
// number the behavior cannot honour, and saying so once is better than quietly
// enforcing a different one.
int spawner_cap(int max_alive) {
    if (max_alive <= MAX_SPAWNED) return max_alive;
    RMP_REPORT_ONCE("BEHAVIOR: Spawner::max_alive = %d, and a spawner tracks %d live "
                    "objects. Capping at %d -- for more than that, pool the objects "
                    "instead of making new ones, or use a second spawner.",
                    max_alive, MAX_SPAWNED, MAX_SPAWNED);
    return MAX_SPAWNED;
}

} // namespace

// cppcheck-suppress constParameterReference ; a hook's signature is the API's
void Spawner::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("Spawner",
            { { "every_seconds", every_seconds },
              { "every_distance", every_distance },
              { "jitter", jitter } });
    const Ref<Scene> scene = Storage::spawned_in(self);
    if (!scene || !on_spawn) return;

    // What the distance is measured on: the tracked object while it lives,
    // and the spawner itself otherwise.
    const Ref<Object> tracked = track.get();
    const Object &ruler = tracked ? *tracked : self;

    if (!ours.started) {
        ours.started = true;
        ours.last_at = ruler.position;
        // A NaN is not kept: neither mode runs on one, and a field put right
        // later starts from 0, due at once.
        ours.countdown = std::isnan(every_seconds) ? 0.0f : every_seconds;
        ours.next_at = std::isnan(every_distance) ? 0.0f : every_distance;
        ours.travelled = 0;
    }

    const float moved = std::sqrt(
        (ruler.position.x - ours.last_at.x) * (ruler.position.x - ours.last_at.x) +
        (ruler.position.y - ours.last_at.y) * (ruler.position.y - ours.last_at.y));
    // A ruler at NaN moved by no distance anyone can add up.
    if (!std::isnan(moved)) ours.travelled += moved;
    ours.last_at = ruler.position;

    bool due = false;
    if (every_distance > 0) {
        // BY DISTANCE, and this is the whole reason this is not a Timer: in an
        // endless runner the speed climbs, so "every two seconds" spaces the
        // obstacles further and further apart and the game gets easier the
        // faster you go.
        if (ours.travelled >= ours.next_at) {
            due = true;
            // From the point that was SCHEDULED, not from where the object
            // happens to be now. Restarting from `ours.travelled` adds one frame of
            // overshoot to every interval, and since the speed is climbing the
            // overshoot climbs with it -- the gaps were 223, 227, 231 for an
            // every_distance of 200, drifting exactly the way the time-based
            // mode this is meant to beat does.
            ours.next_at += every_distance + jitter_amount();
        }
    } else if (every_seconds > 0) {
        ours.countdown -= seconds;
        if (ours.countdown <= 0) {
            due = true;
            // += rather than =, for the same reason as above: a frame that
            // overshoots owes the difference to the next interval.
            ours.countdown += every_seconds + jitter_amount();
        }
    }

    if (!due) return;

    // Forget the dead first, because the cap is on what is ALIVE and a handle
    // whose object is gone is exactly what says so. A plain counter could only
    // ever go up: nothing tells a spawner that what it made has died.
    int kept = 0;
    for (int i = 0; i < ours.made_count; ++i) {
        if (ours.made[i]) ours.made[kept++] = ours.made[i];
    }
    ours.made_count = kept;

    if (max_alive > 0 && ours.made_count >= spawner_cap(max_alive)) return;

    // Which objects the callback actually made. It is not always one -- a wave
    // spawner makes five, and the cap has to count them all or it is not a cap
    // -- and it is not always the last ones in the scene either, because the
    // callback may destroy as well as spawn. The live set before and after is
    // what says which they are.
    std::vector<Object *> before = rmp::objects::detail::live_objects(*scene);
    std::ranges::sort(before);
    on_spawn(*scene, ruler.position);

    for (const Object *made : rmp::objects::detail::live_objects(*scene)) {
        if (std::ranges::binary_search(before, made)) continue;
        if (ours.made_count >= MAX_SPAWNED) break;
        ours.made[ours.made_count++] = made->handle();
    }
}

int Spawner::alive() const {
    int count = 0;
    for (int i = 0; i < ours.made_count; ++i) {
        if (ours.made[i]) ++count;
    }
    return count;
}

float Spawner::jitter_amount() const {
    // None for a jitter that is not a finite number: rmp::random::range() of
    // an infinity gave one back, and every interval after it with it. And at
    // most half of float's range either way, so the range's width is a number.
    if (!(jitter > 0) || !std::isfinite(jitter)) return 0;
    const float most = std::min(jitter, std::numeric_limits<float>::max() / 2);
    return rmp::random::range(-most, most);
}

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------

void Timer::_update(Object &self, float delta) {
    const float step = seconds_of(delta);
    say_nan("Timer", { { "seconds", seconds } });
    if (ours.spent || !(seconds > 0)) return;
    ours.elapsed += step;
    if (!(ours.elapsed >= seconds)) return;
    // Every interval the frame covered, and not one: a frame longer than the
    // interval owes more than one tick, and dropping them makes a countdown
    // run slow on a slow machine. COUNTED, with the remainder kept exactly:
    // subtracting `seconds` once a tick never ended when `seconds` was below
    // the rounding of `elapsed` (in a float, 1e8 - 1 is 1e8), nor for an
    // infinite frame. And at most detail::MAX_TIMEOUTS calls: more than that
    // owed in one frame is an interval far shorter than any frame, and the
    // rest of the time is dropped, said once.
    const double owed = std::isfinite(ours.elapsed)
        ? std::floor(static_cast<double>(ours.elapsed) / seconds)
        : std::numeric_limits<double>::infinity();
    ours.elapsed = std::isfinite(ours.elapsed) ? std::fmod(ours.elapsed, seconds) : 0.0f;
    int calls = detail::MAX_TIMEOUTS;
    if (owed <= detail::MAX_TIMEOUTS) {
        calls = static_cast<int>(owed);
    } else {
        RMP_REPORT_ONCE("BEHAVIOR: a Timer of %g seconds owed %g calls in one frame, and "
                        "makes %d; the rest of the time is dropped. Its interval is far "
                        "shorter than a frame.",
                        static_cast<double>(seconds), owed, detail::MAX_TIMEOUTS);
    }
    for (int i = 0; i < calls; ++i) {
        // Spent BEFORE its one call: a call that destroys the object returned
        // before the line that spent it, and the next update -- a hook called
        // by hand -- called it again.
        if (!repeat) ours.spent = true;
        if (on_timeout) on_timeout(self);
        if (!self.alive() || ours.spent) return;
    }
}

// ---------------------------------------------------------------------------
// Lifespan
// ---------------------------------------------------------------------------

void Lifespan::_update(Object &self, float delta) {
    say_nan("Lifespan", { { "seconds", seconds } });
    ours.elapsed += seconds_of(delta);
    if (ours.elapsed >= seconds) self.destroy();
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

// cppcheck-suppress constParameterReference ; a hook's signature is the API's
void Health::_ready(Object &self) {
    (void)self;
    // `{ .hp = 5 }` alone used to leave max_hp at 3, so a progress bar drawn
    // from hp / max_hp went past 1 and heal() clamped to a number nobody set.
    if (max_hp <= 0) max_hp = hp;
}

bool Health::damage(Object &self, int amount) {
    if (amount <= 0 || ours.died) return false;
    if (ours.invulnerable_left > 0) return false; // the window, and the point of it

    // In a wider type: hp is whatever the game set, and below INT_MIN the
    // subtraction was undefined.
    const std::int64_t left = static_cast<std::int64_t>(hp) - amount;
    hp = left < std::numeric_limits<int>::min() ? std::numeric_limits<int>::min()
                                                : static_cast<int>(left);
    if (on_damage) on_damage(self, amount);
    // A window that is not a number is none.
    ours.invulnerable_left = invulnerable_for > 0 ? invulnerable_for : 0;

    if (hp <= 0) {
        hp = 0;
        // Once, even if two lethal hits land in the same frame. Without the
        // flag, a player who walks into two spikes at once drops two sets of
        // loot and the death sound plays twice.
        ours.died = true;
        if (on_death) on_death(self);
        if (destroy_on_death && self.alive()) self.destroy();
    }
    return true;
}

void Health::heal(int amount) {
    if (amount <= 0 || ours.died) return;
    // In a wider type: heal(INT_MAX), "back to full", overflowed before the
    // clamp to max_hp could.
    const std::int64_t healed = static_cast<std::int64_t>(hp) + amount;
    hp = healed > max_hp ? max_hp : static_cast<int>(healed);
}

void Health::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("Health",
            { { "invulnerable_for", invulnerable_for }, { "blink_hz", blink_hz } });
    if (!(ours.invulnerable_left > 0)) {
        if (ours.hid) {
            self.visible = true;
            ours.hid = false;
        }
        return;
    }
    ours.invulnerable_left -= seconds;
    if (ours.invulnerable_left <= 0) {
        ours.invulnerable_left = 0;
        self.visible = true;
        ours.hid = false;
        return;
    }
    if (blink_hz > 0) {
        const float phase = ours.invulnerable_left * blink_hz;
        // The second half of each blink, by fmod: the cast of phase * 2 to an
        // int it used to be was undefined past int's range -- a window of a
        // hundred million seconds at 12 Hz, an invulnerability that is
        // "forever" -- and for a NaN.
        const bool hide = std::fmod(phase * 2, 2.0f) >= 1.0f;
        self.visible = !hide;
        ours.hid = hide;
    }
}

void Health::_collision(Object &self, Object &other) {
    // `hurt_by` is a mask over the OTHER object's layer, the same numbers
    // Object::collision_mask uses. 0 is "nothing hurts", which is why a Health
    // with nothing configured is a hit-point counter and not a hazard detector.
    if (hurt_by == 0) return;
    if ((other.collision_layer & hurt_by) == 0) return;
    // A Hitbox decides for itself, and calls damage() when it does: counted
    // here too, one touch would cost twice.
    if (other.has<Hitbox>()) return;
    // Through damage() and not through hp, so the invulnerable window, the
    // on_damage callback and the once-only death all apply -- which is what
    // stops standing inside a fire costing sixty hearts a second.
    damage(self, damage_on_hit);
}

void Health::_end(Object &self) {
    // Whatever the blink left behind, the object does not go out invisible.
    if (ours.hid) self.visible = true;
}

} // namespace rmp::behavior
// NOLINTEND(readability-convert-member-functions-to-static,readability-make-member-function-const,readability-named-parameter)
