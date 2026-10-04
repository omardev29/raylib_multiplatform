// ---------------------------------------------------------------------------
// examples/games/01_pong/src/main.cpp — Pong, whole.
//
// The first of the six judges. What is worth counting here is not the lines but
// WHAT they say: every one of them is a rule of Pong. There is no frame loop,
// no bounds check, no bounce arithmetic, no "has it left the screen", no
// AABB test. Those are not missing -- they are in the framework, which is the
// claim this file exists to make checkable.
//
// It is a whole game: two players, a point when the ball leaves by a side,
// first to seven, and a way to play again. With sound, by name: the three
// files in resources/ are hit.wav, point.wav and win.wav, and nothing in this
// file opens, feeds or closes a sound device. On a machine without one it
// plays the same game in silence. (The CI runner that boots it has a silent
// null device; tests/configure_test.py checks the three files exist, since
// thirty frames never reach a paddle to play them.)
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/audio.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

namespace {

constexpr float PADDLE_SPEED = 420;
constexpr float SERVE_SPEED = 340;
constexpr int WINNING_SCORE = 7;
constexpr float MID_X = RMP_WINDOW_WIDTH / 2.0f;
constexpr float MID_Y = RMP_WINDOW_HEIGHT / 2.0f;

// A paddle is a rectangle that goes up and down and stops at the edge. `edges`
// is the stopping, and it is one field.
class Paddle : public rmp::Object {
public:
    const char *up = "move_up";
    const char *down = "move_down";

    void _ready() override {
        shape = rmp::rect({ 14, 90 });
        edges = rmp::Edge::CLAMP;
        solid = true;
        immovable = true;
    }

    void _update(float) override {
        velocity.y = rmp::input::axis(up, down) * PADDLE_SPEED;
    }
};

// The end of a game, PUSHED on top of it: the court below freezes, stays on
// screen and stops hearing the keyboard, so this scene writes no policy at all
// -- exactly the pause overlay of examples/scenes/01_stack, with a different
// label on the button.
template <class Game> class OverScene : public rmp::Scene {
public:
    explicit OverScene(const char *said) : _said(said) {}

    // So that Enter, Space or the gamepad restart without a mouse: nothing has
    // the focus until something is given it.
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

class PongScene : public rmp::Scene {
public:
    void _ready() override {
        background = Color{ 14, 30, 28, 255 };

        // Player two's keys. The six actions that come as standard are player
        // one's, and a second player is the game's to name -- reading an action
        // nobody defined is silent, which is how these two went missing.
        rmp::input::action("p2_up", KEY_UP);
        rmp::input::action("p2_down", KEY_DOWN);

        const rmp::ui::Theme &theme = rmp::ui::current_theme();
        auto &left = spawn<Paddle>({ .position = { 40, MID_Y } });
        left.shape.color = theme.primary;
        _left = left.handle<Paddle>();

        auto &right = spawn<Paddle>({ .position = { RMP_WINDOW_WIDTH - 40, MID_Y } });
        right.up = "p2_up";
        right.down = "p2_down";
        right.shape.color = theme.danger;
        _right = right.handle<Paddle>();

        // BOUNDS WIDER THAN THE COURT, and this is the rule the game turns on:
        // Ball sets Edge::BOUNCE, which bounces off all four sides of its
        // bounds -- so a ball left with the view's bounds can never go out and
        // no point can ever be scored. Tall as the court and wide past it, and
        // it bounces off the top and the bottom and leaves by the sides.
        auto &ball = spawn(
            { .position = { MID_X, MID_Y },
              .shape = rmp::circle(7),
              .bounds = { -240, 0, RMP_WINDOW_WIDTH + 480.0f, RMP_WINDOW_HEIGHT } });
        ball.add<rmp::behavior::Ball>({ .speed = SERVE_SPEED });
        // Ball does the bouncing; the sound of it is the game's.
        ball.on_collision([](rmp::Object &, rmp::Object &) { rmp::audio::play("hit"); });
        _ball = ball.handle();
        serve(1);
    }

    void _update(float) override {
        // The only rule Pong has that the framework does not: a point.
        if (_ball->position.x < 0) {
            point(_right_score, "Red wins", -1);
        } else if (_ball->position.x > RMP_WINDOW_WIDTH) {
            point(_left_score, "Blue wins", 1);
        }
    }

    void _draw() override {
        // The net, over the court and under the score. IN WORLD UNITS, because
        // _draw() is handed the screen and the paddles are drawn through the
        // camera: on a window that is not the design size the two disagree, and
        // the net ends up somewhere the court is not.
        BeginMode2D(camera.raylib());
        for (int y = 8; y < RMP_WINDOW_HEIGHT; y += 30) {
            DrawRectangle(static_cast<int>(MID_X) - 2, y, 4, 16,
                          Color{ 255, 255, 255, 38 });
        }
        EndMode2D();

        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_CENTER });
        rmp::ui::row({ .gap = 110 }, [&] {
            rmp::ui::text(TextFormat("%d", _left_score),
                          { .color = rmp::ui::ColorRole::PRIMARY, .size = 56 });
            rmp::ui::text(TextFormat("%d", _right_score),
                          { .color = rmp::ui::ColorRole::DANGER, .size = 56 });
        });
        rmp::ui::end();
    }

private:
    // Which way the ball leaves is a RULE OF THE GAME, which is why no behavior
    // decides it. Here it goes towards whoever just conceded, and an impulse
    // and not a force: a force is spread over the frame it is applied in, and
    // only the direction of this one matters -- Ball normalises it to `speed`.
    void serve(float towards) {
        _ball->position = { MID_X, MID_Y };
        _ball->velocity = {};
        _ball->apply_impulse({ towards, 0.35f });
        _left->position.y = MID_Y; // a new rally starts level
        _right->position.y = MID_Y;
    }

    void point(int &counter, const char *winner, float towards) {
        counter++;
        serve(towards);
        if (counter >= WINNING_SCORE) {
            rmp::audio::play("win");
            rmp::Scene::push<OverScene<PongScene>>(winner);
        } else {
            rmp::audio::play("point");
        }
    }

    // Between frames, a handle. A raw pointer to an object is good for the
    // frame it was got in and no longer -- see rmp/object.h.
    rmp::Handle<Paddle> _left;
    rmp::Handle<Paddle> _right;
    rmp::Handle<rmp::Object> _ball;
    int _left_score = 0;
    int _right_score = 0;
};

} // namespace

RMP_GAME(PongScene);
