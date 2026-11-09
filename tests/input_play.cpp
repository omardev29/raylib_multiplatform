// ===========================================================================
// rmp::input through the real runner, in both shapes a game is written in.
//
// A game written with RMP_ENTRY_POINT -- its own on_ready, on_frame and
// on_exit, as most of the examples are -- once read nothing at all through
// rmp::input: the devices were sampled inside rmp::app::detail::frame(), which
// only RMP_GAME calls. And sampling twice in one frame is no better than not
// at all: the second sample compares the key with itself, and every "just
// pressed" is gone. So this program is one runner, RMP_ENTRY_POINT, driven
// with fake keys through rmp::input's seam, and it asks both questions:
//
//   1. frames 0-12, the RMP_ENTRY_POINT shape: the hook reads rmp::input
//      itself and draws its own frame. Space held for four frames is pressed
//      on those four, and just pressed on exactly one.
//   2. frames 13-25, the RMP_GAME shape: the hook is rmp::app::detail::frame(),
//      and a scene's _update() reads it. The same press, seen exactly once.
//   3. frames 26-39, the window out of sight, through the window-state seam:
//      minimized (26-29), the scene moves and nothing is drawn; then, with
//      [window] pause_unfocused on from here (under it a minimized window is
//      a pause too), out of focus (30-33), the scene is told
//      _suspend() and does not move, and is drawn again as it stands; then
//      back (34-39): _resume(), and every frame moved and drawn again.
//   4. the scene is made as RMP_GAME makes it: after open_window(), so the
//      texture its members load has a window and comes out with a picture,
//      and nothing was loaded before the window.
//
// Built with the examples (CMakeLists.txt, target input_play) and run by
// tools/examples_build.sh: `rmp test examples` here, the examples job in CI.
// It prints INPUT PASS or INPUT FAIL and the reason.
// ===========================================================================

#include "../src/rmp/internal.h"

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/input.h>
#include <rmp/scene.h>

#include <raylib.h>

#include <cstdio>
#include <memory>

namespace {

constexpr int SECOND_SHAPE = 13;
constexpr int THIRD_SHAPE = 26;
constexpr int MINIMIZED_UNTIL = 30;
constexpr int UNFOCUSED_UNTIL = 34;
// 40: the coverage run plays it under RAY_TEST_MAX_FRAMES=40, as the smoke test.
constexpr int LAST_FRAME = 40;
constexpr int HELD_FROM = 5; // frames into each shape
constexpr int HELD_FOR = 4;

// The run, and what each shape saw. The dot at every use says "file state".
struct {
    int frame = 0;
    bool space = false;
    int hook_held = 0;
    int hook_pressed = 0;
    int scene_held = 0;
    int scene_pressed = 0;
    int updates = 0; // in the third shape
    int draws = 0;
    int suspends = 0;
    int resumes = 0;
    bool window_when_made = false;
    int failures = 0;
} run;

// What the window is, frame by frame, in the third shape.
void window(rmp::app::detail::WindowState &w) {
    w.minimized = run.frame >= THIRD_SHAPE && run.frame < MINIMIZED_UNTIL;
    w.focused = run.frame < MINIMIZED_UNTIL || run.frame >= UNFOCUSED_UNTIL;
}

void sample(rmp::input::detail::DeviceState &out) {
    out = rmp::input::detail::DeviceState{};
    out.keys[KEY_SPACE] = run.space;
}

class Counting : public rmp::Scene {
public:
    Counting() { run.window_when_made = IsWindowReady(); }
    // A member's load, the shape a game writes: it needs the window.
    rmp::Texture rabbit = rmp::assets::load_texture("rabbit.png");

    void _ready() override { rmp::input::action("jump", KEY_SPACE); }
    void _update(float /*delta*/) override {
        if (rmp::input::pressed("jump")) ++run.scene_held;
        if (rmp::input::just_pressed("jump")) ++run.scene_pressed;
        if (run.frame >= THIRD_SHAPE) ++run.updates;
    }
    void _draw() override {
        if (run.frame >= THIRD_SHAPE) ++run.draws;
    }
    void _suspend() override { ++run.suspends; }
    void _resume() override { ++run.resumes; }
};

void expect(const char *what, int got, int want) {
    if (got == want) {
        std::printf("INPUT  %s: %d\n", what, got);
        return;
    }
    std::printf("INPUT  %s: %d, expected %d\n", what, got, want);
    ++run.failures;
}

void ready() {
    rmp::input::detail::set_sample_provider(sample);
    rmp::app::detail::set_window_state_provider(window);
    // Opens the window, makes the scene and enters it, as RMP_GAME's ready
    // hook does. The first shape below simply never calls frame(), so the
    // scene sits there while the hook reads the input itself.
    rmp::app::detail::open_window();
    rmp::app::detail::start(std::make_unique<Counting>());
    // The scene on top is the Counting just made.
    const auto &scene = static_cast<const Counting &>(rmp::Scene::current());
    const Texture2D &rabbit = scene.rabbit;
    expect("the first scene made with a window", run.window_when_made ? 1 : 0, 1);
    expect("its member's texture has a picture", rabbit.id != 0 ? 1 : 0, 1);
    expect("loads before the window", rmp::assets::detail::loaded_before_window() ? 1 : 0,
           0);
}

void frame(float delta) {
    if (run.frame < SECOND_SHAPE) {
        if (rmp::input::pressed("jump")) ++run.hook_held;
        if (rmp::input::just_pressed("jump")) ++run.hook_pressed;
        BeginDrawing();
        ClearBackground(BLACK);
        EndDrawing();
    } else {
        rmp::app::detail::frame(delta);
    }

    // The keyboard changes BETWEEN frames, as raylib's does: its key state
    // moves only when EndDrawing() polls the events. Changed anywhere inside
    // the hook, a second sample in the same frame would see a different key
    // and the lost press this program exists to catch would look like a new
    // one. So the press lands on frames HELD_FROM .. HELD_FROM+HELD_FOR-1 of
    // each shape, decided at the end of the frame before.
    ++run.frame;
    if (run.frame == MINIMIZED_UNTIL) rmp::app::detail::set_pause_unfocused(true);
    const int next = run.frame < SECOND_SHAPE ? run.frame
        : run.frame < THIRD_SHAPE             ? run.frame - SECOND_SHAPE
                                              : 0;
    run.space = next >= HELD_FROM && next < HELD_FROM + HELD_FOR;
    if (run.frame < LAST_FRAME) return;
    expect("RMP_ENTRY_POINT, frames Space was held", run.hook_held, HELD_FOR);
    expect("RMP_ENTRY_POINT, presses", run.hook_pressed, 1);
    expect("RMP_GAME, frames Space was held", run.scene_held, HELD_FOR);
    expect("RMP_GAME, presses", run.scene_pressed, 1);
    // Minimized: 4 steps, none drawn. Unfocused: 4 drawn, none moved. Back: 6 of each.
    expect("out of sight, frames the scene moved", run.updates, 10);
    expect("out of sight, frames the scene was drawn", run.draws, 10);
    expect("out of sight, _suspend()", run.suspends, 1);
    expect("out of sight, _resume()", run.resumes, 1);
    if (run.failures == 0) {
        std::printf("INPUT PASS\n");
    } else {
        std::printf("INPUT FAIL (%d)\n", run.failures);
    }
    rmp::app::quit();
}

void stop() { rmp::app::detail::stop(); }

} // namespace

RMP_ENTRY_POINT(ready, frame, stop)
