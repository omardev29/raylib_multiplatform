// ---------------------------------------------------------------------------
// The catalogue's implementations. The surface, and the argument for each one
// being in the catalogue at all, is include/rmp/behavior.h.
//
// Every one of these is ordinary code against the public API: they read the
// same rmp::input a game reads, write the same position and velocity a game
// writes, and call the same scene()->raycast(). That is not an accident of how
// they were written -- it is the claim the model makes, and the fact that none
// of this file needs anything a user could not reach is the evidence for it.
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
#include <string_view>
#include <numbers>
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

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kEpsilon = 0.0001f;

float length_of(Vector2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

Vector2 normalised(Vector2 v) {
    const float len = length_of(v);
    if (len < kEpsilon) return Vector2{ 0, 0 };
    return Vector2{ v.x / len, v.y / len };
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

// The action name a behavior was given, or the standard one when it was left
// empty. Empty means "the defaults", and the defaults are what rmp::input ships.
std::string_view or_default(std::string_view given, std::string_view fallback) {
    return given.empty() ? fallback : given;
}

bool held(std::string_view action) { return rmp::input::pressed(action); }
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
bool standing_on_something(Object &self, float reach) {
    Scene *scene = self.scene();
    if (scene == nullptr) return false;
    const Rectangle box = self.world_collider();
    if (box.height <= 0) return false;
    const Vector2 feet{ box.x + box.width / 2, box.y + box.height };
    const RayQuery query{ .from = Vector2{ feet.x, feet.y - 1 },
                          .to = Vector2{ feet.x, feet.y + reach },
                          .mask = self.collision_mask,
                          .ignore = &self,
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
    if (angle < 0) angle += 2 * kPi;
    const float eighth = 2 * kPi / 8;
    auto index = static_cast<int>((angle + eighth / 2) / eighth);
    return index % 8;
}

void TopDown::_update(Object &self, float delta) {
    Vector2 wanted =
        rmp::input::vector(or_default(left, "move_left"), or_default(right, "move_right"),
                           or_default(up, "move_up"), or_default(down, "move_down"));

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

    if (wanted.x != 0 || wanted.y != 0) ours.facing = normalised(wanted);

    const Vector2 target{ wanted.x * speed, wanted.y * speed };
    self.velocity.x = approach(self.velocity.x, target.x, acceleration, delta);
    self.velocity.y = approach(self.velocity.y, target.y, acceleration, delta);

    // The flip first, because it is the half that works with no sheet at all:
    // a one-direction sheet mirrored for the left.
    if (ours.facing.x < -kEpsilon) self.flip_x = true;
    if (ours.facing.x > kEpsilon) self.flip_x = false;

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
        char tag[kMaxTagName * 2];
        const std::string &suffix = suffixes[static_cast<std::size_t>(sector())];
        int at = 0;
        for (std::size_t i = 0; i < base.size() && at + 1 < static_cast<int>(sizeof(tag));
             i++) {
            tag[at++] = base[i];
        }
        if (!suffix.empty() && at + 1 < static_cast<int>(sizeof(tag))) {
            tag[at++] = '_';
            for (std::size_t i = 0;
                 i < suffix.size() && at + 1 < static_cast<int>(sizeof(tag)); i++) {
                tag[at++] = suffix[i];
            }
        }
        tag[at] = '\0';
        if (rmp::animation::detail::tag_index(self.sprite.sheet.raw(), tag) >= 0) {
            self.sprite.play(tag);
            return;
        }
    }
    self.sprite.play(base.c_str());
}

// ---------------------------------------------------------------------------
// Platformer
// ---------------------------------------------------------------------------

void Platformer::_ready(Object &self) {
    // A behavior does not own gravity, it CONFIGURES it: these are the same two
    // fields the game would write, so `player.gravity_scale = 2` afterwards
    // works and there are not two places where gravity lives.
    self.solid = true;
    self.gravity_scale = 0; // this one integrates its own, to keep `jump` exact
}

void Platformer::_update(Object &self, float delta) {
    const float dir = (held(or_default(right, "move_right")) ? 1.0f : 0.0f) -
        (held(or_default(left, "move_left")) ? 1.0f : 0.0f);
    self.velocity.x = approach(self.velocity.x, dir * speed, acceleration, delta);

    const bool was_grounded = ours.grounded;
    ours.grounded = standing_on_something(self, 2.0f) && self.velocity.y >= -kEpsilon;
    if (ours.grounded) {
        ours.since_grounded = 0;
        if (!was_grounded) ours.jumps_used = 0;
    } else {
        ours.since_grounded += delta;
        // LEAVING THE GROUND SPENDS THE GROUND JUMP, once the coyote window has
        // closed. Without this line `jumps_used < air_jumps + 1` hands a free
        // jump to anything that has not jumped yet -- a player long past the
        // ledge, or one that has never touched the ground at all -- and that
        // makes both of the features below meaningless: coyote_time cannot be
        // felt, because there was always a jump available anyway, and the
        // buffered jump is never observed, because the press fires the moment
        // it happens instead of waiting for the landing.
        if (ours.since_grounded > coyote_time && ours.jumps_used == 0) {
            ours.jumps_used = 1;
        }
    }

    if (pressed(or_default(jump_action, "ui_accept")))
        ours.since_jump_pressed = 0;
    else
        ours.since_jump_pressed += delta;

    // Coyote time and the buffered jump, which are the two things nobody can
    // name and everybody feels: a jump pressed just after the ledge still
    // fires, and one pressed just before landing is remembered.
    const bool may_jump =
        ours.since_grounded <= coyote_time || ours.jumps_used < air_jumps + 1;
    if (ours.since_jump_pressed <= jump_buffer && may_jump) {
        self.velocity.y = -jump;
        ours.since_jump_pressed = 1000;
        ours.since_grounded = 1000;
        ours.jumps_used++;
        ours.grounded = false;
    }

    if (!ours.grounded)
        self.velocity.y += gravity * delta;
    else if (self.velocity.y > 0)
        self.velocity.y = 0;
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

void Runner::_ready(Object &self) {
    self.solid = true;
    self.gravity_scale = 0;
    ours.ground_y = self.position.y;
}

void Runner::_update(Object &self, float delta) {
    speed += accelerate * delta;
    if (speed > max_speed) speed = max_speed;
    ours.distance += speed * delta;

    ours.ducking = !duck_action.empty() && held(duck_action);

    const bool was_grounded = ours.grounded;
    ours.grounded = standing_on_something(self, 2.0f) && self.velocity.y >= -kEpsilon;
    if (ours.grounded) {
        if (!was_grounded) ours.jumps_used = 0;
    } else if (ours.jumps_used == 0) {
        // Leaving the ground spends the ground jump, exactly as in Platformer:
        // otherwise `jumps_used < air_jumps + 1` hands a free mid-air jump to a
        // runner that has never stood on anything.
        ours.jumps_used = 1;
    }

    if (pressed(or_default(jump_action, "ui_accept")) &&
        (ours.grounded || ours.jumps_used < air_jumps + 1)) {
        self.velocity.y = -jump;
        ours.jumps_used++;
        ours.grounded = false;
    }

    if (!ours.grounded)
        self.velocity.y += gravity * delta;
    else if (self.velocity.y > 0)
        self.velocity.y = 0;

    // The x is the runner's, always: this is the behavior that runs forward.
    self.velocity.x = speed;
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

    if (overlap_x < overlap_y) {
        self.velocity.x =
            my_x < their_x ? -std::fabs(self.velocity.x) : std::fabs(self.velocity.x);
        // Where along the face it landed, -1..1, tilts the bounce. This is half
        // of why Pong feels like Pong: hitting with the end of the paddle sends
        // the ball off at an angle, and the player learns it without being told.
        if (theirs.height > 0 && max_bounce_deg > 0) {
            const float along = (my_y - their_y) / (theirs.height / 2);
            const float clamped = along < -1 ? -1 : (along > 1 ? 1 : along);
            const float angle = clamped * max_bounce_deg * kPi / 180;
            const float sign = self.velocity.x < 0 ? -1.0f : 1.0f;
            self.velocity =
                Vector2{ sign * std::cos(angle) * speed, std::sin(angle) * speed };
        }
    } else {
        self.velocity.y =
            my_y < their_y ? -std::fabs(self.velocity.y) : std::fabs(self.velocity.y);
        if (theirs.width > 0 && max_bounce_deg > 0) {
            const float along = (my_x - their_x) / (theirs.width / 2);
            const float clamped = along < -1 ? -1 : (along > 1 ? 1 : along);
            const float angle = clamped * max_bounce_deg * kPi / 180;
            const float sign = self.velocity.y < 0 ? -1.0f : 1.0f;
            self.velocity =
                Vector2{ std::sin(angle) * speed, sign * std::cos(angle) * speed };
        }
    }

    if (other.solid && speed_up != 1.0f) speed *= speed_up;
}

void Ball::_update(Object &self, float delta) {
    (void)delta;
    // ONLY when it is not zero. That one condition is what makes the serve a
    // single apply_force -- the direction is all that survives the normalise --
    // and what makes `ball.velocity = {}` a stable "stopped" rather than
    // something the next frame undoes.
    const float len = length_of(self.velocity);
    if (len < kEpsilon) return;
    self.velocity =
        Vector2{ self.velocity.x / len * speed, self.velocity.y / len * speed };
}

// ---------------------------------------------------------------------------
// Projectile
// ---------------------------------------------------------------------------

void Projectile::_ready(Object &self) { self.edges = Edge::DESTROY; }

void Projectile::_collision(Object &self, Object &other) {
    (void)other;
    if (destroy_on_hit) self.destroy();
}

void Projectile::_update(Object &self, float delta) {
    (void)delta;
    if (speed <= 0) return; // keep whatever it was given
    const float len = length_of(self.velocity);
    if (len < kEpsilon) return;
    self.velocity =
        Vector2{ self.velocity.x / len * speed, self.velocity.y / len * speed };
}

// ---------------------------------------------------------------------------
// Follow
// ---------------------------------------------------------------------------

void Follow::_update(Object &self, float delta) {
    Object *goal = target.get();
    if (goal == nullptr) {
        // The target died. Coasting is the right answer rather than stopping
        // dead: a homing missile whose target explodes keeps flying.
        return;
    }
    const Vector2 to{ goal->position.x - self.position.x,
                      goal->position.y - self.position.y };
    const float distance = length_of(to);
    if (distance <= stop_distance) {
        self.velocity.x = approach(self.velocity.x, 0, acceleration, delta);
        self.velocity.y = approach(self.velocity.y, 0, acceleration, delta);
        return;
    }
    const Vector2 dir = normalised(to);
    self.velocity.x = approach(self.velocity.x, dir.x * speed, acceleration, delta);
    self.velocity.y = approach(self.velocity.y, dir.y * speed, acceleration, delta);
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
    if (ours.done || seconds <= 0) {
        if (seconds <= 0 && !ours.done) {
            // A zero-length tween is the destination, immediately. Dividing by
            // it would be a NaN that spreads to the position and then to
            // everything the position touches.
            write_property(self, property, to);
            ours.done = true;
        }
        return;
    }

    ours.elapsed += delta;
    float t = ours.elapsed / seconds;
    if (t >= 1) {
        if (ping_pong) {
            ours.back = !ours.back;
            ours.elapsed -= seconds;
            t = ours.elapsed / seconds;
        } else if (loop) {
            ours.elapsed -= seconds;
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
    write_property(self, property, from + (to - from) * shaped);
}

// ---------------------------------------------------------------------------
// GridSnap
// ---------------------------------------------------------------------------

void GridSnap::_late_update(Object &self, float delta) {
    (void)delta;
    // Round, not floor: a piece has to land on the CENTRE of its cell, and
    // flooring would drift it half a cell towards the origin every time.
    if (cell.x > 0) {
        self.position.x =
            offset.x + std::round((self.position.x - offset.x) / cell.x) * cell.x;
    }
    if (cell.y > 0) {
        self.position.y =
            offset.y + std::round((self.position.y - offset.y) / cell.y) * cell.y;
    }
}

// ---------------------------------------------------------------------------
// Parallax
// ---------------------------------------------------------------------------

void Parallax::_ready(Object &self) {
    (void)self;
    if (!texture.empty()) ours.art = rmp::assets::load_texture(texture);
}

void Parallax::_update(Object &self, float delta) {
    (void)self;
    // Only its own drift. Where the CAMERA is gets read at draw time and
    // multiplied by the factor there, so a camera that follows the player
    // scrolls every layer at its own rate with no bookkeeping in between.
    ours.scroll += speed * delta;
}

void Parallax::_draw(Object &self) {
    if (!ours.art.valid()) return;
    const Texture2D &tex = ours.art;
    if (tex.width <= 0) return;

    // The layer is drawn INSIDE the camera like everything else, so it is
    // placed relative to the view: a layer with factor 0 is pinned to the
    // screen because it moves exactly as much as the view does, and factor 1
    // stands still in the world. The object's own x shifts it on top of that.
    const Rectangle view = self.scene() != nullptr
        ? self.scene()->camera.view()
        : Rectangle{ 0, 0, static_cast<float>(APP_WINDOW_WIDTH),
                     static_cast<float>(APP_WINDOW_HEIGHT) };
    // The arithmetic is next door, in detail::parallax_tiling, and this call is
    // the only thing between it and the GPU. That is not tidiness: everything
    // below this line needs a render batch InitWindow() creates, and everything
    // above it is the part that gets written wrong -- so the part that gets
    // written wrong is the part a headless test can reach.
    const float shift = (self.position.x - view.x) * factor + ours.scroll;
    const detail::ParallaxTiling tiling =
        detail::parallax_tiling(shift, static_cast<float>(tex.width), view.width);

    for (int i = 0; i < tiling.copies; i++) {
        const float at = view.x + tiling.offset +
            static_cast<float>(i) * static_cast<float>(tex.width);
        DrawTextureV(tex, Vector2{ at, view.y + y }, tint);
    }
}

namespace detail {

ParallaxTiling parallax_tiling(float shift, float width, float screen) {
    if (width <= 0) return ParallaxTiling{};
    // Modulo the texture's width, and drawn MORE THAN ONCE. This is the whole
    // behavior: one copy leaves a gap the moment the offset passes the width,
    // and getting the next copy's position wrong by a pixel is a seam that
    // crawls across the screen. std::fmod returns a NEGATIVE for a negative
    // argument and a positive for a positive one, and a positive offset is a
    // gap down the left-hand side -- which is why it is nudged back into range
    // rather than trusted.
    float offset = std::fmod(shift, width);
    if (offset > 0) offset -= width;

    // An integer count, with each copy's position derived from it. Stepping a
    // float by `width` accumulates error across the copies, and error between
    // two copies of a scrolling background IS the seam this behavior exists to
    // remove: a gap of a third of a pixel that crawls sideways.
    const int copies = static_cast<int>((screen - offset) / width) + 1;
    return ParallaxTiling{ offset, copies };
}

} // namespace detail

// ---------------------------------------------------------------------------
// Spawner
// ---------------------------------------------------------------------------

namespace {

// The cap this spawner can actually hold to. A live cap needs a handle per live
// object, and there are kMaxSpawned of them -- so a larger max_alive is a
// number the behavior cannot honour, and saying so once is better than quietly
// enforcing a different one.
int spawner_cap(int max_alive) {
    if (max_alive <= kMaxSpawned) return max_alive;
    RMP_REPORT_ONCE("BEHAVIOR: Spawner::max_alive = %d, and a spawner tracks %d live "
                    "objects. Capping at %d -- for more than that, pool the objects "
                    "instead of making new ones, or use a second spawner.",
                    max_alive, kMaxSpawned, kMaxSpawned);
    return kMaxSpawned;
}

} // namespace

void Spawner::_update(Object &self, float delta) {
    Scene *scene = self.scene();
    if (scene == nullptr || !on_spawn) return;

    Object *ruler = track ? track.get() : &self;
    if (ruler == nullptr) ruler = &self;

    if (!ours.started) {
        ours.started = true;
        ours.last_at = ruler->position;
        ours.countdown = every_seconds;
        ours.next_at = every_distance;
        ours.travelled = 0;
    }

    const float moved = std::sqrt(
        (ruler->position.x - ours.last_at.x) * (ruler->position.x - ours.last_at.x) +
        (ruler->position.y - ours.last_at.y) * (ruler->position.y - ours.last_at.y));
    ours.travelled += moved;
    ours.last_at = ruler->position;

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
        ours.countdown -= delta;
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
    for (int i = 0; i < ours.made_count; i++) {
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
    on_spawn(*scene, ruler->position);

    for (Object *made : rmp::objects::detail::live_objects(*scene)) {
        if (std::ranges::binary_search(before, made)) continue;
        if (ours.made_count >= kMaxSpawned) break;
        ours.made[ours.made_count++] = made->handle();
    }
}

int Spawner::alive() const {
    int count = 0;
    for (int i = 0; i < ours.made_count; i++) {
        if (ours.made[i]) count++;
    }
    return count;
}

float Spawner::jitter_amount() const {
    if (jitter <= 0) return 0;
    return rmp::random::range(-jitter, jitter);
}

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------

void Timer::_update(Object &self, float delta) {
    if (ours.spent || seconds <= 0) return;
    ours.elapsed += delta;
    // `while` and not `if`: a frame longer than the interval owes more than one
    // tick, and dropping them makes a countdown run slow on a slow machine.
    // [app] max_delta bounds how many that can ever be.
    while (ours.elapsed >= seconds) {
        ours.elapsed -= seconds;
        if (on_timeout) on_timeout(self);
        if (!self.alive()) return;
        if (!repeat) {
            ours.spent = true;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Lifespan
// ---------------------------------------------------------------------------

void Lifespan::_update(Object &self, float delta) {
    ours.elapsed += delta;
    if (ours.elapsed >= seconds) self.destroy();
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

bool Health::damage(Object &self, int amount) {
    if (amount <= 0 || ours.died) return false;
    if (ours.invulnerable_left > 0) return false; // the window, and the point of it

    hp -= amount;
    if (on_damage) on_damage(self, amount);
    ours.invulnerable_left = invulnerable_for;

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
    hp += amount;
    if (hp > max_hp) hp = max_hp;
}

void Health::_update(Object &self, float delta) {
    if (ours.invulnerable_left <= 0) {
        if (ours.hid) {
            self.visible = true;
            ours.hid = false;
        }
        return;
    }
    ours.invulnerable_left -= delta;
    if (ours.invulnerable_left <= 0) {
        ours.invulnerable_left = 0;
        self.visible = true;
        ours.hid = false;
        return;
    }
    if (blink_hz > 0) {
        const float phase = ours.invulnerable_left * blink_hz;
        const bool hide = (static_cast<int>(phase * 2) % 2) == 1;
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
