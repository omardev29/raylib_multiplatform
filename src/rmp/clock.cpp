// ===========================================================================
// The clock -- see "clock.cpp" in src/rmp/internal.h.
//
// The one GetFrameTime() in the framework. step_delta() turns it into what
// the game is handed, and remembers it, so that rmp::ui animates by the same
// number the scenes moved by. It used to be two clocks: the game's here and
// the UI's own GetFrameTime() in ui/context.cpp, so under a fixed step the
// game advanced by the script while the menus faded and repeat-scrolled by
// whatever the machine took.
//
// In the library and not in app.cpp, which is compiled into each executable
// with its entry point: a test binary has no entry point, and it is the one
// that has to be able to fix the step and read it back.
// ===========================================================================

#include "audio_internal.h"
#include "internal.h"
#include "scene_internal.h"

#include <rmp/app.h>
#include <rmp/config.h>
#include <rmp/object.h> // rmp::Callback

#include <raylib.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>

namespace rmp::app::detail {

namespace {

// Constant-initialised, every field: a test's static initializer can set them
// before main(), whatever order the linker put the files in, because there is
// no dynamic initialisation here for it to race.
struct {
    float fixed = 0; // > 0: every frame is exactly this long
    float last = 0; // what step_delta() returned last
    bool stepped = false; // a runner has called step_delta(): its number is the frame's
    std::optional<std::uint64_t> seed; // what begin_run() seeds with
    // The clock of its own, for frames raylib did not time: a frame raylib
    // times starts in one EndDrawing() and ends in the next, so the frames
    // that skipped it -- and the first one drawn after them, which raylib
    // measures from the last EndDrawing() before the skip -- are timed from
    // `mark` instead. `drawn` counts the frames drawn in a row, up to 2.
    double mark = 0; // GetTime() when the last step was taken; 0 = never
    int drawn = 2;
    bool held = false; // paused: every step is 0
    bool zero_once = false; // the first step after a pause is 0 too
} timing;

// The window's pace. Constant-initialised like the clock.
struct {
    bool pause_unfocused = RMP_WINDOW_PAUSE_UNFOCUSED != 0;
    bool paused = false; // the frames are paused: the edge has been handled
    Pace now{};
} pacing;

// How long the frame took: what raylib timed, or the step a test fixed.
float raw_delta() {
    if (timing.fixed > 0) return timing.fixed;
    if (timing.drawn < 2 && timing.mark > 0) return static_cast<float>(GetTime() - timing.mark);
    return GetFrameTime();
}

float clamped(const float raw) { return clamp_delta(raw, RMP_MAX_DELTA); }

// Behind a function, so it is built on first use: rmp::Callback is not a
// literal type, and a namespace-scope one would be initialised in an order
// nobody chose.
rmp::Callback<> &observer() {
    static rmp::Callback<> on_frame_end;
    return on_frame_end;
}

rmp::Callback<WindowState &> &window_provider() {
    static rmp::Callback<WindowState &> provider;
    return provider;
}

} // namespace

// Every comparison is written so that a NaN fails it: `raw >= 0` is false for
// NaN, where `raw < 0` would have let it through -- and a NaN step makes every
// position it touches NaN for the rest of the run.
float clamp_delta(const float raw, const float max) {
    // A ceiling that is not a positive number of seconds is no ceiling.
    const bool ceiling = max > 0 && std::isfinite(max);
    if (ceiling && raw > max) return max; // +inf included
    // A negative reading becomes 0: no platform should produce one, but a
    // monotonic clock going backwards on a suspend/resume is not unheard of --
    // and a negative step would run the whole world in reverse for one frame.
    // NaN is no time at all, and so is +inf with nothing to cut it to.
    if (!(raw >= 0) || !std::isfinite(raw)) return 0.0f;
    return raw;
}

float step_delta() {
    const float raw = raw_delta();
    timing.mark = GetTime();
    const bool still = timing.held || timing.zero_once;
    timing.zero_once = false;
    timing.last = still ? 0.0f : clamped(raw);
    timing.stepped = true;
    return timing.last;
}

// With no runner -- rmp::ui in a plain raylib loop of the game's own --
// nothing calls step_delta(), and what it last returned would be 0 for
// good: no menu would fade, no notice go away, no tooltip show.
float frame_delta() { return timing.stepped ? timing.last : clamped(raw_delta()); }

void forget_steps_for_tests() {
    timing.stepped = false;
    timing.last = 0;
}

// Not a positive number of seconds -- 0, negative, NaN, infinite -- is the
// real clock again.
void set_fixed_delta(float seconds) {
    timing.fixed = seconds > 0 && std::isfinite(seconds) ? seconds : 0;
}

void set_test_seed(std::optional<std::uint64_t> seed) { timing.seed = seed; }

std::optional<std::uint64_t> test_seed() { return timing.seed; }

void set_frame_observer(rmp::Callback<> on_frame_end) {
    observer() = std::move(on_frame_end);
}

void frame_ended() { observer()(); }

// ---------------------------------------------------------------------------
// The window's pace -- see internal.h.
// ---------------------------------------------------------------------------

void set_window_state_provider(rmp::Callback<WindowState &> provider) {
    window_provider() = std::move(provider);
}

WindowState window_state() {
    WindowState w;
    if (window_provider()) {
        window_provider()(w);
        return w;
    }
    w.minimized = IsWindowMinimized();
    w.focused = IsWindowFocused();
    return w;
}

void set_pause_unfocused(bool pause) { pacing.pause_unfocused = pause; }

bool pause_unfocused() { return pacing.pause_unfocused; }

void begin_pace() {
    const WindowState w = window_state();
    // A minimized window has no focus either: under pause_unfocused it is a
    // pause too, and nothing is drawn.
    const bool pause = pacing.pause_unfocused && (!w.focused || w.minimized);
    if (pause && !pacing.paused) {
        pacing.paused = true;
        timing.held = true;
        rmp::scenes::detail::suspend_top();
        rmp::audio::detail::pause_all();
    } else if (!pause && pacing.paused) {
        pacing.paused = false;
        timing.held = false;
        timing.zero_once = true;
        rmp::audio::detail::resume_all();
        rmp::scenes::detail::resume_top();
    }
    pacing.now = Pace{ .update = !pause, .draw = !w.minimized };
}

Pace pace() { return pacing.now; }

void frame_skipped(const double seconds) {
    timing.drawn = 0;
    // Under a fixed step (the e2e driver, a test) nothing waits: the frames
    // are as long as the step says, however long the machine took.
    if (timing.fixed <= 0 && seconds > 0) WaitTime(seconds);
}

void frame_drawn() {
    if (timing.drawn < 2) ++timing.drawn;
}

void reset_pace_for_tests() {
    pacing.pause_unfocused = RMP_WINDOW_PAUSE_UNFOCUSED != 0;
    pacing.paused = false;
    pacing.now = Pace{};
    timing.held = false;
    timing.zero_once = false;
    timing.drawn = 2;
    timing.mark = 0;
    window_provider() = {};
}

} // namespace rmp::app::detail
