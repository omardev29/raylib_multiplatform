// ---------------------------------------------------------------------------
// examples/games/04_top_down/src/main.cpp — a top-down room, with a way out.
//
// The fourth judge, and the one that shows the collision layers doing the work
// they exist for: the player's shot does not hit the player, the enemies walk
// through each other but not through the walls, the door sees the player and
// nothing else, and what may hurt the player is one field and not an `if` at
// the top of a _collision. Every one of those is a pair of integers, and they
// are named once in include/layers.h.
//
// A whole level: clear the room or don't, walk out of the door, or die trying.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include "layers.h"

namespace {

constexpr int kEnemies = 5;
constexpr int kPlayerHp = 5;
constexpr float kDoorY = 225; // the gap in the right-hand wall

// The end of a level, PUSHED on top of it: the room below freezes, stays on
// screen and stops hearing the keyboard, so this scene writes no policy at all
// -- the pause overlay of examples/scenes/01_stack with another label.
template <class Game> class OverScene : public rmp::Scene {
public:
    explicit OverScene(const char *said) : said_(said) {}

    void _draw() override {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 190 });
        rmp::ui::begin();
        rmp::ui::text(said_, { .size = rmp::ui::Size::LARGE });
        if (rmp::ui::button("Play again")) rmp::Scene::change<Game>();
        rmp::ui::end();
    }

private:
    const char *said_;
};

class TopDownScene : public rmp::Scene {
public:
    void _ready() override {
        background = Color{ 22, 20, 24, 255 };

        // The floor. An object at a layer below everything else rather than a
        // background colour, and out of the collision pass entirely: layer 0
        // and mask 0 is the pair that says "I am scenery".
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
        player.collision_layer = layer::kPlayer;
        player.collision_mask = layer::kWorld | layer::kEnemy | layer::kTrigger;
        // Eight directions, normalised, so the diagonal is not 41 % faster.
        player.add<rmp::behavior::TopDown>({ .speed = 240 });
        // WHAT IS ALLOWED TO HURT IT. Without `hurt_by` a Health is a
        // hit-point counter that nothing ever reduces, which is how this
        // player used to walk through five enemies and not notice.
        player.add<rmp::behavior::Health>({
            .hp = kPlayerHp,
            .invulnerable_for = 0.8f,
            .destroy_on_death = false, // it stays on screen under the overlay
            .on_death =
                [](rmp::Object &) {
                    rmp::Scene::push<OverScene<TopDownScene>>("You died");
                },
            .hurt_by = layer::kEnemy,
        });
        player_ = player.handle();

        for (int i = 0; i < kEnemies; i++) add_enemy(560 + static_cast<float>(i) * 30);
    }

    void _update(float) override {
        if (rmp::input::just_pressed("ui_accept")) shoot();
    }

    void _draw() override {
        auto *health = player_->get<rmp::behavior::Health>();
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::row({ .gap = 16 }, [&] {
            rmp::ui::progress(static_cast<float>(health->hp) / kPlayerHp,
                              { .width = 150, .fill = rmp::ui::current_theme().danger });
            rmp::ui::text(TextFormat("Enemies %d", enemies_));
        });
        rmp::ui::end();
    }

private:
    void add_wall(Vector2 at, Vector2 size) {
        auto &wall = spawn({ .position = at, .size = size });
        wall.solid = true;
        wall.immovable = true;
        wall.collision_layer = layer::kWorld;
        wall.collision_mask = 0;
        wall.shape.color = DARKGRAY;
    }

    // The way out: not solid, so it is walked INTO rather than bumped against,
    // and on a layer of its own that only the player's bit is in -- an enemy
    // standing in the doorway does not finish the level.
    void add_door() {
        auto &door = spawn({ .position = { 780, kDoorY }, .size = { 34, 90 } });
        door.shape.color = GOLD;
        door.collision_layer = layer::kTrigger;
        door.collision_mask = layer::kPlayer;
        door.on_collision([](rmp::Object &, rmp::Object &) {
            rmp::Scene::push<OverScene<TopDownScene>>("Level cleared");
        });
    }

    void add_enemy(float x) {
        auto &enemy = spawn({ .position = { x, 225 }, .shape = rmp::circle(14) });
        enemy.solid = true;
        enemy.collision_layer = layer::kEnemy;
        // Walls, bullets and the player, not other enemies: they walk through
        // each other.
        enemy.collision_mask = layer::kWorld | layer::kBullet | layer::kPlayer;
        enemy.shape.color = MAROON;
        enemy.add<rmp::behavior::Follow>({ .target = player_, .speed = 90 });
        // Two hits, and what may land them. No `if` at the top of a
        // _collision, and no counter of our own: `hurt_by` is the rule and
        // on_death is what the room does about it.
        enemy.add<rmp::behavior::Health>({
            .hp = 2,
            .invulnerable_for = 0.1f,
            .on_death = [this](rmp::Object &) { enemies_--; },
            .hurt_by = layer::kBullet,
        });
        enemies_++;
    }

    void shoot() {
        // The direction the character is facing, which is what TopDown keeps.
        const Vector2 aim = player_->get<rmp::behavior::TopDown>()->direction();
        auto &shot = spawn({ .position = player_->position, .shape = rmp::circle(4) });
        shot.shape.color = GOLD;
        shot.velocity = { aim.x * 520, aim.y * 520 };
        shot.collision_layer = layer::kBullet;
        shot.collision_mask = layer::kEnemy | layer::kWorld; // never the player
        shot.add<rmp::behavior::Projectile>();
        shot.add<rmp::behavior::Lifespan>({ .seconds = 1.5f });
    }

    // Between frames, a handle -- and Follow takes one for the same reason:
    // what is being chased is the thing most likely to die while it is chased.
    rmp::Handle<rmp::Object> player_;
    int enemies_ = 0;
};

} // namespace

RMP_GAME(TopDownScene);
