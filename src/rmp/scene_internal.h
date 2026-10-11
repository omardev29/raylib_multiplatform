#pragma once
#include <memory>
// ---------------------------------------------------------------------------
// Private to src/rmp/. The scene stack as the app drives it.
//
// The public surface is include/rmp/scene.h. Everything here is the half the
// user never calls: run one frame, apply what the frame asked for, tear the
// stack down. tests/scene_test.cpp includes this directly, which is what lets
// the whole stack be tested without a window — see the note at the bottom.
// ---------------------------------------------------------------------------

#include <raylib.h> // Color, for clear_color()

#include <optional>
#include <string_view>

namespace rmp {
class Scene;
} // namespace rmp

namespace rmp::app {
enum class Stretch; // rmp/app.h; a scoped enum, so declaring it is enough here
} // namespace rmp::app

namespace rmp::scenes::detail {

// Put the first scene on the stack and run its _ready(). Called by
// rmp::app::detail::start(), which the RMP_GAME macro wires to the platform's
// ready hook.
void start(std::unique_ptr<Scene> first);

// The update pass: bottom of the stack upwards, stopping at the lowest scene
// that something above has frozen.
void update(float delta);

// The draw pass, bottom upwards, starting at the lowest scene that something
// above has not covered. The caller has already cleared the frame.
void draw();

// The colour the frame should be cleared with: the background of the lowest
// scene that draws, or the theme's when that scene left it BLANK.
Color clear_color();

// Apply everything change/push/replace/pop/quit recorded during the frame.
// Runs after EndDrawing(), so nothing is destroyed while it is on the stack of
// the call that asked for it.
void apply_pending();

// Unwind the stack, top down, calling _end() on each. Runs while the window is
// still open and before the resource table is released, because a scene can
// hold an rmp::Texture and its destructor has to find its slot still there.
void shutdown();

// The rectangle of the window the world -- the map and the objects -- is cut
// to while it is drawn, in whole pixels: the game's area under
// Stretch::LETTERBOX, so nothing of the world shows on the bars, which keep the
// clear colour. Nothing when the world may cover the whole window: NONE,
// EXPAND, and a letterbox that leaves no bar (a window of the design's own
// shape), so there the draw is exactly what it always was. `screen` is the
// window, in the pixels BeginScissorMode() takes.
struct WorldClip {
    int x;
    int y;
    int width;
    int height;
};
std::optional<WorldClip> world_clip(rmp::app::Stretch mode, Vector2 screen);

// RAY_TEST_WINDOW=WxH: the size the CI smoke test may ask RMP_GAME's window to
// open at instead of [window] width x height, so a [window] stretch can be
// booted on a screen of another shape. Two whole numbers joined by a lower-case
// x, each 16..16384 like [window] width and height; anything else is nothing.
// rmp::app::detail::start() asks for it only under the smoke test.
struct WindowSize {
    int width;
    int height;
};
std::optional<WindowSize> parse_test_window(std::string_view text);

// What RAY_TEST_WINDOW asks of RMP_GAME's window. `asked` is its value: ""
// when it is not set, or the smoke test is off. A size parse_test_window()
// reads is the window's, and the log says "RAY_TEST_WINDOW WxH". Anything
// else is refused with RAY_TEST_BOOT_FAIL and the reason, and the boot must
// then not be reported: a typo in a CI step would otherwise boot at the
// design size, where every [window] stretch is the same picture.
struct TestWindow {
    std::optional<WindowSize> size;
    bool refused;
};
TestWindow read_test_window(std::string_view asked);

// True once start() has run and until shutdown() finishes. The UI uses it to
// tell "inside an app" from "someone calling rmp::ui::begin() on their own".
bool running();

// --- the test seam ---------------------------------------------------------
// Everything above is arithmetic and callbacks. Nothing in this file touches
// the GPU, so tests/scene_test.cpp drives update/draw/apply_pending directly
// and compares the transcript of events. draw() is the only one that would,
// and it goes through the same test_mode() switch rmp::ui already has.

} // namespace rmp::scenes::detail
