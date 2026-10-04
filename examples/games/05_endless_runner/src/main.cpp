// ---------------------------------------------------------------------------
// examples/games/05_endless_runner -- an endless runner.
//
// Worth reading for what is NOT here: no background loop, no modulo of a
// texture width, no distance counter, no camera arithmetic, no obstacle
// recycling.
//
// What is left is: what art is in the background, how fast the player runs, how
// much it accelerates, how often a rock appears, and what a rock does to you.
// That is the game.
//
// The camera is what makes it a runner rather than a treadmill: it FOLLOWS the
// player, every parallax layer reads it, and the rocks stand still in the world
// while the view goes past them.
//
// Run it: `just example 05_endless_runner`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/behavior.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

namespace {

namespace layer {
constexpr unsigned PLAYER = 1u << 0;
constexpr unsigned GROUND = 1u << 1;
constexpr unsigned ROCK = 1u << 2;
} // namespace layer

constexpr float GROUND_TOP = 360; // where ground.png starts, and the floor with it

// The end of a run, PUSHED on top of it: the world below freezes, stays on
// screen -- distance and all -- and stops hearing the keyboard, which is what
// the scene stack does on its own. So this is only what it says and the way
// out.
class OverScene : public rmp::Scene {
public:
    explicit OverScene(const char *said) : _said(said) {}
    void _draw() override; // below RunnerScene, which it starts again

private:
    const char *_said;
};

class RunnerScene : public rmp::Scene {
public:
    void _ready() override {
        // Three layers, each at its own speed, each repeating itself. They read
        // the camera, so nothing here has to tell them it moved.
        add_layer("sky.png", 0.1f, 0);
        add_layer("hills.png", 0.4f, 120);
        add_layer("ground.png", 1.0f, GROUND_TOP);

        auto &player = spawn({ .position = { 120, 300 } });
        player.sprite.texture = rmp::assets::load_texture("runner.png");
        // The picture is 36x54 and includes air; the box that collides is not.
        player.collider = rmp::rect({ 26, 48 });
        player.collision_layer = layer::PLAYER;
        player.collision_mask = layer::GROUND | layer::ROCK;
        player.add<rmp::behavior::Runner>({ .speed = 320, .accelerate = 6 });
        // One rock and it is over. `hurt_by` is what makes touching a rock
        // cost something.
        player.add<rmp::behavior::Health>({
            .hp = 1,
            .invulnerable_for = 0,
            .destroy_on_death = false, // it stays on screen under the overlay
            .on_death = [](rmp::Object &) { rmp::Scene::push<OverScene>("Ouch"); },
            .hurt_by = layer::ROCK,
        });
        _player = player.handle();

        // The camera follows the runner, and `limits` keeps it level: a zero
        // width leaves x free, and a height of exactly the view pins y. A camera
        // that followed the jump would take the ground and the sky up with it.
        camera.follow = _player;
        camera.limits = { 0, 0, 0, RMP_WINDOW_HEIGHT };
        // And a little give: the camera catches up at a rate, so the runner
        // leads the view by a few pixels at speed instead of being nailed to
        // its centre. A rate per second, the same at 30 and at 144 Hz.
        camera.smoothing = 12;

        add_ground();
        add_rocks();
    }

    void _update(float) override {
        // The floor travels with the runner, so the strip under the art is
        // always there. One line instead of a mile of collider.
        _ground->position.x = _player->position.x;
    }

    void _draw() override {
        // Ten world units to the metre.
        const float distance = _player->get<rmp::behavior::Runner>()->distance();
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_CENTER });
        rmp::ui::text(TextFormat("%d m", static_cast<int>(distance / 10)),
                      { .size = 40 });
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
        auto &floor =
            spawn({ .position = { 400, GROUND_TOP + 60 }, .size = { 2400, 120 } });
        floor.visible = false; // ground.png is the picture; this is the floor
        floor.solid = true;
        floor.immovable = true;
        floor.collision_layer = layer::GROUND;
        floor.collision_mask = 0;
        _ground = floor.handle();
    }

    // One rock every 520 units travelled, not every N seconds: by time, the
    // rocks would spread further apart the faster you go, and the game would
    // get easier as it speeds up.
    void add_rocks() {
        spawn().add<rmp::behavior::Spawner>({
            .every_distance = 520,
            .jitter = 120,
            .max_alive = 8, // a live cap: it goes down again as they are dropped
            .track = _player,
            .on_spawn =
                [](rmp::Scene &scene, Vector2 at) {
                    auto &rock = scene.spawn({ .position = { at.x + 520, 330 } });
                    rock.sprite.texture = rmp::assets::load_texture("rock.png");
                    rock.collider = rmp::rect({ 30, 52 });
                    rock.collision_layer = layer::ROCK;
                    rock.collision_mask = layer::PLAYER;
                    rock.edges = rmp::Edge::DESTROY; // once the view is past it
                },
        });
    }

    // Between frames, a handle. A raw pointer is good for the frame it was got
    // in and no longer -- see rmp/object.h.
    rmp::Handle<rmp::Object> _player;
    rmp::Handle<rmp::Object> _ground;
};

void OverScene::_draw() {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 190 });
    rmp::ui::begin();
    rmp::ui::text(_said, { .size = rmp::ui::Size::LARGE });
    if (rmp::ui::button("Play again")) rmp::Scene::change<RunnerScene>();
    rmp::ui::end();
}

} // namespace

RMP_GAME(RunnerScene);
