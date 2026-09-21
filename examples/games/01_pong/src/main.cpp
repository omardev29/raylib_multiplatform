// ---------------------------------------------------------------------------
// examples/games/01_pong/src/main.cpp — Pong, whole.
//
// The first of the six judges. What is worth counting here is not the lines but
// WHAT they say: every one of them is a rule of Pong. There is no frame loop,
// no bounds check, no bounce arithmetic, no "has it left the screen", no
// AABB test. Those are not missing -- they are in the framework, which is the
// claim this file exists to make checkable.
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

class PongScene : public rmp::Scene {
public:
    void _ready() override {
        left_ = &spawn<Paddle>({ .position = { 40, 225 } });
        right_ = &spawn<Paddle>({ .position = { 760, 225 } });
        right_->up = "p2_up";
        right_->down = "p2_down";

        ball_ = &spawn({ .position = { 400, 225 }, .shape = rmp::circle(7) });
        // Bounce off the top and bottom, keep the pace, and leave the paddle at
        // an angle that depends on where it was hit. One line.
        ball_->add<rmp::behavior::Ball>({ .speed = kServeSpeed });
        serve(1);
    }

    void _update(float) override {
        // The only rule Pong has that the framework does not: a point.
        if (ball_->position.x < 0) {
            right_score_++;
            serve(-1);
        } else if (ball_->position.x > 800) {
            left_score_++;
            serve(1);
        }
    }

    void _draw() override {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_CENTER });
        rmp::ui::text(TextFormat("%d   %d", left_score_, right_score_));
        rmp::ui::end();
    }

private:
    // Which way the ball leaves is a RULE OF THE GAME, which is why no behavior
    // decides it. Here it goes towards whoever just conceded.
    void serve(float towards) {
        ball_->position = { 400, 225 };
        ball_->velocity = {};
        ball_->apply_force({ towards, 0.35f });
    }

    Paddle *left_ = nullptr;
    Paddle *right_ = nullptr;
    rmp::Object *ball_ = nullptr;
    int left_score_ = 0;
    int right_score_ = 0;
};

} // namespace

RMP_GAME(PongScene);
