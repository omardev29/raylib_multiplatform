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
#include <rmp/random.h>

#include "animation_internal.h"

#include <cmath>
#include <numbers>

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
const char *or_default(const char *given, const char *fallback) {
    return (given != nullptr && given[0] != '\0') ? given : fallback;
}

bool held(const char *action) { return rmp::input::pressed(action); }
bool pressed(const char *action) { return rmp::input::just_pressed(action); }

// Is there something solid directly below? A raycast rather than a flag on the
// object, because the collision pass does not keep a contact list -- and a ray
// is what a game would write anyway, only correct the first time.
bool standing_on_something(Object &self, float reach) {
    Scene *scene = self.scene();
    if (scene == nullptr) return false;
    const Rectangle box = self.world_collider();
    if (box.height <= 0) return false;
    const Vector2 feet{ box.x + box.width / 2, box.y + box.height };
    const RayHit hit = scene->raycast({ .from = Vector2{ feet.x, feet.y - 1 },
                                        .to = Vector2{ feet.x, feet.y + reach },
                                        .mask = self.collision_mask,
                                        .ignore = &self });
    return static_cast<bool>(hit) && hit.object->solid;
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
    const char *base = moving ? walk : idle;
    if (base == nullptr || base[0] == '\0') return;

    if (moving) {
        char tag[kMaxTagName * 2];
        const char *suffix = suffixes[sector()];
        int at = 0;
        for (int i = 0; base[i] != '\0' && at + 1 < static_cast<int>(sizeof(tag)); i++) {
            tag[at++] = base[i];
        }
        if (suffix != nullptr && suffix[0] != '\0' &&
            at + 1 < static_cast<int>(sizeof(tag))) {
            tag[at++] = '_';
            for (int i = 0; suffix[i] != '\0' && at + 1 < static_cast<int>(sizeof(tag));
                 i++) {
                tag[at++] = suffix[i];
            }
        }
        tag[at] = '\0';
        if (rmp::animation::detail::tag_index(self.sprite.sheet.raw(), tag) >= 0) {
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

    ours.ducking = duck_action[0] != '\0' && held(duck_action);

    ours.grounded = standing_on_something(self, 2.0f) && self.velocity.y >= -kEpsilon;
    if (ours.grounded) ours.jumps_used = 0;

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
    if (texture != nullptr && texture[0] != '\0') {
        ours.art = rmp::assets::load_texture(texture);
    }
}

void Parallax::_update(Object &self, float delta) {
    (void)self;
    // Only its own drift. Where the object IS gets read at draw time and
    // multiplied by the factor there, so moving the object moves the layer with
    // no bookkeeping in between -- which is what lets a game scroll the world
    // by moving one object and have every layer follow at its own rate.
    ours.scroll += speed * delta;
}

void Parallax::_draw(Object &self) {
    if (!ours.art.valid()) return;
    const Texture2D &tex = ours.art;
    if (tex.width <= 0) return;

    const auto width = static_cast<float>(tex.width);
    // Modulo the texture's width, and drawn TWICE. This is the whole behavior:
    // one copy leaves a gap the moment the offset passes the width, and getting
    // the second copy's position wrong by a pixel is a seam that crawls across
    // the screen. std::fmod can return a negative, which is why it is nudged
    // back into range rather than trusted.
    const float shift = self.position.x * factor + ours.scroll;
    float offset = std::fmod(shift, width);
    if (offset > 0) offset -= width;

    const auto screen =
        static_cast<float>(GetScreenWidth() > 0 ? GetScreenWidth() : APP_WINDOW_WIDTH);
    // An integer loop counter, with the position derived from it. Stepping a
    // float by `width` accumulates error across the copies, and error between
    // two copies of a scrolling background IS the seam this behavior exists to
    // remove: a gap of a third of a pixel that crawls sideways.
    const int copies = static_cast<int>((screen - offset) / width) + 1;
    for (int i = 0; i < copies; i++) {
        DrawTextureV(tex, Vector2{ offset + static_cast<float>(i) * width, y }, tint);
    }
}

// ---------------------------------------------------------------------------
// Spawner
// ---------------------------------------------------------------------------

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
    if (max_alive > 0 && ours.alive >= max_alive) return;

    const int before = scene->object_count();
    on_spawn(*scene, ruler->position);
    // However many it actually made, which is not always one: a wave spawner
    // makes five, and the cap has to count them all or it is not a cap.
    ours.alive += scene->object_count() - before;
    if (ours.alive < 0) ours.alive = 0;
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

void Health::_end(Object &self) {
    // Whatever the blink left behind, the object does not go out invisible.
    if (ours.hid) self.visible = true;
}

} // namespace rmp::behavior
// NOLINTEND(readability-convert-member-functions-to-static,readability-make-member-function-const,readability-named-parameter)
