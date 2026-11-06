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

#include "../src/rmp/audio_internal.h"
#include "../src/rmp/internal.h"
#include "../src/rmp/scene_internal.h"
#include "../src/rmp/ui/internal.h"

#include <rmp/app.h>
#include <rmp/config.h>
#include <rmp/object.h> // rmp::Callback
#include <rmp/scene.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <limits>
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
        rmp::ui::detail::set_test_frame_time(0);
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

    TEST_CASE_FIXTURE(Fixture,
                      "a headless test can let time go by, and only a headless one") {
        rmp::ui::detail::set_test_viewport(800, 450);
        rmp::ui::detail::set_test_frame_time(0.25f);
        CHECK(rmp::ui::detail::frame_time() == 0.25f);
        // Nothing below zero: time does not run backwards.
        rmp::ui::detail::set_test_frame_time(-1);
        CHECK(rmp::ui::detail::frame_time() == 0.0f);
        // Outside test mode it is the game's clock, whatever a test set.
        rmp::ui::detail::set_test_frame_time(0.25f);
        rmp::ui::detail::set_test_viewport(0, 0);
        rmp::app::detail::set_fixed_delta(STEP);
        (void)rmp::app::detail::step_delta();
        CHECK(rmp::ui::detail::frame_time() == STEP);
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

    TEST_CASE("a reading that is no number of seconds is no time at all") {
        // tests/property/app_prop.cpp's counterexamples: NaN under a ceiling
        // of 1 was NaN, and NaN steps make every position they touch NaN; and
        // +inf with the ceiling off was +inf.
        using rmp::app::detail::clamp_delta;
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        CHECK(clamp_delta(nan, 1.0f) == 0.0f);
        CHECK(clamp_delta(nan, 0.0f) == 0.0f);
        CHECK(clamp_delta(-inf, 1.0f) == 0.0f);
        CHECK(clamp_delta(-STEP, 1.0f) == 0.0f);
        CHECK(clamp_delta(inf, 0.0f) == 0.0f);
        // A ceiling cuts +inf to itself, as it cuts any longer frame.
        CHECK(clamp_delta(inf, 0.05f) == 0.05f);
        CHECK(clamp_delta(3.0f, 0.05f) == 0.05f);
        // A ceiling that is no positive number of seconds is no ceiling.
        for (const float off : { 0.0f, -1.0f, nan, inf }) {
            CHECK(clamp_delta(3.0f, off) == 3.0f);
            CHECK(clamp_delta(STEP, off) == STEP);
            CHECK(clamp_delta(inf, off) == 0.0f);
        }
        // And inside the ceiling a reading is what it was.
        CHECK(clamp_delta(STEP, 0.05f) == STEP);
        CHECK(clamp_delta(0.05f, 0.05f) == 0.05f);
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a fixed step that is no number of seconds is the real clock") {
        // set_fixed_delta(+inf) was a step of inf, which [app] max_delta then
        // cut to 0.05 a frame whatever the machine took; NaN is refused too.
        for (const float bad : { std::numeric_limits<float>::infinity(),
                                 std::numeric_limits<float>::quiet_NaN() }) {
            rmp::app::detail::set_fixed_delta(STEP);
            rmp::app::detail::set_fixed_delta(bad);
            CHECK(rmp::app::detail::step_delta() == 0.0f);
            CHECK(rmp::app::detail::frame_delta() == 0.0f);
        }
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

    TEST_CASE_FIXTURE(Fixture,
                      "with no runner stepping, the frame is still timed -- a plain "
                      "raylib loop's menus fade and its notices go") {
        rmp::app::detail::forget_steps_for_tests();
        // The fixed step stands in for what raylib timed: there is no window.
        rmp::app::detail::set_fixed_delta(STEP);
        CHECK(rmp::app::detail::frame_delta() == STEP);
        CHECK(rmp::ui::detail::frame_time() == STEP);
        // Clamped as a step is.
        rmp::app::detail::set_fixed_delta(1.0f);
        if (RMP_MAX_DELTA > 0) {
            CHECK(rmp::app::detail::frame_delta() == doctest::Approx(RMP_MAX_DELTA));
        } else {
            CHECK(rmp::app::detail::frame_delta() == 1.0f);
        }
        // Once a runner steps, its number is the frame's, whatever the clock
        // says after it.
        rmp::app::detail::set_fixed_delta(STEP);
        (void)rmp::app::detail::step_delta();
        rmp::app::detail::set_fixed_delta(1.0f / 240);
        CHECK(rmp::app::detail::frame_delta() == STEP);
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

// ---------------------------------------------------------------------------
// The window's pace: minimized draws nothing and keeps moving; unfocused under
// [window] pause_unfocused stops the scenes, the clock and the sound.
// ---------------------------------------------------------------------------

namespace {

struct Window {
    bool minimized = false;
    bool focused = true;
};

Window &window() {
    static Window w;
    return w;
}

// What the scene on the stack was told, in order: S for _suspend, R for _resume.
std::string &said() {
    static std::string text;
    return text;
}

struct Watched : rmp::Scene {
    void _suspend() override { said() += "S"; }
    void _resume() override { said() += "R"; }
};

// A scene on the stack, a fake window and a fake sound device, and everything
// put back after: the pace, the stack, the audio.
struct Paced {
    Paced() {
        rmp::app::detail::reset_pace_for_tests();
        window() = {};
        rmp::app::detail::set_window_state_provider([](rmp::app::detail::WindowState &w) {
            w.minimized = window().minimized;
            w.focused = window().focused;
        });
        rmp::audio::detail::reset_for_tests();
        said().clear();
        rmp::scenes::detail::start(std::make_unique<Watched>());
        rmp::app::detail::set_fixed_delta(STEP);
    }
    ~Paced() {
        rmp::scenes::detail::shutdown();
        rmp::audio::detail::reset_for_tests();
        rmp::app::detail::reset_pace_for_tests();
        rmp::app::detail::set_fixed_delta(0);
    }
    Paced(const Paced &) = delete;
    Paced &operator=(const Paced &) = delete;

    // One frame as the runner takes it: the pace, then the step.
    static float frame() {
        rmp::app::detail::begin_pace();
        return rmp::app::detail::step_delta();
    }
};

} // namespace

TEST_SUITE("app: the window's pace") {
    TEST_CASE_FIXTURE(Paced, "a focused window updates and draws, and nothing is held") {
        CHECK(frame() == STEP);
        const rmp::app::detail::Pace p = rmp::app::detail::pace();
        CHECK(p.update);
        CHECK(p.draw);
        CHECK(said().empty());
        CHECK_FALSE(rmp::audio::detail::paused_all());
    }

    TEST_CASE_FIXTURE(Paced, "minimized: the world moves and nothing is drawn") {
        window().minimized = true;
        window().focused = false;
        CHECK(frame() == STEP);
        CHECK(rmp::app::detail::pace().update);
        CHECK_FALSE(rmp::app::detail::pace().draw);
        // pause_unfocused is off: no pause, no suspend, the music goes on.
        CHECK(said().empty());
        CHECK_FALSE(rmp::audio::detail::paused_all());
        window() = {};
        CHECK(frame() == STEP);
        CHECK(rmp::app::detail::pace().draw);
    }

    TEST_CASE_FIXTURE(Paced, "unfocused without pause_unfocused changes nothing") {
        window().focused = false;
        CHECK(frame() == STEP);
        CHECK(rmp::app::detail::pace().update);
        CHECK(rmp::app::detail::pace().draw);
        CHECK(said().empty());
    }

    TEST_CASE_FIXTURE(Paced, "pause_unfocused: the scene, the clock and the sound stop, once") {
        rmp::app::detail::set_pause_unfocused(true);
        CHECK(rmp::app::detail::pause_unfocused());
        CHECK(frame() == STEP);
        window().focused = false;
        CHECK(frame() == 0.0f);
        CHECK(rmp::app::detail::frame_delta() == 0.0f);
        CHECK_FALSE(rmp::app::detail::pace().update);
        // The last picture stays: it is still drawn.
        CHECK(rmp::app::detail::pace().draw);
        CHECK(said() == "S");
        CHECK(rmp::audio::detail::paused_all());
        // Still out of focus: no second suspend, the clock still stands.
        CHECK(frame() == 0.0f);
        CHECK(frame() == 0.0f);
        CHECK(said() == "S");
    }

    TEST_CASE_FIXTURE(Paced, "back in focus: resumed, and the first step is 0, then real") {
        rmp::app::detail::set_pause_unfocused(true);
        window().focused = false;
        (void)frame();
        window().focused = true;
        CHECK(frame() == 0.0f); // no giant step on the way back
        CHECK(said() == "SR");
        CHECK_FALSE(rmp::audio::detail::paused_all());
        CHECK(rmp::app::detail::pace().update);
        CHECK(frame() == STEP);
        CHECK(frame() == STEP);
        CHECK(said() == "SR");
    }

    TEST_CASE_FIXTURE(Paced, "minimized under pause_unfocused is a pause with nothing drawn") {
        rmp::app::detail::set_pause_unfocused(true);
        window().minimized = true; // a window that says it still has focus
        CHECK(frame() == 0.0f);
        CHECK_FALSE(rmp::app::detail::pace().update);
        CHECK_FALSE(rmp::app::detail::pace().draw);
        CHECK(said() == "S");
    }

    TEST_CASE_FIXTURE(Paced, "pause and resume of the sound are a pair, and idempotent") {
        rmp::audio::detail::pause_all();
        rmp::audio::detail::pause_all();
        CHECK(rmp::audio::detail::paused_all());
        rmp::audio::detail::resume_all();
        CHECK_FALSE(rmp::audio::detail::paused_all());
        rmp::audio::detail::resume_all();
        CHECK_FALSE(rmp::audio::detail::paused_all());
    }

    TEST_CASE_FIXTURE(Paced, "a skipped frame waits for nothing under a fixed step") {
        rmp::app::detail::frame_skipped(10.0); // would be ten seconds on the real clock
        CHECK(frame() == STEP);
        rmp::app::detail::frame_drawn();
        rmp::app::detail::frame_drawn();
        CHECK(frame() == STEP);
    }

    TEST_CASE("on the real clock a skipped frame waits, and the next step is timed by the clock") {
        rmp::app::detail::reset_pace_for_tests();
        rmp::app::detail::set_fixed_delta(0);
        (void)rmp::app::detail::step_delta();
        rmp::app::detail::frame_skipped(0.002);
        const float step = rmp::app::detail::step_delta();
        CHECK(std::isfinite(step));
        CHECK(step >= 0.0f);
        rmp::app::detail::frame_drawn();
        rmp::app::detail::frame_drawn();
        rmp::app::detail::reset_pace_for_tests();
    }

    TEST_CASE("with no provider the state is raylib's: focused, not minimized, headless") {
        rmp::app::detail::reset_pace_for_tests();
        const rmp::app::detail::WindowState w = rmp::app::detail::window_state();
        CHECK_FALSE(w.minimized);
        CHECK(w.focused);
    }

    TEST_CASE("an empty stack is suspended and resumed without a scene") {
        rmp::scenes::detail::suspend_top();
        rmp::scenes::detail::resume_top();
        CHECK_FALSE(rmp::scenes::detail::running());
    }
}
