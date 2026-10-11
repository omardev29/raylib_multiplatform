// ---------------------------------------------------------------------------
// The runner's arithmetic, held to what rmp/app.h and src/rmp/internal.h say
// of it for any number a machine, a test or a game can hand it:
//
//   - clamp_delta() -- the step every runner hands the game -- is a finite
//     number of seconds in [0, max] whatever the clock read and whatever the
//     ceiling is, leaves a reading inside the ceiling as it was, cuts a longer
//     one to exactly the ceiling, and never gives a longer frame less time;
//     step_delta() and frame_delta() are it, applied to the step a test fixed;
//   - world_viewport() -- where [window] stretch puts the world -- has a
//     positive finite scale for any screen and any mode, an area inside the
//     screen, LETTERBOX the largest design-shaped area centred on it, EXPAND
//     and NONE the whole screen;
//   - world_clip() is nothing, or whole pixels inside the screen, the
//     letterbox's area to the nearest pixel;
//   - set_stretch() keeps a mode that is one, and refuses one that is not --
//     a settings menu's index past the last mode -- saying so once;
//   - RAY_TEST_WINDOW is read exactly when it is two whole numbers 16..16384
//     joined by an x, against a second reading of that sentence.
//
// rmp::app::quit() and quit_requested() are in src/rmp/app.cpp, which is
// compiled into each executable with its entry point and is not in librmp.a:
// property_test cannot link them, so they stay in waivers.txt.
//
// A counterexample one of these finds becomes a named case in
// tests/clock_test.cpp or tests/stretch_test.cpp; the commits that added them
// say what each was seen red on.
//
// covers: rmp::app::stretch()
// covers: rmp::app::set_stretch(Stretch)
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/internal.h"
#include "../../src/rmp/scene_internal.h"

#include <rmp/app.h>
#include <rmp/config.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <cfloat>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace {

using rmp::app::Stretch;
using rmp::app::detail::clamp_delta;
using rmp::app::detail::world_viewport;
using rmp::app::detail::WorldViewport;

constexpr auto DESIGN_W = static_cast<float>(RMP_WINDOW_WIDTH);
constexpr auto DESIGN_H = static_cast<float>(RMP_WINDOW_HEIGHT);
constexpr float INF = std::numeric_limits<float>::infinity();

// The log off for a property: the refusals below are said on purpose, a
// hundred times a run.
struct QuietLog {
    QuietLog() { SetTraceLogLevel(LOG_NONE); }
    ~QuietLog() { SetTraceLogLevel(LOG_INFO); }
    QuietLog(const QuietLog &) = delete;
    QuietLog &operator=(const QuietLog &) = delete;
    QuietLog(QuietLog &&) = delete;
    QuietLog &operator=(QuietLog &&) = delete;
};

// The clock as it was, whatever a property fixed.
struct RealClock {
    RealClock() = default;
    ~RealClock() {
        rmp::app::detail::set_fixed_delta(0);
        (void)rmp::app::detail::step_delta();
    }
    RealClock(const RealClock &) = delete;
    RealClock &operator=(const RealClock &) = delete;
    RealClock(RealClock &&) = delete;
    RealClock &operator=(RealClock &&) = delete;
};

// The mode the program started with, put back however a property ends.
struct KeepStretch {
    Stretch was = rmp::app::stretch();
    KeepStretch() = default;
    ~KeepStretch() {
        rmp::app::set_stretch(was);
        rmp::detail::reset_reports_for_tests();
    }
    KeepStretch(const KeepStretch &) = delete;
    KeepStretch &operator=(const KeepStretch &) = delete;
    KeepStretch(KeepStretch &&) = delete;
    KeepStretch &operator=(KeepStretch &&) = delete;
};

// A reading of the clock: any float, and the ones the edges are made of --
// a frame at 60 Hz, a stall of seconds, the ceiling itself, a hair either
// side of it -- picked by category, because inRange shrinks with the run's
// size and the early cases would never leave the low end.
rc::Gen<float> reading(const float max) {
    return rc::gen::weightedOneOf<float>({
        { 3, rmp::test::gen::hostile_float() },
        { 1,
          rc::gen::element(1.0f / 60, 1.0f / 240, 0.5f, 3.0f, 0.0f, -0.0f, -1.0f / 60) },
        { 1,
          rc::gen::element(max, std::nextafter(max, INF), std::nextafter(max, -INF),
                           2 * max, max / 2) },
    });
}

// A ceiling: [app] max_delta as configure.py allows it, 0 (off), and what it
// does not allow -- negative, NaN, infinite -- which clamp_delta() still has
// to take, being a function of two floats.
rc::Gen<float> ceiling() {
    return rc::gen::weightedOneOf<float>({
        { 2, rc::gen::element(0.0f, 1.0f / 240, 1.0f / 30, 0.05f, 0.25f, 1.0f) },
        { 1, rmp::test::gen::hostile_float() },
    });
}

bool positive_finite(const float x) { return x > 0 && std::isfinite(x); }

// One mode, of the three there are -- drawn as its number, which is what a
// counterexample prints (0 NONE, 1 LETTERBOX, 2 EXPAND).
Stretch draw_mode() { return static_cast<Stretch>(*rc::gen::element(0, 1, 2)); }

// A screen: two hostile floats, or a window a machine makes -- whole pixels,
// 1..16384 a side -- or one only a test makes: under a pixel, a denormal, past
// what an int can say. By category, for the same reason as reading().
// Drawn one side at a time, so a counterexample prints two floats.
Vector2 draw_screen() {
    const auto side = [] {
        return rc::gen::weightedOneOf<float>({
            // At the nominal size: inRange scales with the run's, and the
            // early cases would only see screens of a few pixels.
            { 2,
              rc::gen::map(rc::gen::resize(rmp::test::gen::NOMINAL_SIZE,
                                           rc::gen::inRange(1, 16385)),
                           [](int px) { return static_cast<float>(px); }) },
            { 1,
              rc::gen::element(1.0f, 16.0f, DESIGN_W, DESIGN_H, 2 * DESIGN_W, 1080.0f,
                               2400.0f, 16384.0f) },
            { 1,
              rc::gen::element(std::numeric_limits<float>::denorm_min(), 1e-40f, FLT_MIN,
                               1e-20f, 0.25f, 0.5f) },
            { 1, rc::gen::element(2.2e9f, 1e12f, 1e30f, FLT_MAX) },
            { 1, rmp::test::gen::hostile_float() },
        });
    };
    // One time in four a screen machines make, whole: two independent sides
    // almost never make a 16:10 monitor, where the letterbox's width once
    // came out a hair past the screen (stretch_test.cpp).
    if (*rc::gen::element(0, 1, 2, 3) == 0) {
        static const std::vector<Vector2> REAL = {
            { 1920, 1200 }, { 480, 854 },   { 480, 800 },  { 1366, 768 }, { 2560, 1080 },
            { 1080, 2400 }, { 3440, 1440 }, { 1024, 768 }, { 1600, 900 }, { 2732, 2048 },
        };
        // By index, which a counterexample prints.
        return REAL[*rc::gen::resize(rmp::test::gen::NOMINAL_SIZE,
                                     rc::gen::inRange<std::size_t>(0, REAL.size()))];
    }
    const float width = *side();
    const float height = *side();
    return Vector2{ width, height };
}

bool has_area(const Vector2 s) {
    return s.x > 0 && s.y > 0 && std::isfinite(s.x) && std::isfinite(s.y);
}

// Within a few parts in a million of `want`: world_viewport() works in floats,
// and the reference below in doubles.
bool near(const double got, const double want) {
    return std::fabs(got - want) <= 4e-6 * std::fmax(1.0, std::fabs(want));
}

void assert_design_at_one(const WorldViewport &v) {
    RC_ASSERT(v.area.x == 0.0f);
    RC_ASSERT(v.area.y == 0.0f);
    RC_ASSERT(v.area.width == DESIGN_W);
    RC_ASSERT(v.area.height == DESIGN_H);
    RC_ASSERT(v.scale == 1.0f);
}

// The sentence of scene_internal.h, read a second way: a regular expression
// and std::stoi, where parse_test_window() walks the characters itself.
std::optional<rmp::scenes::detail::WindowSize> read_by_regex(const std::string &text) {
    static const std::regex SHAPE("([0-9]{1,5})x([0-9]{1,5})");
    std::smatch m;
    if (!std::regex_match(text, m, SHAPE)) return std::nullopt;
    const int w = std::stoi(m[1].str());
    const int h = std::stoi(m[2].str());
    if (w < 16 || w > 16384 || h < 16 || h > 16384) return std::nullopt;
    return rmp::scenes::detail::WindowSize{ w, h };
}

// What a RAY_TEST_WINDOW can be: pieces of what a right one is made of --
// digits, a lower-case x -- among what a typo puts there.
rc::Gen<std::string> window_text() {
    return rc::gen::weightedOneOf<std::string>({
        { 3, rc::gen::exec([] {
              const auto pieces = *rc::gen::container<std::vector<std::string>>(
                  rc::gen::element<std::string>(
                      "0", "1", "6", "9", "16", "384", "1080", "16384", "16385", "00016",
                      "x", "X", " ", "+", "-", "e", "\xC2\xB2", std::string(1, '\0')));
              std::string text;
              for (const std::string &piece : pieces) text += piece;
              return text;
          }) },
        { 2, rc::gen::exec([] {
              const int w = *rc::gen::inRange(0, 20000);
              const int h = *rc::gen::inRange(0, 20000);
              return std::to_string(w) + "x" + std::to_string(h);
          }) },
        { 1, rmp::test::gen::hostile_string() },
    });
}

} // namespace

TEST_SUITE("property: app") {
    TEST_CASE("the step is a finite number of seconds in [0, max]") {
        rmp::test::check(
            "clamp_delta is a finite number of seconds in [0, max] for any reading and "
            "any ceiling",
            [] {
                const float max = *ceiling();
                const float raw = *reading(max);
                const float step = clamp_delta(raw, max);
                RC_ASSERT(std::isfinite(step));
                RC_ASSERT(step >= 0.0f);
                if (positive_finite(max)) RC_ASSERT(step <= max);
                // Nothing that is not a number of seconds becomes one.
                if (!(raw >= 0)) RC_ASSERT(step == 0.0f);
            });

        rmp::test::check(
            "clamp_delta leaves a reading inside its ceiling as it was, and cuts a "
            "longer one to exactly the ceiling",
            [] {
                const float max = *ceiling();
                const float raw = *reading(max);
                const float step = clamp_delta(raw, max);
                const bool clamps = positive_finite(max);
                if (raw >= 0 && std::isfinite(raw) && (!clamps || raw <= max)) {
                    RC_ASSERT(step == raw);
                }
                if (clamps && raw > max) RC_ASSERT(step == max);
            });

        rmp::test::check("clamp_delta never gives a longer frame less time", [] {
            const float max = *ceiling();
            const float a = *reading(max);
            const float b = *reading(max);
            RC_PRE(a >= 0 && b >= 0 && std::isfinite(a) && std::isfinite(b));
            const float shorter = std::fmin(a, b);
            const float longer = std::fmax(a, b);
            RC_ASSERT(clamp_delta(shorter, max) <= clamp_delta(longer, max));
        });

        rmp::test::check(
            "step_delta and frame_delta are the fixed step clamped, and the real clock "
            "when the step is no number of seconds",
            [] {
                const RealClock restore;
                const float fixed = *reading(RMP_MAX_DELTA);
                rmp::app::detail::set_fixed_delta(fixed);
                const float step = rmp::app::detail::step_delta();
                // With no window the real clock reads 0.
                const float want =
                    positive_finite(fixed) ? clamp_delta(fixed, RMP_MAX_DELTA) : 0.0f;
                RC_ASSERT(step == want);
                RC_ASSERT(rmp::app::detail::frame_delta() == want);
                rmp::app::detail::forget_steps_for_tests();
                RC_ASSERT(rmp::app::detail::frame_delta() == want);
            });
    }

    TEST_CASE("the world fits any screen") {
        rmp::test::check(
            "world_viewport's scale is a positive finite number for any screen and mode",
            [] {
                const Vector2 s = draw_screen();
                const WorldViewport v = world_viewport(draw_mode(), s);
                RC_ASSERT(v.scale > 0.0f);
                RC_ASSERT(std::isfinite(v.scale));
                RC_ASSERT(std::isfinite(v.area.x));
                RC_ASSERT(std::isfinite(v.area.y));
                RC_ASSERT(std::isfinite(v.area.width));
                RC_ASSERT(std::isfinite(v.area.height));
                RC_ASSERT(v.area.width > 0.0f);
                RC_ASSERT(v.area.height > 0.0f);
            });

        rmp::test::check(
            "world_viewport puts the world inside a screen a window can have, and the "
            "design at 1:1 on one with no area",
            [] {
                const Vector2 s = draw_screen();
                const Stretch m = draw_mode();
                const WorldViewport v = world_viewport(m, s);
                if (!has_area(s)) {
                    assert_design_at_one(v);
                    return;
                }
                // Whole pixels, a window's worth: what GetScreenWidth() gives.
                RC_PRE(s.x >= 1 && s.y >= 1 && s.x <= 16384 && s.y <= 16384);
                RC_ASSERT(v.area.x >= 0.0f);
                RC_ASSERT(v.area.y >= 0.0f);
                // The far edge is the near one plus the width, rounded once.
                RC_ASSERT(v.area.x + v.area.width <= s.x * (1 + 1e-6f));
                RC_ASSERT(v.area.y + v.area.height <= s.y * (1 + 1e-6f));
            });

        rmp::test::check(
            "LETTERBOX is the largest design-shaped area centred on the screen; EXPAND "
            "and NONE are the whole screen",
            [] {
                const Vector2 s = draw_screen();
                RC_PRE(has_area(s));
                const double fit = std::fmin(static_cast<double>(s.x) / DESIGN_W,
                                             static_cast<double>(s.y) / DESIGN_H);
                // Where the design fits at a float scale at all.
                RC_PRE(fit >= FLT_MIN && fit <= FLT_MAX / DESIGN_W);
                const WorldViewport box = world_viewport(Stretch::LETTERBOX, s);
                RC_ASSERT(near(box.scale, fit));
                RC_ASSERT(near(box.area.width, DESIGN_W * fit));
                RC_ASSERT(near(box.area.height, DESIGN_H * fit));
                RC_ASSERT(near(box.area.x + static_cast<double>(box.area.width) / 2,
                               static_cast<double>(s.x) / 2));
                RC_ASSERT(near(box.area.y + static_cast<double>(box.area.height) / 2,
                               static_cast<double>(s.y) / 2));

                const WorldViewport expand = world_viewport(Stretch::EXPAND, s);
                const WorldViewport none = world_viewport(Stretch::NONE, s);
                for (const WorldViewport &whole : { expand, none }) {
                    RC_ASSERT(whole.area.x == 0.0f);
                    RC_ASSERT(whole.area.y == 0.0f);
                    RC_ASSERT(whole.area.width == s.x);
                    RC_ASSERT(whole.area.height == s.y);
                }
                RC_ASSERT(expand.scale == box.scale);
                RC_ASSERT(none.scale == 1.0f);
            });

        rmp::test::check(
            "world_clip is nothing, or the letterbox's area to the nearest whole pixel "
            "inside the screen",
            [] {
                const Vector2 s = draw_screen();
                const Stretch m = draw_mode();
                const std::optional<rmp::scenes::detail::WorldClip> clip =
                    rmp::scenes::detail::world_clip(m, s);
                if (m != Stretch::LETTERBOX || !has_area(s)) {
                    RC_ASSERT(!clip.has_value());
                    return;
                }
                if (!clip) return;
                RC_ASSERT(clip->x >= 0);
                RC_ASSERT(clip->y >= 0);
                RC_ASSERT(clip->width >= 0);
                RC_ASSERT(clip->height >= 0);
                // In 64 bits: the sum of two ints a broken clip holds can
                // overflow one.
                const auto right = static_cast<double>(clip->x) + clip->width;
                const auto bottom = static_cast<double>(clip->y) + clip->height;
                RC_ASSERT(right <= std::ceil(static_cast<double>(s.x)));
                RC_ASSERT(bottom <= std::ceil(static_cast<double>(s.y)));
                const Rectangle area = world_viewport(m, s).area;
                RC_ASSERT(std::fabs(clip->x - static_cast<double>(area.x)) <= 0.5);
                RC_ASSERT(std::fabs(clip->y - static_cast<double>(area.y)) <= 0.5);
                RC_ASSERT(std::fabs(right - (static_cast<double>(area.x) + area.width)) <=
                          0.5);
                RC_ASSERT(std::fabs(bottom -
                                    (static_cast<double>(area.y) + area.height)) <= 0.5);
            });
    }

    TEST_CASE("the mode is one of the three") {
        const QuietLog quiet;
        rmp::test::check(
            "set_stretch keeps a mode that is one and refuses one that is not, saying "
            "so once",
            [] {
                const KeepStretch keep;
                rmp::detail::reset_reports_for_tests();
                const Stretch before = draw_mode();
                rmp::app::set_stretch(before);
                RC_ASSERT(rmp::app::stretch() == before);
                RC_ASSERT(rmp::detail::report_count() == 0);
                // Any int: a scoped enum holds every value of its underlying
                // type, and a settings menu's index is one.
                const int asked = *rc::gen::weightedOneOf<int>({
                    { 2, rc::gen::element(0, 1, 2) },
                    { 1, rc::gen::element(-1, 3, 4, INT_MAX, INT_MIN) },
                    { 1, rc::gen::arbitrary<int>() },
                });
                const bool real = asked >= 0 && asked <= 2;
                for (int again = 0; again < 2; ++again) {
                    rmp::app::set_stretch(static_cast<Stretch>(asked));
                    RC_ASSERT(rmp::app::stretch() ==
                              (real ? static_cast<Stretch>(asked) : before));
                }
                RC_ASSERT(rmp::detail::report_count() == (real ? 0 : 1));
                // What the camera and the interface are handed is a mode's.
                const WorldViewport v = world_viewport();
                RC_ASSERT(v.scale > 0.0f);
            });
    }

    TEST_CASE("RAY_TEST_WINDOW") {
        const QuietLog quiet;
        rmp::test::check(
            "a test window is read exactly when it is two whole numbers 16..16384 joined "
            "by an x",
            [] {
                const std::string text = *window_text();
                const std::optional<rmp::scenes::detail::WindowSize> got =
                    rmp::scenes::detail::parse_test_window(text);
                const std::optional<rmp::scenes::detail::WindowSize> want =
                    read_by_regex(text);
                RC_ASSERT(got.has_value() == want.has_value());
                if (got && want) {
                    RC_ASSERT(got->width == want->width);
                    RC_ASSERT(got->height == want->height);
                }
                const rmp::scenes::detail::TestWindow asked =
                    rmp::scenes::detail::read_test_window(text);
                RC_ASSERT(asked.size.has_value() == got.has_value());
                RC_ASSERT(asked.refused == (!text.empty() && !got.has_value()));
            });
    }
}
