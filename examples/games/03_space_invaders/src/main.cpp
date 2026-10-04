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
constexpr unsigned PLAYER = 1u << 0;
constexpr unsigned ALIEN = 1u << 1;
constexpr unsigned PLAYER_SHOT = 1u << 2;
constexpr unsigned ALIEN_SHOT = 1u << 3;
} // namespace layer

constexpr int COLUMNS = 11;
constexpr int ROWS = 4;
constexpr int ALIENS = COLUMNS * ROWS;
constexpr float MARCH = 40; // how far the block slides each way
constexpr float DROP = 16; // and how far down it steps when it turns
constexpr float PLAYER_Y = 410;
constexpr float GROUND_Y = 434;

// One per row, and the row a picture would put the strange ones in is the one
// at the top.
constexpr Color ROW_COLORS[ROWS] = { VIOLET, PINK, ORANGE, GOLD };

// The end of a game, PUSHED on top of it: the board below freezes, stays on
// screen and stops hearing the keyboard, so this scene writes no policy at all
// -- the pause overlay of examples/scenes/01_stack with another label.
template <class Game> class OverScene : public rmp::Scene {
public:
    explicit OverScene(const char *said) : _said(said) {}

    void _draw() override {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 190 });
        rmp::ui::begin();
        rmp::ui::text(_said, { .size = rmp::ui::Size::LARGE });
        if (rmp::ui::button("Play again")) rmp::Scene::change<Game>();
        rmp::ui::end();
    }

private:
    const char *_said;
};

class InvadersScene : public rmp::Scene {
public:
    void _ready() override {
        background = Color{ 8, 10, 20, 255 };

        auto &player =
            spawn({ .position = { 400, PLAYER_Y }, .shape = rmp::rect({ 44, 18 }) });
        player.shape.color = LIME;
        player.edges = rmp::Edge::CLAMP;
        player.collision_layer = layer::PLAYER;
        player.collision_mask = layer::ALIEN_SHOT | layer::ALIEN;
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
            .hurt_by = layer::ALIEN_SHOT | layer::ALIEN,
        });
        _player = player.handle();

        for (int i = 0; i < ALIENS; i++) add_alien(i);
    }

    void _update(float delta) override {
        _player->velocity.x = rmp::input::axis("move_left", "move_right") * 360;
        if (rmp::input::just_pressed("ui_accept")) shoot();

        // The formation. Every alien moves as one, turns at the wall, drops a
        // step, and the whole block speeds up as it thins -- which is the game.
        _march += _step * delta * static_cast<float>(ALIENS) /
            static_cast<float>(_alive > 0 ? _alive : 1);
        if (_march > MARCH || _march < -MARCH) {
            _step = -_step;
            _march = _march > 0 ? MARCH : -MARCH;
            _drop += DROP;
        }

        float lowest = 0;
        for (int i = 0; i < ALIENS; i++) {
            rmp::Object *alien = _aliens[i].get();
            if (alien == nullptr) continue; // a handle answers this on its own
            alien->position = { home(i).x + _march, home(i).y + _drop };
            lowest = alien->position.y > lowest ? alien->position.y : lowest;
        }

        if (_alive == 0) {
            rmp::Scene::push<OverScene<InvadersScene>>("You win");
        } else if (lowest > PLAYER_Y - 30) {
            rmp::Scene::push<OverScene<InvadersScene>>("They landed");
        }
    }

    void _draw() override {
        // The ground, IN WORLD UNITS: _draw() is handed the screen and the
        // ships are drawn through the camera, so on a window that is not the
        // design size a line drawn in screen units lands somewhere else.
        BeginMode2D(camera.raylib());
        DrawRectangle(0, static_cast<int>(GROUND_Y), RMP_WINDOW_WIDTH, 3, DARKGREEN);
        EndMode2D();

        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::row({ .gap = 24 }, [&] {
            rmp::ui::text(TextFormat("Aliens %d", _alive));
            rmp::ui::text(TextFormat("HP %d", _player->get<rmp::behavior::Health>()->hp),
                          { .color = rmp::ui::ColorRole::DANGER });
        });
        rmp::ui::end();
    }

private:
    // Where an alien belongs when the block has not moved. The formation is
    // this plus one offset, which is why no alien has to remember anything.
    static Vector2 home(int index) {
        const int column = index % COLUMNS;
        const int row = index / COLUMNS;
        return { 180 + static_cast<float>(column) * 44,
                 70 + static_cast<float>(row) * 38 };
    }

    void add_alien(int index) {
        auto &alien = spawn({ .position = home(index), .shape = rmp::rect({ 32, 22 }) });
        alien.shape.color = ROW_COLORS[index / COLUMNS];
        alien.collision_layer = layer::ALIEN;
        alien.collision_mask = layer::PLAYER_SHOT;
        alien.on_collision([this](rmp::Object &self, rmp::Object &) {
            self.destroy();
            _alive--;
        });
        // Shooting is a Timer plus a callback, which is why `shooter` is not in
        // the catalogue: the same two pieces make an enemy spawner and a blink.
        // A period of its own per alien, from rmp::random -- forty-four timers
        // started on the same frame with the same number fire as one gun.
        alien.add<rmp::behavior::Timer>({
            .seconds = rmp::random::range(4.0f, 9.0f),
            .on_timeout = [this, index](rmp::Object &self) { alien_shoot(self, index); },
        });
        _aliens[index] = alien.handle();
        _alive++;
    }

    void shoot() {
        auto &shot =
            spawn({ .position = _player->position, .shape = rmp::rect({ 4, 14 }) });
        shot.shape.color = LIME;
        shot.velocity = { 0, -560 };
        shot.collision_layer = layer::PLAYER_SHOT;
        shot.collision_mask = layer::ALIEN;
        shot.add<rmp::behavior::Projectile>();
    }

    // Only the lowest alien of a column fires. The ones above it would shoot
    // their own row in the back, and one `if` against a handle says so.
    void alien_shoot(rmp::Object &from, int index) {
        if (index + COLUMNS < ALIENS && _aliens[index + COLUMNS]) return;
        auto &shot = spawn({ .position = from.position, .shape = rmp::rect({ 4, 14 }) });
        shot.shape.color = RED;
        shot.velocity = { 0, 300 };
        shot.collision_layer = layer::ALIEN_SHOT;
        shot.collision_mask = layer::PLAYER;
        shot.add<rmp::behavior::Projectile>();
    }

    // Between frames, a handle. A raw pointer is good for the frame it was got
    // in and no longer -- and an alien is the thing most likely to die between
    // two of them.
    rmp::Handle<rmp::Object> _player;
    rmp::Handle<rmp::Object> _aliens[ALIENS];
    int _alive = 0;
    float _march = 0;
    float _step = 40;
    float _drop = 0;
};

} // namespace

RMP_GAME(InvadersScene);
