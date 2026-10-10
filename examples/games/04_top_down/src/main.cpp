// ---------------------------------------------------------------------------
// examples/games/04_top_down -- a top-down room, with a way out.
//
// The collision layers doing the work they exist for: the player's shot does
// not hit the player, the enemies walk through each other but not through the
// walls, the door sees the player and nothing else, and what may hurt the
// player is one field rather than an `if` in a _collision. Each of those is a
// pair of bit masks, named once in include/layers.h.
//
// A whole level: clear the room or don't, walk out of the door, or die trying.
//
// Run it: `rmp example 04_top_down`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include "layers.h"

namespace {

constexpr int ENEMIES = 5;
constexpr int PLAYER_HP = 5;
constexpr float DOOR_Y = 225; // the gap in the right-hand wall
constexpr float SHOT_SPEED = 520;

// The end of a level, PUSHED on top of it: the room below freezes, stays on
// screen and stops hearing the keyboard, which is what the scene stack does on
// its own -- so this is only what it says and the way out.
class OverScene : public rmp::Scene {
public:
    explicit OverScene(const char *said) : _said(said) {}
    void _draw() override; // below TopDownScene, which it starts again

private:
    const char *_said;
};

class TopDownScene : public rmp::Scene {
public:
    void _ready() override {
        background = Color{ 22, 20, 24, 255 };

        // The floor: an object drawn below everything else, and out of the
        // collision pass entirely -- layer 0 and mask 0 say "I am scenery".
        auto &floor =
            spawn({ .position = { 400, 225 }, .size = { 720, 370 }, .layer = -10 });
        floor.shape.color = Color{ 62, 52, 44, 255 };
        floor.collision_layer = 0;
        floor.collision_mask = 0;

        add_wall({ 400, 20 }, { 800, 40 });
        add_wall({ 400, 430 }, { 800, 40 });
        add_wall({ 20, 225 }, { 40, 450 });
        // The right-hand wall, in two pieces, and the gap between them is the
        // way out.
        add_wall({ 780, 90 }, { 40, 180 });
        add_wall({ 780, 360 }, { 40, 180 });
        add_door();

        auto &player =
            spawn({ .position = { 200, 225 }, .shape = rmp::rect({ 26, 26 }) });
        player.shape.color = rmp::ui::current_theme().primary;
        player.solid = true;
        player.collision_layer = layer::PLAYER;
        player.collision_mask = layer::WORLD | layer::ENEMY | layer::TRIGGER;
        // Eight directions, normalised, so the diagonal is not 41 % faster.
        player.add<rmp::behavior::TopDown>({ .speed = 240 });
        // What may hurt it is one field, `hurt_by`: without it a Health is a
        // hit-point counter that nothing ever reduces.
        player.add<rmp::behavior::Health>({
            .hp = PLAYER_HP,
            .invulnerable_for = 0.8f,
            .destroy_on_death = false, // it stays on screen under the overlay
            .on_death = [](rmp::Object &) { rmp::Scene::push<OverScene>("You died"); },
            .hurt_by = layer::ENEMY,
        });
        _player = player.handle();

        for (int i = 0; i < ENEMIES; i++) add_enemy(560 + static_cast<float>(i) * 30);
    }

    void _update(float) override {
        if (rmp::input::just_pressed("ui_accept")) shoot();
    }

    void _draw() override {
        auto health = _player->get<rmp::behavior::Health>();
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::row({ .gap = 16 }, [&] {
            rmp::ui::progress(static_cast<float>(health->hp) / PLAYER_HP,
                              { .width = 150, .fill = rmp::ui::current_theme().danger });
            rmp::ui::text(TextFormat("Enemies %d", _enemies));
        });
        rmp::ui::end();
    }

private:
    void add_wall(Vector2 at, Vector2 size) {
        auto &wall = spawn({ .position = at, .size = size });
        wall.solid = true;
        wall.immovable = true;
        wall.collision_layer = layer::WORLD;
        wall.collision_mask = 0;
        wall.shape.color = DARKGRAY;
    }

    // The way out: not solid, so it is walked INTO rather than bumped against,
    // and on a layer of its own that only the player's bit is in -- an enemy
    // standing in the doorway does not finish the level.
    void add_door() {
        auto &door = spawn({ .position = { 780, DOOR_Y }, .size = { 34, 90 } });
        door.shape.color = GOLD;
        door.collision_layer = layer::TRIGGER;
        door.collision_mask = layer::PLAYER;
        door.on_collision([](rmp::Object &, rmp::Object &) {
            rmp::Scene::push<OverScene>("Level cleared");
        });
    }

    void add_enemy(float x) {
        auto &enemy = spawn({ .position = { x, 225 }, .shape = rmp::circle(14) });
        enemy.solid = true;
        enemy.collision_layer = layer::ENEMY;
        // Walls, bullets and the player, not other enemies: they walk through
        // each other.
        enemy.collision_mask = layer::WORLD | layer::BULLET | layer::PLAYER;
        enemy.shape.color = MAROON;
        enemy.add<rmp::behavior::Follow>({ .target = _player, .speed = 90 });
        // Two hits, and only a bullet lands one: `hurt_by` is the rule, and
        // on_death is what the room does about it.
        enemy.add<rmp::behavior::Health>({
            .hp = 2,
            .invulnerable_for = 0.1f,
            .on_death = [this](rmp::Object &) { _enemies--; },
            .hurt_by = layer::BULLET,
        });
        _enemies++;
    }

    void shoot() {
        // The direction the character is facing, which is what TopDown keeps.
        const Vector2 aim = _player->get<rmp::behavior::TopDown>()->direction();
        auto &shot = spawn({ .position = _player->position, .shape = rmp::circle(4) });
        shot.shape.color = GOLD;
        shot.velocity = { aim.x * SHOT_SPEED, aim.y * SHOT_SPEED };
        shot.collision_layer = layer::BULLET;
        shot.collision_mask = layer::ENEMY | layer::WORLD; // never the player
        shot.add<rmp::behavior::Projectile>();
        shot.add<rmp::behavior::Lifespan>({ .seconds = 1.5f });
    }

    // Between frames, a handle -- and Follow takes one for the same reason:
    // what is being chased is the thing most likely to die while it is chased.
    rmp::Handle<rmp::Object> _player;
    int _enemies = 0;
};

void OverScene::_draw() {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 190 });
    rmp::ui::begin();
    rmp::ui::text(_said, { .size = rmp::ui::Size::LARGE });
    if (rmp::ui::button("Play again")) rmp::Scene::change<TopDownScene>();
    rmp::ui::end();
}

} // namespace

RMP_GAME(TopDownScene);
