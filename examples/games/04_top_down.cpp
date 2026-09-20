// ---------------------------------------------------------------------------
// examples/games/04_top_down.cpp — a top-down room, with enemies that chase.
//
// The fourth judge, and the one that shows the collision layers doing the work
// they exist for: the player's shot does not hit the player, the enemies walk
// through each other but not through the walls, and the exit trigger sees the
// player and nothing else. Every one of those is a pair of integers rather than
// an `if` at the top of a _collision.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

namespace {

namespace layer {
constexpr unsigned kPlayer = 1u << 0;
constexpr unsigned kEnemy = 1u << 1;
constexpr unsigned kBullet = 1u << 2;
constexpr unsigned kWorld = 1u << 3;
} // namespace layer

class TopDownScene : public rmp::Scene {
public:
    void _ready() override {
        add_wall({ 400, 20 }, { 800, 40 });
        add_wall({ 400, 430 }, { 800, 40 });
        add_wall({ 20, 225 }, { 40, 450 });
        add_wall({ 780, 225 }, { 40, 450 });

        player_ = &spawn({ .position = { 200, 225 }, .shape = rmp::rect({ 26, 26 }) });
        player_->solid = true;
        player_->collision_layer = layer::kPlayer;
        player_->collision_mask = layer::kWorld | layer::kEnemy;
        // Eight directions, normalised, so the diagonal is not 41 % faster.
        player_->add<rmp::behavior::TopDown>({ .speed = 240 });
        player_->add<rmp::behavior::Health>({ .hp = 5, .invulnerable_for = 0.8f });

        for (int i = 0; i < 5; i++) add_enemy(560 + static_cast<float>(i) * 30);
    }

    void _update(float) override {
        if (rmp::input::just_pressed("ui_accept")) shoot();
    }

    void _draw() override {
        auto *health = player_->get<rmp::behavior::Health>();
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::text(TextFormat("HP %d   Enemies %d", health->hp, enemies_));
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

    void add_enemy(float x) {
        auto &enemy = spawn({ .position = { x, 225 }, .shape = rmp::circle(14) });
        enemy.solid = true;
        enemy.collision_layer = layer::kEnemy;
        // Walls and bullets, not other enemies: they walk through each other.
        enemy.collision_mask = layer::kWorld | layer::kBullet | layer::kPlayer;
        enemy.shape.color = MAROON;
        enemy.add<rmp::behavior::Follow>({ .target = player_->handle(), .speed = 90 });
        enemy.add<rmp::behavior::Health>({ .hp = 2, .invulnerable_for = 0.1f });
        enemies_++;
        enemy.on_collision([this](rmp::Object &self, rmp::Object &other) {
            if ((other.collision_layer & layer::kBullet) == 0) return;
            auto *health = self.get<rmp::behavior::Health>();
            if (health->damage(self)) {
                if (health->dead()) enemies_--;
            }
        });
    }

    void shoot() {
        // The direction the character is facing, which is what TopDown keeps.
        const Vector2 aim = player_->get<rmp::behavior::TopDown>()->direction();
        auto &shot = spawn({ .position = player_->position, .shape = rmp::circle(4) });
        shot.velocity = { aim.x * 520, aim.y * 520 };
        shot.collision_layer = layer::kBullet;
        shot.collision_mask = layer::kEnemy | layer::kWorld; // never the player
        shot.add<rmp::behavior::Projectile>();
        shot.add<rmp::behavior::Lifespan>({ .seconds = 1.5f });
    }

    rmp::Object *player_ = nullptr;
    int enemies_ = 0;
};

} // namespace

RMP_GAME(TopDownScene);
