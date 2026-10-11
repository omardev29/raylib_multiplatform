// ===========================================================================
// tests/ui_render_test.cpp — rmp::ui on a real GPU.
//
//   cmake --preset debug -DBUILD_UI_TESTS=ON
//   cmake --build --preset debug --target ui_render_test
//   ./build/debug/ui_render_test        (CI: xvfb-run -a, Mesa's llvmpipe)
//
// Everything else the interface does is checked with no window at all:
// tests/ui_layout_test.cpp and the unit tests lay it out headless, and the
// examples job draws it with raylib's software renderer. What overlay()'s
// backdrop draws cannot be checked either way -- the software renderer has
// no shaders, and the blur is a shader -- so this opens a hidden window on a
// real OpenGL, draws, and reads the pixels back.
//
// It prints one line per check and PASS or FAILED, and its exit status is the
// verdict: `rmp test ui-render`, and a step of the CI lint job.
// ===========================================================================

#include "../src/rmp/ui/backdrop.h"
#include "../src/rmp/ui/internal.h"

#include <raylib.h>
#include <rlgl.h>

#include <cstdio>
#include <string_view>

namespace {

constexpr int WIDTH = 320;
constexpr int HEIGHT = 180;

struct {
    int failures = 0;
} results;

void check(bool ok, std::string_view what) {
    std::printf("%s  %.*s\n", ok ? "ok  " : "FAIL", static_cast<int>(what.size()),
                what.data());
    if (!ok) ++results.failures;
}

bool same(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// The frame as it is now, read back before EndDrawing() swaps it away -- with
// raylib's batch drawn first, or the last of it is missing (CLAUDE.md).
Image read_back() {
    rlDrawRenderBatchActive();
    return LoadImageFromScreen();
}

// The colour at a fraction of the picture's width and height: the window may
// be drawn at a scale of the screen's, so no pixel is named by its number.
Color colour_at(const Image &image, float fx, float fy) {
    return GetImageColor(image,
                         static_cast<int>(fx * static_cast<float>(image.width - 1)),
                         static_cast<int>(fy * static_cast<float>(image.height - 1)));
}

// Black on the left, white on the right: the frame a blur is measured on.
void draw_split() {
    ClearBackground(BLACK);
    DrawRectangle(WIDTH / 2, 0, WIDTH - WIDTH / 2, HEIGHT, WHITE);
}

// Through the backdrop's hooks, as rmp::app::detail::frame() calls them, with
// no overlay anywhere: the frame is exactly what was drawn.
void untouched_without_an_overlay() {
    BeginDrawing();
    rmp::ui::detail::begin_backdrop();
    draw_split();
    rmp::ui::detail::finish_backdrop();
    const Image frame = read_back();
    EndDrawing();
    check(!rmp::ui::detail::backdrop_redirected(),
          "no overlay asked for a blur, so the frame was drawn straight to the screen");
    check(frame.width > 0 && frame.height > 0, "the frame reads back");
    if (frame.width > 0 && frame.height > 0) {
        check(same(colour_at(frame, 0.05f, 0.5f), BLACK) &&
                  same(colour_at(frame, 0.45f, 0.5f), BLACK),
              "the left half is black, to its edge");
        check(same(colour_at(frame, 0.55f, 0.5f), WHITE) &&
                  same(colour_at(frame, 0.95f, 0.5f), WHITE),
              "the right half is white, from its edge");
    }
    UnloadImage(frame);
}

} // namespace

int main() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(WIDTH, HEIGHT, "ui_render_test");
    check(IsWindowReady(), "a hidden window opened");
    if (!IsWindowReady()) {
        std::printf("\nFAILED: no window -- under CI this runs inside xvfb-run\n");
        return 1;
    }

    // A GL that can build a shader. 1.1 and 2.1 cannot run the blur, and the
    // software renderer is not a GPU: either one here is the wrong machine.
    const int gl = rlGetVersion();
    std::printf("      rlGetVersion() = %d\n", gl);
    check(gl == RL_OPENGL_33 || gl == RL_OPENGL_43 || gl == RL_OPENGL_ES_20 ||
              gl == RL_OPENGL_ES_30,
          "the context is OpenGL 3.3, 4.3, ES 2.0 or ES 3.0");

    untouched_without_an_overlay();

    rmp::ui::shutdown();
    CloseWindow();

    std::printf("\n%s\n", results.failures == 0 ? "PASS" : "FAILED");
    return results.failures == 0 ? 0 : 1;
}
