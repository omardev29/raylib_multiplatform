// ---------------------------------------------------------------------------
// examples/games/02_breakout/src/main.cpp — Breakout.
//
// Pong plus a wall of bricks, and the wall is where the second judge earns its
// place: destroying a brick on contact is `on_collision` and one line, which is
// why `destroy_on_hit` is not in the catalogue. Question 2 of the three that
// decide whether a feature enters -- can it be composed from what exists? --
// answering itself.
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

class BreakoutScene : public rmp::Scene {
public:
    void _ready() override {
        paddle_ = &spawn({ .position = { 400, 420 }, .shape = rmp::rect({ 110, 16 }) });
        paddle_->edges = rmp::Edge::CLAMP;
        paddle_->solid = true;
        paddle_->immovable = true;

        ball_ = &spawn({ .position = { 400, 380 }, .shape = rmp::circle(7) });
        ball_->add<rmp::behavior::Ball>({ .speed = 320, .speed_up = 1.03f });
        ball_->apply_force({ 0.4f, -1 }); // the serve is the game's

        for (int row = 0; row < kRows; row++) {
            for (int column = 0; column < kColumns; column++) {
                add_brick(row, column);
            }
        }
    }

    void _update(float) override {
        paddle_->velocity.x = rmp::input::axis("move_left", "move_right") * 520;
        // Losing the ball. The other rule the framework has no opinion about.
        if (ball_->position.y > 460) rmp::Scene::change<BreakoutScene>();
    }

    void _draw() override {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::text(TextFormat("Bricks: %d", bricks_));
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
        brick.shape.color = row % 2 == 0 ? SKYBLUE : ORANGE;
        bricks_++;
        // The whole of "a brick breaks". No behavior, no subclass.
        brick.on_collision([this](rmp::Object &self, rmp::Object &other) {
            if (&other != ball_) return;
            self.destroy();
            bricks_--;
        });
    }

    rmp::Object *paddle_ = nullptr;
    rmp::Object *ball_ = nullptr;
    int bricks_ = 0;
};

} // namespace

RMP_GAME(BreakoutScene);
