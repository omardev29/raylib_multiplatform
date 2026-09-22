// ---------------------------------------------------------------------------
// examples/games/05_endless_runner/src/main.cpp — an endless runner.
//
// The fifth judge, and the one that asked for the most new abstractions, so it
// is the one worth reading for what is NOT here: no background loop, no modulo
// of a texture width, no distance counter, no camera arithmetic, no obstacle
// recycling.
//
// What is left is: what art is in the background, how fast the player runs, how
// much it accelerates, how often a rock appears, and what a rock does to you.
// That is the game.
//
// The camera is what makes it a runner rather than a treadmill: it FOLLOWS the
// player, every parallax layer reads it, and the rocks stand still in the world
// while the view goes past them.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/behavior.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

namespace {

namespace layer {
constexpr unsigned kPlayer = 1u << 0;
constexpr unsigned kGround = 1u << 1;
constexpr unsigned kRock = 1u << 2;
} // namespace layer

constexpr float kGroundTop = 360; // where ground.png starts, and the floor with it

// The end of a run, PUSHED on top of it: the world below freezes, stays on
// screen -- distance counter and all -- and stops hearing the keyboard, so this
// scene writes no policy at all.
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

class RunnerScene : public rmp::Scene {
public:
    void _ready() override {
        // Three layers, each at its own speed, each repeating itself. They read
        // the camera, so nothing here has to tell them it moved.
        add_layer("sky.png", 0.1f, 0);
        add_layer("hills.png", 0.4f, 120);
        add_layer("ground.png", 1.0f, kGroundTop);

        auto &player = spawn({ .position = { 120, 300 } });
        player.sprite.texture = rmp::assets::load_texture("runner.png");
        // The picture is 36x54 and includes air; the box that collides is not.
        player.collider = rmp::rect({ 26, 48 });
        player.collision_layer = layer::kPlayer;
        player.collision_mask = layer::kGround | layer::kRock;
        player.add<rmp::behavior::Runner>({ .speed = 320, .accelerate = 6 });
        // One rock is the whole of it. `hurt_by` is what makes the contact
        // cost something -- without it this player ran through the scenery.
        player.add<rmp::behavior::Health>({
            .hp = 1,
            .invulnerable_for = 0,
            .destroy_on_death = false, // it stays on screen under the overlay
            .on_death =
                [](rmp::Object &) { rmp::Scene::push<OverScene<RunnerScene>>("Ouch"); },
            .hurt_by = layer::kRock,
        });
        player_ = player.handle();

        // THE CAMERA IS THE GAME'S ONE LINE, and `limits` with a zero width is
        // the second: follow the x, leave the y alone. A camera that followed
        // the jump would take the ground and the sky up with it.
        camera.follow = player_;
        camera.limits = { 0, 0, 0, APP_WINDOW_HEIGHT };

        add_ground();
        add_rocks();
    }

    void _update(float) override {
        // The floor travels with the runner, so the strip under the art is
        // always there. One line instead of a mile of collider.
        ground_->position.x = player_->position.x;
    }

    void _draw() override {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_CENTER });
        rmp::ui::text(
            TextFormat(
                "%d m",
                static_cast<int>(player_->get<rmp::behavior::Runner>()->distance() / 10)),
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
            spawn({ .position = { 400, kGroundTop + 60 }, .size = { 2400, 120 } });
        floor.visible = false; // ground.png is the picture; this is the floor
        floor.solid = true;
        floor.immovable = true;
        floor.collision_layer = layer::kGround;
        floor.collision_mask = 0;
        ground_ = floor.handle();
    }

    // ONE ROCK EVERY 520 UNITS TRAVELLED, not every N seconds -- which is the
    // difference that matters once the speed is climbing. By time, the rocks
    // spread further and further apart the faster you go and the game gets
    // easier, which is the opposite of the intention.
    void add_rocks() {
        spawn().add<rmp::behavior::Spawner>({
            .every_distance = 520,
            .jitter = 120,
            .max_alive = 8, // a live cap: it goes down again as they are dropped
            .track = player_,
            .on_spawn =
                [](rmp::Scene &scene, Vector2 at) {
                    auto &rock = scene.spawn({ .position = { at.x + 520, 330 } });
                    rock.sprite.texture = rmp::assets::load_texture("rock.png");
                    rock.collider = rmp::rect({ 30, 52 });
                    rock.collision_layer = layer::kRock;
                    rock.collision_mask = layer::kPlayer;
                    rock.edges = rmp::Edge::DESTROY; // once the view is past it
                },
        });
    }

    // Between frames, a handle. A raw pointer is good for the frame it was got
    // in and no longer -- see rmp/object.h.
    rmp::Handle<rmp::Object> player_;
    rmp::Handle<rmp::Object> ground_;
};

} // namespace

RMP_GAME(RunnerScene);
