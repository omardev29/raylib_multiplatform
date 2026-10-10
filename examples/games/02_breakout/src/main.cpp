// ---------------------------------------------------------------------------
// examples/games/02_breakout -- Breakout.
//
// Pong plus a wall of bricks. A brick breaking on contact is `on_collision`
// and one line, so there is no behavior for it. The rest is the three rules a
// Breakout has: three lives, a floor the ball can fall through, and a wall
// that is gone.
//
// Run it: `rmp example 02_breakout`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <array>

namespace {

constexpr int COLUMNS = 10;
constexpr int ROWS = 5;
constexpr float BRICK_WIDTH = 72;
constexpr float BRICK_HEIGHT = 24;
constexpr int LIVES = 3;
constexpr float PADDLE_Y = 420;
constexpr float PADDLE_SPEED = 520;

// One per row, top to bottom. A wall of one colour is a wall; five is a game.
constexpr std::array<Color, ROWS> ROW_COLORS{ MAROON, ORANGE, GOLD, LIME, SKYBLUE };

// The end of a game, PUSHED on top of it: the wall below freezes, stays on
// screen and stops hearing the keyboard, which is what the scene stack does on
// its own -- so this is only what it says and the way out.
class OverScene : public rmp::Scene {
public:
    explicit OverScene(const char *said) : _said(said) {}
    void _draw() override; // below BreakoutScene, which it starts again

private:
    const char *_said;
};

class BreakoutScene : public rmp::Scene {
public:
    void _ready() override {
        background = Color{ 16, 18, 34, 255 };

        auto &paddle =
            spawn({ .position = { 400, PADDLE_Y }, .shape = rmp::rect({ 110, 16 }) });
        paddle.shape.color = rmp::ui::current_theme().primary;
        paddle.edges = rmp::Edge::CLAMP;
        paddle.solid = true;
        paddle.immovable = true;
        _paddle = paddle.handle();

        // AN OPEN FLOOR, and it is one field: Ball sets Edge::BOUNCE, which
        // bounces off all four sides of its bounds, so a ball left with the
        // view's bounds would come back up off the bottom of the screen for
        // ever and a life could never be lost. Bounds taller than the court
        // keep the three walls and take the fourth away.
        auto &ball =
            spawn({ .position = { 400, 380 },
                    .shape = rmp::circle(7),
                    .bounds = { 0, 0, RMP_WINDOW_WIDTH, RMP_WINDOW_HEIGHT + 300.0f } });
        ball.add<rmp::behavior::Ball>({ .speed = 320, .speed_up = 1.03f });
        _ball = ball.handle();
        serve();

        for (int row = 0; row < ROWS; ++row) {
            for (int column = 0; column < COLUMNS; ++column) {
                add_brick(row, column);
            }
        }
    }

    void _update(float) override {
        _paddle->velocity.x = rmp::input::axis("move_left", "move_right") * PADDLE_SPEED;

        // The two rules the framework has no opinion about: the ball is lost,
        // and the wall is gone.
        if (_ball->position.y > RMP_WINDOW_HEIGHT + 20) {
            --_lives;
            if (_lives <= 0) {
                rmp::Scene::push<OverScene>("Game over");
                return;
            }
            // A life lost, felt: the view jolts and settles in a third of a
            // second. It never moves the paddle, the ball or the bricks --
            // gameplay does not see it, only what is drawn does.
            camera.shake(8, 0.35f);
            serve();
        }
        if (_bricks == 0) rmp::Scene::push<OverScene>("You win");
    }

    void _draw() override {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::row({ .gap = 24 }, [&] {
            rmp::ui::text(TextFormat("Bricks %d", _bricks));
            rmp::ui::text(TextFormat("Lives %d", _lives),
                          { .color = rmp::ui::ColorRole::DANGER });
        });
        rmp::ui::end();
    }

private:
    void add_brick(int row, int column) {
        auto &brick = spawn({
            .position = { 44 + static_cast<float>(column) * (BRICK_WIDTH + 4),
                          60 + static_cast<float>(row) * (BRICK_HEIGHT + 4) },
            .shape = rmp::rect({ BRICK_WIDTH, BRICK_HEIGHT }),
        });
        brick.solid = true;
        brick.immovable = true;
        brick.shape.color = ROW_COLORS[row];
        ++_bricks;
        // The whole of "a brick breaks". No behavior, no subclass.
        brick.on_collision([this](rmp::Object &self, rmp::Object &other) {
            if (other.handle() != _ball) return;
            self.destroy();
            --_bricks;
        });
    }

    // Where the ball goes is the game's to say: up off the paddle. An impulse,
    // because only its direction matters -- Ball keeps its own speed.
    void serve() {
        _ball->position = { _paddle->position.x, PADDLE_Y - 40 };
        _ball->velocity = {};
        _ball->apply_impulse({ 0.4f, -1 });
    }

    // Between frames, a handle. A raw pointer is good for the frame it was got
    // in and no longer -- see rmp/object.h.
    rmp::Handle<rmp::Object> _paddle;
    rmp::Handle<rmp::Object> _ball;
    int _bricks = 0;
    int _lives = LIVES;
};

void OverScene::_draw() {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 190 });
    rmp::ui::begin();
    rmp::ui::text(_said, { .size = rmp::ui::Size::LARGE });
    if (rmp::ui::button("Play again")) rmp::Scene::change<BreakoutScene>();
    rmp::ui::end();
}

} // namespace

RMP_GAME(BreakoutScene);
