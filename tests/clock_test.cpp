// ---------------------------------------------------------------------------
// The clock seam (src/rmp/clock.cpp): one clock for the game and its menus,
// and a test's way to fix it.
//
// The UI read GetFrameTime() itself, so under a fixed step the game advanced
// by the script while the menus faded and repeat-scrolled by wall time, and no
// e2e run could draw the same frame twice. These say the fixed step reaches
// what the game is handed, what frame_delta() reports, and what the UI
// animates by -- and that nothing changes for a run that fixes nothing.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/internal.h"
#include "../src/rmp/ui/internal.h"

#include <rmp/app.h>
#include <rmp/config.h>
#include <rmp/object.h> // rmp::Callback

#include <cstdint>
#include <optional>

namespace {

// Back to the real clock, no seed, no observer, whatever a case did.
struct Fixture {
    Fixture() { reset(); }
    ~Fixture() { reset(); }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;

    static void reset() {
        rmp::app::detail::set_fixed_delta(0);
        rmp::app::detail::set_test_seed(std::nullopt);
        rmp::app::detail::set_frame_observer({});
        rmp::ui::detail::set_test_viewport(0, 0);
        (void)rmp::app::detail::step_delta();
    }
};

constexpr float STEP = 1.0f / 60;

} // namespace

TEST_SUITE("app: the clock") {
    TEST_CASE_FIXTURE(
        Fixture, "a fixed step is what the game is handed and what frame_delta says") {
        rmp::app::detail::set_fixed_delta(STEP);
        CHECK(rmp::app::detail::step_delta() == STEP);
        CHECK(rmp::app::detail::frame_delta() == STEP);
        // Every frame, not the first one only.
        CHECK(rmp::app::detail::step_delta() == STEP);
        CHECK(rmp::app::detail::frame_delta() == STEP);
    }

    TEST_CASE_FIXTURE(Fixture, "the UI animates by the same number") {
        rmp::app::detail::set_fixed_delta(STEP);
        (void)rmp::app::detail::step_delta();
        CHECK(rmp::ui::detail::frame_time() == STEP);
        rmp::app::detail::set_fixed_delta(1.0f / 240);
        (void)rmp::app::detail::step_delta();
        CHECK(rmp::ui::detail::frame_time() == 1.0f / 240);
    }

    TEST_CASE_FIXTURE(Fixture, "a headless layout test still sees no time at all") {
        rmp::app::detail::set_fixed_delta(STEP);
        (void)rmp::app::detail::step_delta();
        rmp::ui::detail::set_test_viewport(800, 450);
        CHECK(rmp::ui::detail::frame_time() == 0.0f);
    }

    TEST_CASE_FIXTURE(Fixture, "a fixed step is still clamped by [app] max_delta") {
        rmp::app::detail::set_fixed_delta(1.0f);
        const float step = rmp::app::detail::step_delta();
        if (RMP_MAX_DELTA > 0) {
            CHECK(step == doctest::Approx(RMP_MAX_DELTA));
        } else {
            CHECK(step == 1.0f);
        }
        CHECK(rmp::app::detail::frame_delta() == step);
    }

    TEST_CASE_FIXTURE(Fixture, "0, or less, is the real clock -- no window, no time") {
        rmp::app::detail::set_fixed_delta(STEP);
        (void)rmp::app::detail::step_delta();
        rmp::app::detail::set_fixed_delta(0);
        CHECK(rmp::app::detail::step_delta() == 0.0f);
        CHECK(rmp::app::detail::frame_delta() == 0.0f);
        rmp::app::detail::set_fixed_delta(-1);
        CHECK(rmp::app::detail::step_delta() == 0.0f);
    }

    TEST_CASE_FIXTURE(Fixture, "the test seed is kept until it is taken back") {
        CHECK_FALSE(rmp::app::detail::test_seed().has_value());
        rmp::app::detail::set_test_seed(std::uint64_t{ 1 });
        CHECK(rmp::app::detail::test_seed() == std::optional<std::uint64_t>(1));
        rmp::app::detail::set_test_seed(std::nullopt);
        CHECK_FALSE(rmp::app::detail::test_seed().has_value());
    }

    TEST_CASE_FIXTURE(Fixture,
                      "the frame observer is told once a frame, and can be taken away") {
        int told = 0;
        rmp::app::detail::set_frame_observer([&told] { ++told; });
        rmp::app::detail::frame_ended();
        rmp::app::detail::frame_ended();
        CHECK(told == 2);
        rmp::app::detail::set_frame_observer({});
        rmp::app::detail::frame_ended();
        CHECK(told == 2);
    }
}
