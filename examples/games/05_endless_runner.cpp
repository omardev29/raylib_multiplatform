// ---------------------------------------------------------------------------
// examples/games/05_endless_runner.cpp — an endless runner.
//
// The fifth judge, and the one that asked for the most new abstractions, so it
// is the one worth reading for what is NOT here: no background loop, no modulo
// of a texture width, no distance counter, no obstacle recycling, and no "it is
// off screen now, delete it".
//
// What is left is: what art is in the background, how fast the player runs, how
// much it accelerates, and how often an obstacle appears. That is the game.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/behavior.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

namespace {

class RunnerScene : public rmp::Scene {
public:
    void _ready() override {
        // Three layers, each at its own speed, each repeating itself.
        add_layer("sky.png", 0.1f, 0);
        add_layer("hills.png", 0.4f, 120);
        add_layer("ground.png", 1.0f, 360);

        add_ground();

        player_ = &spawn({ .position = { 120, 300 }, .shape = rmp::rect({ 36, 54 }) });
        player_->add<rmp::behavior::Runner>({ .speed = 320, .accelerate = 6 });
        player_->add<rmp::behavior::Health>({ .hp = 1, .invulnerable_for = 0 });

        // ONE OBSTACLE EVERY 380 UNITS TRAVELLED, not every N seconds -- which
        // is the difference that matters once the speed is climbing. By time,
        // the obstacles spread further apart the faster you go and the game
        // gets easier, which is the opposite of the intention.
        spawn().add<rmp::behavior::Spawner>({
            .every_distance = 380,
            .jitter = 120,
            .track = player_->handle(),
            .on_spawn =
                [](rmp::Scene &scene, Vector2 at) {
                    auto &block = scene.spawn(
                        { .position = { at.x + 900, 330 }, .size = { 40, 60 } });
                    block.solid = true;
                    block.immovable = true;
                    block.edges = rmp::Edge::DESTROY; // it leaves on its own
                    block.shape.color = DARKBROWN;
                },
        });
    }

    void _draw() override {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_CENTER });
        rmp::ui::text(TextFormat(
            "%d m",
            static_cast<int>(player_->get<rmp::behavior::Runner>()->distance() / 10)));
        rmp::ui::end();
    }

private:
    void add_layer(const char *art, float factor, float y) {
        spawn({ .layer = -10 })
            .add<rmp::behavior::Parallax>({
                .texture = art,
                .factor = factor,
                .y = y,
            });
    }

    void add_ground() {
        auto &floor = spawn({ .position = { 400, 380 }, .size = { 4000, 40 } });
        floor.solid = true;
        floor.immovable = true;
    }

    rmp::Object *player_ = nullptr;
};

} // namespace

RMP_GAME(RunnerScene);
