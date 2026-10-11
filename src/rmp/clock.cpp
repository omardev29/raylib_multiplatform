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

#include "internal.h"

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
} timing;

// How long the frame took: what raylib timed, or the step a test fixed.
float raw_delta() { return timing.fixed > 0 ? timing.fixed : GetFrameTime(); }

float clamped(const float raw) { return clamp_delta(raw, RMP_MAX_DELTA); }

// Behind a function, so it is built on first use: rmp::Callback is not a
// literal type, and a namespace-scope one would be initialised in an order
// nobody chose.
rmp::Callback<> &observer() {
    static rmp::Callback<> on_frame_end;
    return on_frame_end;
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
    timing.last = clamped(raw_delta());
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

} // namespace rmp::app::detail
