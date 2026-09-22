// ---------------------------------------------------------------------------
// examples/games/02_breakout/src/main.cpp — Breakout.
//
// Pong plus a wall of bricks, and the wall is where the second judge earns its
// place: destroying a brick on contact is `on_collision` and one line, which is
// why `destroy_on_hit` is not in the catalogue. Question 2 of the three that
// decide whether a feature enters -- can it be composed from what exists? --
// answering itself.
//
// The rest of it is the three rules a Breakout has: three lives, a floor the
// ball can fall through, and a wall that is gone.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

namespace {

constexpr int kColumns = 10;
constexpr int kRows = 5;
constexpr float kBrickWidth = 72;
constexpr float kBrickHeight = 24;
constexpr int kLives = 3;
constexpr float kPaddleY = 420;

// One per row, top to bottom. A wall of one colour is a wall; five is a game.
constexpr Color kRowColors[kRows] = { MAROON, ORANGE, GOLD, LIME, SKYBLUE };

// The end of a game, PUSHED on top of it: the wall below freezes, stays on
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

class BreakoutScene : public rmp::Scene {
public:
    void _ready() override {
        background = Color{ 16, 18, 34, 255 };

        auto &paddle =
            spawn({ .position = { 400, kPaddleY }, .shape = rmp::rect({ 110, 16 }) });
        paddle.shape.color = rmp::ui::current_theme().primary;
        paddle.edges = rmp::Edge::CLAMP;
        paddle.solid = true;
        paddle.immovable = true;
        paddle_ = paddle.handle();

        // AN OPEN FLOOR, and it is one field: Ball sets Edge::BOUNCE, which
        // bounces off all four sides of its bounds, so a ball left with the
        // view's bounds would come back up off the bottom of the screen for
        // ever and a life could never be lost. Bounds taller than the court
        // keep the three walls and take the fourth away.
        auto &ball =
            spawn({ .position = { 400, 380 },
                    .shape = rmp::circle(7),
                    .bounds = { 0, 0, APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT + 300.0f } });
        ball.add<rmp::behavior::Ball>({ .speed = 320, .speed_up = 1.03f });
        ball_ = ball.handle();
        serve();

        for (int row = 0; row < kRows; row++) {
            for (int column = 0; column < kColumns; column++) {
                add_brick(row, column);
            }
        }
    }

    void _update(float) override {
        paddle_->velocity.x = rmp::input::axis("move_left", "move_right") * 520;

        // The two rules the framework has no opinion about: the ball is lost,
        // and the wall is gone.
        if (ball_->position.y > APP_WINDOW_HEIGHT + 20) {
            if (--lives_ <= 0) {
                rmp::Scene::push<OverScene<BreakoutScene>>("Game over");
                return;
            }
            serve();
        }
        if (bricks_ == 0) rmp::Scene::push<OverScene<BreakoutScene>>("You win");
    }

    void _draw() override {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::row({ .gap = 24 }, [&] {
            rmp::ui::text(TextFormat("Bricks %d", bricks_));
            rmp::ui::text(TextFormat("Lives %d", lives_),
                          { .color = rmp::ui::ColorRole::DANGER });
        });
        rmp::ui::end();
    }

private:
    void add_brick(int row, int column) {
        auto &brick = spawn({
            .position = { 44 + static_cast<float>(column) * (kBrickWidth + 4),
                          60 + static_cast<float>(row) * (kBrickHeight + 4) },
            .shape = rmp::rect({ kBrickWidth, kBrickHeight }),
        });
        brick.solid = true;
        brick.immovable = true;
        brick.shape.color = kRowColors[row];
        bricks_++;
        // The whole of "a brick breaks". No behavior, no subclass.
        brick.on_collision([this](rmp::Object &self, rmp::Object &other) {
            if (&other != ball_.get()) return;
            self.destroy();
            bricks_--;
        });
    }

    // Where the ball goes is the game's, not a behavior's: it leaves the paddle
    // upwards, and an impulse rather than a force because only the direction
    // survives -- Ball normalises whatever it is given to `speed`.
    void serve() {
        ball_->position = { paddle_->position.x, kPaddleY - 40 };
        ball_->velocity = {};
        ball_->apply_impulse({ 0.4f, -1 });
    }

    // Between frames, a handle. A raw pointer is good for the frame it was got
    // in and no longer -- see rmp/object.h.
    rmp::Handle<rmp::Object> paddle_;
    rmp::Handle<rmp::Object> ball_;
    int bricks_ = 0;
    int lives_ = kLives;
};

} // namespace

RMP_GAME(BreakoutScene);
