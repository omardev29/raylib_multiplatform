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
//   1. frames 0-19, the RMP_ENTRY_POINT shape: the hook reads rmp::input
//      itself and draws its own frame. Space held for four frames is pressed
//      on those four, and just pressed on exactly one.
//   2. frames 20-39, the RMP_GAME shape: the hook is rmp::app::detail::frame(),
//      and a scene's _update() reads it. The same press, seen exactly once.
//
// Built with the examples (CMakeLists.txt, target input_play) and run by
// tools/examples_build.sh: `rmp test examples` here, the examples job in CI.
// It prints INPUT PASS or INPUT FAIL and the reason.
// ===========================================================================

#include <rmp/app.h>
#include <rmp/input.h>
#include <rmp/scene.h>

#include <raylib.h>

#include <cstdio>
#include <memory>

namespace {

constexpr int SECOND_SHAPE = 20;
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
    int failures = 0;
} run;

void sample(rmp::input::detail::DeviceState *out) {
    *out = rmp::input::detail::DeviceState{};
    out->keys[KEY_SPACE] = run.space;
}

class Counting : public rmp::Scene {
public:
    void _ready() override { rmp::input::action("jump", KEY_SPACE); }
    void _update(float /*delta*/) override {
        if (rmp::input::pressed("jump")) run.scene_held++;
        if (rmp::input::just_pressed("jump")) run.scene_pressed++;
    }
};

void expect(const char *what, int got, int want) {
    if (got == want) {
        std::printf("INPUT  %s: %d\n", what, got);
        return;
    }
    std::printf("INPUT  %s: %d, expected %d\n", what, got, want);
    run.failures++;
}

void ready() {
    rmp::input::detail::set_sample_provider(sample);
    // Opens the window and enters the scene, as RMP_GAME's ready hook does.
    // The first shape below simply never calls frame(), so the scene sits
    // there while the hook reads the input itself.
    rmp::app::detail::start(std::make_unique<Counting>());
}

void frame(float delta) {
    if (run.frame < SECOND_SHAPE) {
        if (rmp::input::pressed("jump")) run.hook_held++;
        if (rmp::input::just_pressed("jump")) run.hook_pressed++;
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
    const int next = run.frame < SECOND_SHAPE ? run.frame : run.frame - SECOND_SHAPE;
    run.space = next >= HELD_FROM && next < HELD_FROM + HELD_FOR;
    if (run.frame < LAST_FRAME) return;
    expect("RMP_ENTRY_POINT, frames Space was held", run.hook_held, HELD_FOR);
    expect("RMP_ENTRY_POINT, presses", run.hook_pressed, 1);
    expect("RMP_GAME, frames Space was held", run.scene_held, HELD_FOR);
    expect("RMP_GAME, presses", run.scene_pressed, 1);
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
