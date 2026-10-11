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
    std::optional<std::uint64_t> seed; // what begin_run() seeds with
} timing;

// Behind a function, so it is built on first use: rmp::Callback is not a
// literal type, and a namespace-scope one would be initialised in an order
// nobody chose.
rmp::Callback<> &observer() {
    static rmp::Callback<> on_frame_end;
    return on_frame_end;
}

} // namespace

float step_delta() {
    const float raw = timing.fixed > 0 ? timing.fixed : GetFrameTime();
    // 0 switches the clamp off, and so does a negative reading, which no
    // platform should produce but a monotonic clock going backwards on a
    // suspend/resume is not unheard of -- and a negative step would run the
    // whole world in reverse for one frame.
    float step = raw;
    if (raw < 0) {
        step = 0;
    } else if (RMP_MAX_DELTA > 0 && raw > RMP_MAX_DELTA) {
        step = RMP_MAX_DELTA;
    }
    timing.last = step;
    return step;
}

float frame_delta() { return timing.last; }

void set_fixed_delta(float seconds) { timing.fixed = seconds > 0 ? seconds : 0; }

void set_test_seed(std::optional<std::uint64_t> seed) { timing.seed = seed; }

std::optional<std::uint64_t> test_seed() { return timing.seed; }

void set_frame_observer(rmp::Callback<> on_frame_end) {
    observer() = std::move(on_frame_end);
}

void frame_ended() { observer()(); }

} // namespace rmp::app::detail
