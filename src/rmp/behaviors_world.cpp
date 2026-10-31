// ---------------------------------------------------------------------------
// The catalogue's second half, what happens between objects: Area, Hitbox,
// Collectible, Shooter, Orbit, Draggable and Animator. The surface is
// include/rmp/behavior.h; the shared arithmetic is src/rmp/behaviors.cpp's,
// through behavior_internal.h.
//
// Like the first half, ordinary code against the public API, with the same two
// places it reaches past it: the scene's live objects (a Collectible's magnet,
// a Draggable looking for the Area it was let go over) and a sheet's tags (an
// Animator skipping a tag the sheet has not got).
// ---------------------------------------------------------------------------

#include <rmp/behavior.h>

#include <rmp/audio.h>
#include <rmp/input.h>
#include <rmp/tilemap.h>

#include "animation_internal.h"
#include "behavior_internal.h"
#include "internal.h"
#include "object_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

// Every hook below is one half of an interface: its signature is the
// contract's, and clang-tidy's advice to make one static or const would break
// it (src/rmp/behaviors.cpp says the same, at more length).
//
// NOLINTBEGIN(readability-convert-member-functions-to-static,readability-make-member-function-const,readability-named-parameter)
namespace rmp::behavior {

namespace {

using detail::finite;
using detail::length_of;
using detail::NEAR_ZERO;
using detail::normalised;
using detail::put;
using detail::say_nan;
using detail::seconds_of;

constexpr float DEGREES = 180.0f / std::numbers::pi_v<float>;

// The most bullets one volley spawns. A count past it is a mistake, said once.
constexpr int MOST_PER_VOLLEY = 256;

// How far a collected pickup rises while it vanishes, and how much it grows.
constexpr float VANISH_RISE = 18;
constexpr float VANISH_GROWTH = 0.6f;

// How fast a rejected Draggable flies back to where it was picked up.
constexpr float RETURN_SPEED = 900;

Vector2 minus(Vector2 a, Vector2 b) { return Vector2{ a.x - b.x, a.y - b.y }; }

Vector2 times(Vector2 v, float k) { return Vector2{ v.x * k, v.y * k }; }

bool has_box(const Rectangle &r) { return r.width > 0 && r.height > 0; }

// Whether `inner` lies wholly within `outer`, edges included.
bool wholly_within(const Rectangle &inner, const Rectangle &outer) {
    if (!has_box(inner) || !has_box(outer)) return false;
    return inner.x >= outer.x && inner.y >= outer.y &&
        inner.x + inner.width <= outer.x + outer.width &&
        inner.y + inner.height <= outer.y + outer.height;
}

bool contains_point(const Rectangle &r, Vector2 p) {
    return has_box(r) && p.x >= r.x && p.y >= r.y && p.x <= r.x + r.width &&
        p.y <= r.y + r.height;
}

template <class T> std::ptrdiff_t index_of(const std::vector<T> &list, const T &value) {
    const auto at = std::ranges::find(list, value);
    return at == list.end() ? -1 : at - list.begin();
}

// The opacity an object is drawn with: its sprite's tint when it has a
// picture, its shape's colour when it has none.
bool draws_a_picture(const Object &self) {
    return self.sprite.sheet.valid() || self.sprite.texture.valid();
}
unsigned char alpha_of(const Object &self) {
    return draws_a_picture(self) ? self.sprite.tint.a : self.shape.color.a;
}
void set_alpha(Object &self, unsigned char alpha) {
    if (draws_a_picture(self)) {
        self.sprite.tint.a = alpha;
    } else {
        self.shape.color.a = alpha;
    }
}

// Whether the sheet on `self` has `tag`. Without a sheet nothing is played,
// and Sprite::play() warns about a tag that is not there -- which, for a tag
// an Animator only tries, is not a mistake.
bool sheet_has(const Object &self, std::string_view tag) {
    if (tag.empty() || !self.sprite.sheet.valid()) return false;
    return rmp::animation::detail::tag_index(self.sprite.sheet.raw(), tag) >= 0;
}

// The way an object faces when nobody said: a TopDown's direction, or right
// unless it is mirrored.
Vector2 facing_of(const Object &self) {
    if (const Ref<TopDown> walk = self.get<TopDown>()) return walk->direction();
    return Vector2{ self.flip_x ? -1.0f : 1.0f, 0 };
}

} // namespace

// ---------------------------------------------------------------------------
// Area
// ---------------------------------------------------------------------------

bool Area::contains(const Object &other) const {
    return index_of(ours.touching, other.handle()) >= 0;
}

void Area::_update(Object &self, float delta) {
    (void)seconds_of(delta);
    // The pass that ended last frame is settled here: whoever touched before
    // it and not in it has left. One the game destroyed meanwhile is gone,
    // and is told nothing -- there is no object left to hand over.
    for (std::size_t i = 0; i < ours.touching.size(); ++i) {
        if (index_of(ours.now, ours.touching[i]) >= 0) continue;
        const Ref<Object> gone = ours.touching[i].get();
        if (!gone || !gone->alive()) continue;
        if (ours.inside[i]) on_outside(self, *gone);
        if (!self.alive()) break;
        on_exit(self, *gone);
        if (!self.alive()) break;
    }
    ours.touching.swap(ours.now);
    ours.inside.swap(ours.now_inside);
    ours.now.clear();
    ours.now_inside.clear();
}

void Area::_collision(Object &self, Object &other) {
    const Handle<Object> who = other.handle();
    if (index_of(ours.now, who) >= 0) return; // once per pair per frame already
    const bool inside = wholly_within(other.world_collider(), self.world_collider());
    ours.now.push_back(who);
    ours.now_inside.push_back(inside);

    const std::ptrdiff_t before = index_of(ours.touching, who);
    if (before < 0) {
        on_enter(self, other);
        if (inside && self.alive() && other.alive()) on_inside(self, other);
        return;
    }
    on_stay(self, other);
    const bool was = ours.inside[static_cast<std::size_t>(before)];
    if (!self.alive() || !other.alive()) return;
    if (inside && !was) on_inside(self, other);
    if (!inside && was) on_outside(self, other);
}

// ---------------------------------------------------------------------------
// Hitbox
// ---------------------------------------------------------------------------

Hitbox Hitbox::from(const MapObject &object) {
    Hitbox made;
    made.damage = object.property_int("damage", made.damage);
    made.knockback = object.property_float("knockback", made.knockback);
    return made;
}

// cppcheck-suppress constParameterReference ; a hook's signature is the API's
void Hitbox::_update(Object &self, float delta) {
    (void)self;
    (void)seconds_of(delta);
    // What it struck and is gone cannot be struck again; the list keeps only
    // the living.
    std::erase_if(ours.struck, [](const Handle<Object> &h) { return !h; });
}

void Hitbox::_collision(Object &self, Object &other) {
    say_nan("Hitbox", { { "knockback", knockback } });
    if ((other.collision_layer & hits) == 0) return;
    const Ref<Health> health = other.get<Health>();
    if (!health) return;
    const Handle<Object> who = other.handle();
    if (once_per_target && index_of(ours.struck, who) >= 0) return;

    const Vector2 from = self.position;
    if (health->damage(other, damage)) {
        if (once_per_target) ours.struck.push_back(who);
        if (knockback > 0 && std::isfinite(knockback) && other.alive()) {
            Vector2 away = normalised(minus(other.position, from));
            if (away.x == 0 && away.y == 0) away = normalised(self.velocity);
            if (away.x == 0 && away.y == 0) away = Vector2{ 0, -1 };
            other.apply_impulse(times(away, knockback));
        }
        on_hit(self, other);
    }
    if (destroy_on_hit && self.alive()) self.destroy();
}

// ---------------------------------------------------------------------------
// Collectible
// ---------------------------------------------------------------------------

Collectible Collectible::from(const MapObject &object) {
    Collectible made;
    made.magnet = object.property_float("magnet", made.magnet);
    return made;
}

void Collectible::_collision(Object &self, Object &other) {
    if (ours.collected) return;
    if ((other.collision_layer & collected_by) == 0) return;
    ours.collected = true;
    ours.scale = self.scale;
    ours.alpha = alpha_of(self);
    // It collides with nothing from here: picked up once, by one collector.
    self.collision_layer = 0;
    self.collision_mask = 0;
    self.gravity_scale = 0;
    const bool lingers = vanish_seconds > 0 && std::isfinite(vanish_seconds);
    self.velocity = Vector2{};
    if (lingers)
        put(self.velocity, Vector2{ 0, -VANISH_RISE / vanish_seconds }, "Collectible");
    if (!sound.empty()) rmp::audio::play(sound);
    on_collect(self, other);
    if (!lingers && self.alive()) self.destroy();
}

void Collectible::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("Collectible",
            { { "magnet", magnet },
              { "magnet_speed", magnet_speed },
              { "vanish_seconds", vanish_seconds } });
    if (ours.collected) {
        ours.vanished += seconds;
        const float t = vanish_seconds > 0
            ? std::clamp(ours.vanished / vanish_seconds, 0.0f, 1.0f)
            : 1.0f;
        put(self.scale, times(ours.scale, 1 + VANISH_GROWTH * t), "Collectible");
        set_alpha(self,
                  static_cast<unsigned char>(static_cast<float>(ours.alpha) * (1 - t)));
        if (t >= 1) self.destroy();
        return;
    }
    if (!(magnet > 0)) return;
    const Ref<Scene> scene = Storage::spawned_in(self);
    if (!scene) return;
    // The nearest collector inside the magnet's reach, centre to centre.
    float best = magnet;
    Vector2 towards{};
    bool found = false;
    const std::vector<Object *> everyone = objects::detail::live_objects(*scene);
    for (const auto &entry : everyone) {
        const Object &other = *entry;
        if (&other == &self || !other.alive()) continue;
        if ((other.collision_layer & collected_by) == 0) continue;
        const Vector2 to = minus(other.position, self.position);
        const float d = length_of(to);
        if (d <= best) {
            best = d;
            towards = to;
            found = true;
        }
    }
    if (found) {
        put(self.velocity, times(normalised(towards), magnet_speed), "Collectible");
        ours.pulled = true;
    } else if (ours.pulled) {
        self.velocity = Vector2{};
        ours.pulled = false;
    }
}

// ---------------------------------------------------------------------------
// Shooter
// ---------------------------------------------------------------------------

Shooter Shooter::from(const MapObject &object) {
    Shooter made;
    made.every = object.property_float("every", made.every);
    made.speed = object.property_float("speed", made.speed);
    made.count = object.property_int("count", made.count);
    made.spread = object.property_float("spread", made.spread);
    made.rotate_per_shot = object.property_float("rotate", made.rotate_per_shot);
    made.range = object.property_float("range", made.range);
    return made;
}

bool Shooter::fire(Object &self) {
    say_nan("Shooter",
            { { "every", every },
              { "speed", speed },
              { "spread", spread },
              { "rotate_per_shot", rotate_per_shot } });
    if (ours.cooldown > 0) return false;
    const Ref<Scene> scene = Storage::spawned_in(self);
    if (!scene) return false;

    const Vector2 from{ self.position.x + (self.flip_x ? -offset.x : offset.x),
                        self.position.y + offset.y };
    Vector2 aim = direction;
    if (target != Handle<Object>{}) {
        const Ref<Object> at = target.get();
        if (!at) return false; // set, and gone: nothing to shoot at
        aim = minus(at->position, from);
    } else if (aim.x == 0 && aim.y == 0) {
        aim = facing_of(self);
    }
    aim = normalised(aim);
    if (aim.x == 0 && aim.y == 0) return false;

    if (count > MOST_PER_VOLLEY) {
        RMP_REPORT_ONCE("BEHAVIOR: a Shooter's count is %d; one volley fires %d at most.",
                        count, MOST_PER_VOLLEY);
    }
    const int n = std::clamp(count, 1, MOST_PER_VOLLEY);
    const float arc = std::isfinite(spread) ? std::clamp(spread, 0.0f, 360.0f) : 0.0f;
    float step = 0;
    float first = std::atan2(aim.y, aim.x) * DEGREES + ours.turned;
    if (n > 1 && arc >= 360) {
        step = 360.0f / static_cast<float>(n);
    } else if (n > 1) {
        step = arc / static_cast<float>(n - 1);
        first -= arc / 2;
    }

    const unsigned mask =
        hits != 0 ? hits : (self.collision_mask & ~self.collision_layer);
    for (int i = 0; i < n; ++i) {
        const float radians = (first + step * static_cast<float>(i)) / DEGREES;
        const Vector2 velocity{ std::cos(radians) * speed, std::sin(radians) * speed };
        if (!finite(velocity)) continue;
        if (on_shoot) {
            on_shoot(*scene, from, velocity);
            continue;
        }
        Shape round = rmp::circle(bullet_radius > 0 ? bullet_radius : 1);
        round.color = bullet_color;
        Object &bullet =
            scene->spawn({ .position = from, .shape = round, .velocity = velocity });
        bullet.collision_layer = 0;
        bullet.collision_mask = mask;
        // The Hitbox first: the Projectile's _collision destroys the bullet,
        // and a behavior after it would not be told of the touch at all.
        bullet.add<Hitbox>({ .damage = damage, .hits = mask, .destroy_on_hit = true });
        bullet.add<Projectile>(
            { .speed = 0, .destroy_on_hit = true, .destroy_on_wall = true });
        bullet.add<Lifespan>({ .seconds = bullet_life });
    }
    if (std::isfinite(rotate_per_shot)) {
        ours.turned = std::fmod(ours.turned + rotate_per_shot, 360.0f);
    }
    ours.cooldown = every > 0 && std::isfinite(every) ? every : 0;
    ++ours.shots;
    if (!sound.empty()) rmp::audio::play(sound);
    return true;
}

void Shooter::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    if (ours.cooldown > 0) ours.cooldown -= seconds;
    bool wants = automatic || (!action.empty() && rmp::input::pressed(action));
    if (wants && range > 0 && target != Handle<Object>{}) {
        const Ref<Object> at = target.get();
        wants = at && length_of(minus(at->position, self.position)) <= range;
    }
    if (wants) (void)fire(self);
}

// ---------------------------------------------------------------------------
// Orbit
// ---------------------------------------------------------------------------

Orbit Orbit::from(const MapObject &object) {
    Orbit made;
    made.radius = object.property_float("radius", made.radius);
    made.degrees_per_second = object.property_float("speed", made.degrees_per_second);
    made.angle = object.property_float("angle", made.angle);
    return made;
}

void Orbit::_late_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("Orbit",
            { { "radius", radius },
              { "degrees_per_second", degrees_per_second },
              { "angle", angle } });
    Vector2 around = point;
    if (centre != Handle<Object>{}) {
        const Ref<Object> c = centre.get();
        if (!c) return; // what it circled is gone: it stays where it is
        around = c->position;
    }
    const float turned = angle + degrees_per_second * seconds;
    if (std::isfinite(turned)) angle = std::fmod(turned, 360.0f);
    const float share =
        of > 1 ? 360.0f * static_cast<float>(index) / static_cast<float>(of) : 0;
    const float radians = (angle + share) / DEGREES;
    const Vector2 out{ std::cos(radians), std::sin(radians) };
    put(self.position, Vector2{ around.x + out.x * radius, around.y + out.y * radius },
        "Orbit");
    self.velocity = Vector2{};
    if (face == Face::CENTRE) put(self.rotation, angle + share + 180, "Orbit");
    if (face == Face::OUTWARD) put(self.rotation, angle + share, "Orbit");
}

// ---------------------------------------------------------------------------
// Animator
// ---------------------------------------------------------------------------

void Animator::_late_update(Object &self, float delta) {
    (void)seconds_of(delta);
    say_nan("Animator", { { "moving_at", moving_at } });
    if (flip) {
        if (self.velocity.x < -NEAR_ZERO) self.flip_x = true;
        if (self.velocity.x > NEAR_ZERO) self.flip_x = false;
    }

    std::string_view chosen;
    bool once = false;
    for (const AnimationRule &rule : rules) {
        if (rule.when(self) && sheet_has(self, rule.tag)) {
            chosen = rule.tag;
            break;
        }
    }
    if (chosen.empty()) {
        const Ref<Health> health = self.get<Health>();
        const bool airborne = self.gravity_scale != 0 && [&] {
            if (const Ref<Platformer> p = self.get<Platformer>()) return !p->grounded();
            if (const Ref<Runner> r = self.get<Runner>()) return !r->ours.grounded;
            return std::fabs(self.velocity.y) > moving_at;
        }();
        const bool moving = length_of(self.velocity) > moving_at;
        const std::array<std::string_view, 6> order{
            health && health->dead() ? std::string_view(die) : std::string_view(),
            health && health->invulnerable() ? std::string_view(hurt)
                                             : std::string_view(),
            airborne && self.velocity.y < 0 ? std::string_view(jump) : std::string_view(),
            airborne && self.velocity.y >= 0 ? std::string_view(fall)
                                             : std::string_view(),
            moving ? std::string_view(run) : std::string_view(),
            std::string_view(idle),
        };
        for (const std::string_view tag : order) {
            if (sheet_has(self, tag)) {
                chosen = tag;
                break;
            }
        }
        once = !chosen.empty() && chosen == die;
    }
    if (chosen.empty()) {
        ours.chosen.clear();
        return;
    }
    if (self.sprite.playing() != chosen) self.sprite.play(chosen, !once);
    ours.chosen = std::string(chosen);
}

// ---------------------------------------------------------------------------
// Draggable
// ---------------------------------------------------------------------------

void Draggable::_ready(Object &self) {
    // The object's on_drag is how the pointer reaches it; what it is handed is
    // kept, and _update does the moving, so the rules -- the bounds, a paused
    // Draggable -- apply in one place.
    self.on_drag([](Object &dragged, Vector2 moved) {
        if (const Ref<Draggable> d = dragged.get<Draggable>()) {
            if (!dragged.enabled<Draggable>()) return;
            d->ours.dragged.x += moved.x;
            d->ours.dragged.y += moved.y;
            d->ours.moved = true;
        }
    });
}

void Draggable::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("Draggable", { { "friction", friction } });
    if (ours.moved) {
        if (!ours.held) {
            ours.held = true;
            ours.going_back = false;
            ours.picked_at = self.position;
            ours.pace = Vector2{};
            on_pick(self);
        }
        Vector2 to{ self.position.x + ours.dragged.x, self.position.y + ours.dragged.y };
        if (has_box(bounds)) {
            to.x = std::clamp(to.x, bounds.x, bounds.x + bounds.width);
            to.y = std::clamp(to.y, bounds.y, bounds.y + bounds.height);
        }
        if (seconds > 0) {
            // The pointer's pace, smoothed over the last few frames: what a
            // throw keeps.
            const Vector2 now{ (to.x - self.position.x) / seconds,
                               (to.y - self.position.y) / seconds };
            if (finite(now)) {
                ours.pace = Vector2{ ours.pace.x * 0.5f + now.x * 0.5f,
                                     ours.pace.y * 0.5f + now.y * 0.5f };
            }
        }
        put(self.position, to, "Draggable");
        self.velocity = Vector2{};
        ours.dragged = Vector2{};
        ours.moved = false;
        return;
    }
    if (ours.held && !rmp::input::pointer_down()) {
        drop(self);
        return;
    }
    if (ours.held) {
        self.velocity = Vector2{}; // held still under a pointer that does not move
        ours.pace = Vector2{};
        return;
    }
    if (ours.going_back) {
        const Vector2 to = minus(ours.picked_at, self.position);
        const float d = length_of(to);
        const float step = RETURN_SPEED * seconds;
        if (d <= step || d < NEAR_ZERO) {
            self.position = ours.picked_at;
            self.velocity = Vector2{};
            ours.going_back = false;
        } else {
            put(self.velocity, times(normalised(to), RETURN_SPEED), "Draggable");
        }
        return;
    }
    if (throwable && (self.velocity.x != 0 || self.velocity.y != 0)) {
        const float keep = std::clamp(1 - friction * seconds, 0.0f, 1.0f);
        self.velocity = times(self.velocity, keep);
        if (length_of(self.velocity) < 1) self.velocity = Vector2{};
        if (has_box(bounds) && !contains_point(bounds, self.position)) {
            self.position.x =
                std::clamp(self.position.x, bounds.x, bounds.x + bounds.width);
            self.position.y =
                std::clamp(self.position.y, bounds.y, bounds.y + bounds.height);
            self.velocity = Vector2{};
        }
    }
}

void Draggable::drop(Object &self) {
    ours.held = false;
    ours.moved = false;
    ours.dragged = Vector2{};
    // The Area it was let go over: the one whose collider holds its centre,
    // nearest centre first when two do.
    const std::vector<Object *> everyone = [&] {
        const Ref<Scene> scene = Storage::spawned_in(self);
        return scene ? objects::detail::live_objects(*scene) : std::vector<Object *>{};
    }();
    std::size_t landing = everyone.size();
    float best = 0;
    for (std::size_t i = 0; i < everyone.size(); ++i) {
        const Object &other = *everyone[i];
        if (&other == &self || !other.alive() || !other.has<Area>()) continue;
        if ((other.collision_layer & drop_on) == 0) continue;
        if (!contains_point(other.world_collider(), self.position)) continue;
        const float d = length_of(minus(other.position, self.position));
        if (landing == everyone.size() || d < best) {
            landing = i;
            best = d;
        }
    }
    if (landing < everyone.size() && (!accepts || accepts(self, *everyone[landing]))) {
        Object &area = *everyone[landing];
        if (centre_on_drop) self.position = area.position;
        self.velocity = Vector2{};
        on_drop(self, area);
        return;
    }
    on_reject(self);
    if (return_if_rejected) {
        ours.going_back = true;
        return;
    }
    if (snap.x > 0 || snap.y > 0) {
        say_nan("Draggable", { { "snap.x", snap.x }, { "snap.y", snap.y } });
        if (snap.x > 0)
            put(self.position.x, std::round(self.position.x / snap.x) * snap.x,
                "Draggable");
        if (snap.y > 0)
            put(self.position.y, std::round(self.position.y / snap.y) * snap.y,
                "Draggable");
        self.velocity = Vector2{};
        return;
    }
    if (throwable) put(self.velocity, ours.pace, "Draggable");
}

} // namespace rmp::behavior
// NOLINTEND(readability-convert-member-functions-to-static,readability-make-member-function-const,readability-named-parameter)
