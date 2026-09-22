// ---------------------------------------------------------------------------
// examples/games/03_space_invaders/src/main.cpp — Space Invaders.
//
// The third judge, and the one that makes the case for what is NOT in the
// catalogue. The formation -- a block of aliens stepping sideways, dropping and
// speeding up as they thin out -- is *the game of Space Invaders*. A behavior
// with fields for columns, step and descent would be this file with a different
// name on it, so the formation is written here and the framework supplies the
// parts that are not the game: shooting on a Timer, bullets that discard
// themselves, hit points, and who is allowed to hurt whom.
//
// The formation is a HANDLE PER ALIEN and one number. Nothing walks a list of
// objects looking for aliens, and nothing holds a pointer to one that a shot
// may have destroyed two frames ago.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/random.h>
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
constexpr int kAliens = kColumns * kRows;
constexpr float kMarch = 40; // how far the block slides each way
constexpr float kDrop = 16; // and how far down it steps when it turns
constexpr float kPlayerY = 410;
constexpr float kGroundY = 434;

// One per row, and the row a picture would put the strange ones in is the one
// at the top.
constexpr Color kRowColors[kRows] = { VIOLET, PINK, ORANGE, GOLD };

// The end of a game, PUSHED on top of it: the board below freezes, stays on
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

class InvadersScene : public rmp::Scene {
public:
    void _ready() override {
        background = Color{ 8, 10, 20, 255 };

        auto &player =
            spawn({ .position = { 400, kPlayerY }, .shape = rmp::rect({ 44, 18 }) });
        player.shape.color = LIME;
        player.edges = rmp::Edge::CLAMP;
        player.collision_layer = layer::kPlayer;
        player.collision_mask = layer::kAlienShot | layer::kAlien;
        // WHO IS ALLOWED TO HURT IT, and it is one field. Without `hurt_by` a
        // Health is a hit-point counter that nothing ever reduces, which is
        // how this player used to be immortal.
        player.add<rmp::behavior::Health>({
            .hp = 3,
            .invulnerable_for = 1.2f,
            .destroy_on_death = false, // the ship stays on screen under the overlay
            .on_death =
                [](rmp::Object &) {
                    rmp::Scene::push<OverScene<InvadersScene>>("Game over");
                },
            .hurt_by = layer::kAlienShot | layer::kAlien,
        });
        player_ = player.handle();

        for (int i = 0; i < kAliens; i++) add_alien(i);
    }

    void _update(float delta) override {
        player_->velocity.x = rmp::input::axis("move_left", "move_right") * 360;
        if (rmp::input::just_pressed("ui_accept")) shoot();

        // The formation. Every alien moves as one, turns at the wall, drops a
        // step, and the whole block speeds up as it thins -- which is the game.
        march_ += step_ * delta * static_cast<float>(kAliens) /
            static_cast<float>(alive_ > 0 ? alive_ : 1);
        if (march_ > kMarch || march_ < -kMarch) {
            step_ = -step_;
            march_ = march_ > 0 ? kMarch : -kMarch;
            drop_ += kDrop;
        }

        float lowest = 0;
        for (int i = 0; i < kAliens; i++) {
            rmp::Object *alien = aliens_[i].get();
            if (alien == nullptr) continue; // a handle answers this on its own
            alien->position = { home(i).x + march_, home(i).y + drop_ };
            lowest = alien->position.y > lowest ? alien->position.y : lowest;
        }

        if (alive_ == 0) {
            rmp::Scene::push<OverScene<InvadersScene>>("You win");
        } else if (lowest > kPlayerY - 30) {
            rmp::Scene::push<OverScene<InvadersScene>>("They landed");
        }
    }

    void _draw() override {
        // The ground, IN WORLD UNITS: _draw() is handed the screen and the
        // ships are drawn through the camera, so on a window that is not the
        // design size a line drawn in screen units lands somewhere else.
        BeginMode2D(camera.raylib());
        DrawRectangle(0, static_cast<int>(kGroundY), APP_WINDOW_WIDTH, 3, DARKGREEN);
        EndMode2D();

        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::row({ .gap = 24 }, [&] {
            rmp::ui::text(TextFormat("Aliens %d", alive_));
            rmp::ui::text(TextFormat("HP %d", player_->get<rmp::behavior::Health>()->hp),
                          { .color = rmp::ui::ColorRole::DANGER });
        });
        rmp::ui::end();
    }

private:
    // Where an alien belongs when the block has not moved. The formation is
    // this plus one offset, which is why no alien has to remember anything.
    static Vector2 home(int index) {
        return { 180 + static_cast<float>(index % kColumns) * 44,
                 70 + static_cast<float>(index / kColumns) * 38 };
    }

    void add_alien(int index) {
        auto &alien = spawn({ .position = home(index), .shape = rmp::rect({ 32, 22 }) });
        alien.shape.color = kRowColors[index / kColumns];
        alien.collision_layer = layer::kAlien;
        alien.collision_mask = layer::kPlayerShot;
        alien.on_collision([this](rmp::Object &self, rmp::Object &) {
            self.destroy();
            alive_--;
        });
        // Shooting is a Timer plus a callback, which is why `shooter` is not in
        // the catalogue: the same two pieces make an enemy spawner and a blink.
        // A period of its own per alien, from rmp::random -- forty-four timers
        // started on the same frame with the same number fire as one gun.
        alien.add<rmp::behavior::Timer>({
            .seconds = rmp::random::range(4.0f, 9.0f),
            .on_timeout = [this, index](rmp::Object &self) { alien_shoot(self, index); },
        });
        aliens_[index] = alien.handle();
        alive_++;
    }

    void shoot() {
        auto &shot =
            spawn({ .position = player_->position, .shape = rmp::rect({ 4, 14 }) });
        shot.shape.color = LIME;
        shot.velocity = { 0, -560 };
        shot.collision_layer = layer::kPlayerShot;
        shot.collision_mask = layer::kAlien;
        shot.add<rmp::behavior::Projectile>();
    }

    // Only the lowest alien of a column fires. The ones above it would shoot
    // their own row in the back, and one `if` against a handle says so.
    void alien_shoot(rmp::Object &from, int index) {
        if (index + kColumns < kAliens && aliens_[index + kColumns]) return;
        auto &shot = spawn({ .position = from.position, .shape = rmp::rect({ 4, 14 }) });
        shot.shape.color = RED;
        shot.velocity = { 0, 300 };
        shot.collision_layer = layer::kAlienShot;
        shot.collision_mask = layer::kPlayer;
        shot.add<rmp::behavior::Projectile>();
    }

    // Between frames, a handle. A raw pointer is good for the frame it was got
    // in and no longer -- and an alien is the thing most likely to die between
    // two of them.
    rmp::Handle<rmp::Object> player_;
    rmp::Handle<rmp::Object> aliens_[kAliens];
    int alive_ = 0;
    float march_ = 0;
    float step_ = 40;
    float drop_ = 0;
};

} // namespace

RMP_GAME(InvadersScene);
