// The hero: walks with TopDown, is drawn by an Animator, lives with a Health,
// and swings a sword -- a Hitbox that lives a fifth of a second, in front.

#include "objects.h"

#include <rmp/assets.h>
#include <rmp/audio.h>
#include <rmp/behavior.h>
#include <rmp/input.h>

#include <cmath>
#include <numbers>

namespace game {

namespace {

constexpr float SPEED = 84;
constexpr float SWING_EVERY = 0.32f;
constexpr float SWING_LASTS = 0.18f;
constexpr float SWING_REACH = 13;
constexpr float SWING_SIZE = 18;
constexpr float SWING_KNOCKBACK = 260;
constexpr float GRACE = 0.9f; // seconds safe after a hit
constexpr float SHAKE = 3;

// The blade, drawn turning through its arc while the swing lives.
struct Slash {
    float angle = 0; // degrees, where the arc is centred
    float elapsed = 0;
    void _update(rmp::Object &self, float delta) {
        (void)self;
        elapsed += delta;
    }
    void _draw(rmp::Object &self) {
        const float t = elapsed / SWING_LASTS;
        const float turn = angle - 70 + (140 * t) + 45; // the tile points up-right
        draw_tile(tiles::SWORD, self.position, WHITE, 1.2f, turn);
    }
};

} // namespace

bool is_hero(const rmp::Object &object) { return object.collision_layer == layer::HERO; }

void Hero::_ready() {
    sprite.sheet = rmp::assets::load_sheet("hero.aseprite");
    collider = rmp::rect({ 10, 10 });
    solid = true;
    collision_layer = layer::HERO;
    collision_mask =
        layer::MONSTER | layer::PICKUP | layer::TRAP | layer::PLACE | layer::WALL;
    layer = 5;

    add<rmp::behavior::TopDown>({ .speed = SPEED, .idle = "", .walk = "" });
    add<rmp::behavior::Animator>();
    add<Light>({ .radius = 78, .color = { 255, 200, 140, 80 } });
    add<rmp::behavior::Health>({
        .hp = run().hp,
        .max_hp = run().max_hp,
        .invulnerable_for = GRACE,
        .destroy_on_death = false,
        .on_death =
            [](rmp::Object &self) {
                static_cast<LevelScene &>(self.scene()).hero_died();
            },
        .on_damage =
            [](rmp::Object &self, int amount) {
                run().hp = self.get<rmp::behavior::Health>()->hp;
                self.scene().camera.shake(SHAKE, 0.25f);
                rmp::audio::play("hurt");
                sparks(self.scene(), self.position, Color{ 228, 59, 68, 255 }, 10);
                float_text(self.scene(), self.position, TextFormat("-%d", amount),
                           Color{ 255, 90, 90, 255 });
            },
    });
}

void Hero::_update(float delta) {
    _cooldown -= delta;
    const auto health = get<rmp::behavior::Health>();
    if (health && health->dead()) {
        velocity = Vector2{};
        enable<rmp::behavior::TopDown>(false);
        return;
    }
    if (health) {
        // A potion or the altar heals the run; the Health follows it.
        health->max_hp = run().max_hp;
        if (run().hp > health->hp) health->heal(run().hp - health->hp);
    }
    if (rmp::input::just_pressed("attack") && _cooldown <= 0) swing();
}

void Hero::swing() {
    _cooldown = SWING_EVERY;
    const Vector2 facing = get<rmp::behavior::TopDown>()->direction();
    const Vector2 at{ position.x + (facing.x * SWING_REACH),
                      position.y + (facing.y * SWING_REACH) };
    auto &slash = scene().spawn({ .position = at, .layer = 6 });
    slash.collider = rmp::rect({ SWING_SIZE, SWING_SIZE }); // felt, and drawn by Slash
    slash.collision_layer = layer::SWING;
    slash.collision_mask = layer::MONSTER;
    slash.add<rmp::behavior::Hitbox>({ .damage = 1,
                                       .hits = layer::MONSTER,
                                       .knockback = SWING_KNOCKBACK,
                                       .once_per_target = true,
                                       .on_hit = [](rmp::Object &, rmp::Object &victim) {
                                           rmp::audio::play("hit");
                                           sparks(victim.scene(), victim.position,
                                                  Color{ 255, 236, 180, 255 }, 6);
                                       } });
    slash.add<rmp::behavior::Lifespan>({ .seconds = SWING_LASTS });
    slash.add<Slash>(
        { .angle = std::atan2(facing.y, facing.x) * 180 / std::numbers::pi_v<float> });
    rmp::audio::play("swing", { .volume = 0.5f });
}

} // namespace game
