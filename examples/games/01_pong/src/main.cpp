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
// first to seven, and a way to play again.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

namespace {

constexpr float kPaddleSpeed = 420;
constexpr float kServeSpeed = 340;
constexpr int kWinningScore = 7;
constexpr float kMidX = APP_WINDOW_WIDTH / 2.0f;
constexpr float kMidY = APP_WINDOW_HEIGHT / 2.0f;

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
        velocity.y = rmp::input::axis(up, down) * kPaddleSpeed;
    }
};

// The end of a game, PUSHED on top of it: the court below freezes, stays on
// screen and stops hearing the keyboard, so this scene writes no policy at all
// -- exactly the pause overlay of examples/scenes/01_stack, with a different
// label on the button.
template <class Game> class OverScene : public rmp::Scene {
public:
    explicit OverScene(const char *said) : said_(said) {}

    // So that Enter, Space or the gamepad restart without a mouse: nothing has
    // the focus until something is given it.
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
        auto &left = spawn<Paddle>({ .position = { 40, kMidY } });
        left.shape.color = theme.primary;
        left_ = left.handle<Paddle>();

        auto &right = spawn<Paddle>({ .position = { APP_WINDOW_WIDTH - 40, kMidY } });
        right.up = "p2_up";
        right.down = "p2_down";
        right.shape.color = theme.danger;
        right_ = right.handle<Paddle>();

        // BOUNDS WIDER THAN THE COURT, and this is the rule the game turns on:
        // Ball sets Edge::BOUNCE, which bounces off all four sides of its
        // bounds -- so a ball left with the view's bounds can never go out and
        // no point can ever be scored. Tall as the court and wide past it, and
        // it bounces off the top and the bottom and leaves by the sides.
        auto &ball = spawn(
            { .position = { kMidX, kMidY },
              .shape = rmp::circle(7),
              .bounds = { -240, 0, APP_WINDOW_WIDTH + 480.0f, APP_WINDOW_HEIGHT } });
        ball.add<rmp::behavior::Ball>({ .speed = kServeSpeed });
        ball_ = ball.handle();
        serve(1);
    }

    void _update(float) override {
        // The only rule Pong has that the framework does not: a point.
        if (ball_->position.x < 0) {
            point(right_score_, "Red wins", -1);
        } else if (ball_->position.x > APP_WINDOW_WIDTH) {
            point(left_score_, "Blue wins", 1);
        }
    }

    void _draw() override {
        // The net, over the court and under the score. IN WORLD UNITS, because
        // _draw() is handed the screen and the paddles are drawn through the
        // camera: on a window that is not the design size the two disagree, and
        // the net ends up somewhere the court is not.
        BeginMode2D(camera.raylib());
        for (int y = 8; y < APP_WINDOW_HEIGHT; y += 30) {
            DrawRectangle(static_cast<int>(kMidX) - 2, y, 4, 16,
                          Color{ 255, 255, 255, 38 });
        }
        EndMode2D();

        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_CENTER });
        rmp::ui::row({ .gap = 110 }, [&] {
            rmp::ui::text(TextFormat("%d", left_score_),
                          { .color = rmp::ui::ColorRole::PRIMARY, .size = 56 });
            rmp::ui::text(TextFormat("%d", right_score_),
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
        ball_->position = { kMidX, kMidY };
        ball_->velocity = {};
        ball_->apply_impulse({ towards, 0.35f });
        left_->position.y = kMidY; // a new rally starts level
        right_->position.y = kMidY;
    }

    void point(int &counter, const char *winner, float towards) {
        counter++;
        serve(towards);
        if (counter >= kWinningScore) rmp::Scene::push<OverScene<PongScene>>(winner);
    }

    // Between frames, a handle. A raw pointer to an object is good for the
    // frame it was got in and no longer -- see rmp/object.h.
    rmp::Handle<Paddle> left_;
    rmp::Handle<Paddle> right_;
    rmp::Handle<rmp::Object> ball_;
    int left_score_ = 0;
    int right_score_ = 0;
};

} // namespace

RMP_GAME(PongScene);
