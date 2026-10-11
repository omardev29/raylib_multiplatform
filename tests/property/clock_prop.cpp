// ---------------------------------------------------------------------------
// The framework's one clock (src/rmp/clock.cpp), held to the campaign's rules
// for any reading a machine or a test can hand it:
//
//   - what a frame hands the game is a finite number of seconds, 0 or more,
//     and never more than [app] max_delta when there is one -- a NaN or an
//     infinity from the reading would be every position in the world the
//     next frame;
//   - a reading the clamp has nothing to say about goes through as it is,
//     and one past max_delta is max_delta;
//   - a fixed step is what every frame hands the game and what the UI reads
//     back, whatever number the test fixed;
//   - the test seed and the frame observer are kept as they were given.
//
// Nothing here is public API: rmp::app::detail is the seam the runners and
// tests/e2e/driver.cpp use.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/internal.h"

#include <rmp/app.h>
#include <rmp/config.h>
#include <rmp/object.h> // rmp::Callback

#include <doctest.h>
#include <rapidcheck.h>

#include <cmath>
#include <cstdint>
#include <optional>

namespace {

// Back to the real clock, no seed, no observer, whatever a case did.
void reset_clock() {
    rmp::app::detail::set_fixed_delta(0);
    rmp::app::detail::set_test_seed(std::nullopt);
    rmp::app::detail::set_frame_observer({});
    rmp::app::detail::forget_steps_for_tests();
}

// What [app] max_delta allows a frame to be: no ceiling when it is 0.
bool within_ceiling(float seconds, float max) {
    return !(max > 0) || !std::isfinite(max) || seconds <= max;
}

} // namespace

TEST_SUITE("property: clock") {
    TEST_CASE("a frame is a finite number of seconds, 0 or more, whatever the reading") {
        rmp::test::check(
            "clamp_delta is a finite number of seconds, 0 or more, at most max", [] {
                const float raw = *rmp::test::gen::hostile_float();
                const float max = *rmp::test::gen::hostile_float();
                const float out = rmp::app::detail::clamp_delta(raw, max);
                RC_ASSERT(std::isfinite(out));
                RC_ASSERT(out >= 0.0f);
                RC_ASSERT(within_ceiling(out, max));
            });
    }

    TEST_CASE("a reading the clamp has nothing to say about goes through as it is") {
        rmp::test::check(
            "clamp_delta keeps a reading inside the clamp, and cuts one past it", [] {
                const float raw = *rmp::test::gen::hostile_float();
                const float max = *rmp::test::gen::hostile_float();
                RC_PRE(std::isfinite(raw) && raw >= 0.0f);
                const float out = rmp::app::detail::clamp_delta(raw, max);
                const bool clamps = max > 0 && std::isfinite(max) && raw > max;
                RC_ASSERT(out == (clamps ? max : raw));
            });
    }

    TEST_CASE("a fixed step is what every frame hands the game, clamped") {
        rmp::test::check(
            "step_delta and frame_delta hand over the fixed step, clamped, every frame",
            [] {
                reset_clock();
                const float seconds = *rmp::test::gen::hostile_float();
                const int frames = *rc::gen::inRange(1, 5);
                rmp::app::detail::set_fixed_delta(seconds);
                for (int i = 0; i < frames; ++i) {
                    const float step = rmp::app::detail::step_delta();
                    RC_ASSERT(std::isfinite(step));
                    RC_ASSERT(step >= 0.0f);
                    RC_ASSERT(within_ceiling(step, RMP_MAX_DELTA));
                    RC_ASSERT(rmp::app::detail::frame_delta() == step);
                    // A fixed step that is a number of seconds is that number,
                    // clamped; anything else is the real clock, which no
                    // window here has ticked.
                    if (std::isfinite(seconds) && seconds > 0) {
                        RC_ASSERT(step ==
                                  rmp::app::detail::clamp_delta(seconds, RMP_MAX_DELTA));
                    }
                }
                reset_clock();
            });
    }

    TEST_CASE("the test seed and the frame observer are kept as given") {
        rmp::test::check(
            "set_test_seed reads back, and the observer hears every frame_ended", [] {
                reset_clock();
                std::optional<std::uint64_t> seed;
                if (*rc::gen::arbitrary<bool>()) {
                    seed = *rc::gen::arbitrary<std::uint64_t>();
                }
                rmp::app::detail::set_test_seed(seed);
                RC_ASSERT(rmp::app::detail::test_seed() == seed);

                static int heard = 0;
                heard = 0;
                const int frames = *rc::gen::inRange(0, 8);
                rmp::app::detail::set_frame_observer([] { ++heard; });
                for (int i = 0; i < frames; ++i) rmp::app::detail::frame_ended();
                RC_ASSERT(heard == frames);
                rmp::app::detail::set_frame_observer({});
                rmp::app::detail::frame_ended();
                RC_ASSERT(heard == frames);
                reset_clock();
            });
    }
}
