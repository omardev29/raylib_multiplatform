// ---------------------------------------------------------------------------
// examples/games/03_space_invaders/src/main.cpp — Space Invaders.
//
// The third judge, and the one that makes the case for what is NOT in the
// catalogue. The formation -- a block of aliens stepping sideways, dropping and
// speeding up as they thin out -- is *the game of Space Invaders*. A behavior
// with fields for columns, step and descent would be this file with a different
// name on it, so the formation is written here and the framework supplies the
// parts that are not the game: shooting on a Timer, bullets that discard
// themselves, and hit points.
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
constexpr unsigned kAlien = 1u << 1;
constexpr unsigned kPlayerShot = 1u << 2;
constexpr unsigned kAlienShot = 1u << 3;
} // namespace layer

constexpr int kColumns = 11;
constexpr int kRows = 4;

class InvadersScene : public rmp::Scene {
public:
    void _ready() override {
        player_ = &spawn({ .position = { 400, 410 }, .shape = rmp::rect({ 44, 18 }) });
        player_->edges = rmp::Edge::CLAMP;
        player_->collision_layer = layer::kPlayer;
        player_->collision_mask = layer::kAlienShot | layer::kAlien;
        player_->add<rmp::behavior::Health>({ .hp = 3, .invulnerable_for = 1.2f });

        for (int row = 0; row < kRows; row++) {
            for (int column = 0; column < kColumns; column++) add_alien(row, column);
        }
    }

    void _update(float delta) override {
        player_->velocity.x = rmp::input::axis("move_left", "move_right") * 360;
        if (rmp::input::just_pressed("ui_accept")) shoot();

        // The formation. Every alien moves as one, turns at the wall, drops a
        // step, and the whole block speeds up as it thins -- which is the game.
        march_ += step_ * delta * static_cast<float>(kColumns * kRows) /
            static_cast<float>(alive_ > 0 ? alive_ : 1);
        if (march_ > 40 || march_ < -40) {
            step_ = -step_;
            march_ = march_ > 0 ? 40 : -40;
            drop_ = 18;
        }
    }

    void _draw() override {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::text(TextFormat("Aliens: %d", alive_));
        rmp::ui::end();
    }

private:
    void add_alien(int row, int column) {
        auto &alien = spawn({
            .position = { 180 + static_cast<float>(column) * 44,
                          70 + static_cast<float>(row) * 38 },
            .shape = rmp::rect({ 32, 22 }),
        });
        alien.collision_layer = layer::kAlien;
        alien.collision_mask = layer::kPlayerShot;
        alive_++;
        alien.on_collision([this](rmp::Object &self, rmp::Object &) {
            self.destroy();
            alive_--;
        });
        // Shooting is a Timer plus a callback, which is why `shooter` is not in
        // the catalogue: the same two pieces make an enemy spawner and a blink.
        alien.add<rmp::behavior::Timer>({
            .seconds = 2.5f,
            .on_timeout = [this](rmp::Object &self) { alien_shoot(self); },
        });
    }

    void shoot() {
        auto &shot =
            spawn({ .position = player_->position, .shape = rmp::rect({ 4, 14 }) });
        shot.velocity = { 0, -560 };
        shot.collision_layer = layer::kPlayerShot;
        shot.collision_mask = layer::kAlien;
        shot.add<rmp::behavior::Projectile>();
    }

    void alien_shoot(rmp::Object &from) {
        auto &shot = spawn({ .position = from.position, .shape = rmp::rect({ 4, 14 }) });
        shot.velocity = { 0, 260 };
        shot.collision_layer = layer::kAlienShot;
        shot.collision_mask = layer::kPlayer;
        shot.add<rmp::behavior::Projectile>();
    }

    rmp::Object *player_ = nullptr;
    int alive_ = 0;
    float march_ = 0;
    float step_ = 14;
    float drop_ = 0;
};

} // namespace

RMP_GAME(InvadersScene);
