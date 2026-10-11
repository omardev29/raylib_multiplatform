// ---------------------------------------------------------------------------
// [window] stretch: how the world fits a window that is not the design size
// (rmp::app::stretch(), src/rmp/stretch.cpp).
//
// world_viewport() is the one function the camera and the interface both draw
// their numbers from, so its arithmetic is pinned here for every mode, on a
// screen bigger than the design, wider, taller, a phone held upright, and one
// with no area at all. No window: the screen is a number handed in.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/internal.h"
#include "../src/rmp/object_internal.h"
#include "../src/rmp/scene_internal.h"

#include <rmp/app.h>
#include <rmp/config.h>

#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

namespace {

using rmp::app::Stretch;
using rmp::app::detail::world_viewport;
using rmp::app::detail::WorldViewport;

constexpr auto W = static_cast<float>(RMP_WINDOW_WIDTH);
constexpr auto H = static_cast<float>(RMP_WINDOW_HEIGHT);

// The mode the test found, and the window, put back however it ends.
struct Fixture {
    Stretch was = rmp::app::stretch();
    Fixture() = default;
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    Fixture(Fixture &&) = delete;
    Fixture &operator=(Fixture &&) = delete;
    ~Fixture() {
        rmp::objects::detail::set_test_screen(0, 0);
        rmp::app::set_stretch(was);
    }
};

void check_clip(const std::optional<rmp::scenes::detail::WorldClip> &clip, const int x,
                const int y, const int w, const int h) {
    REQUIRE(clip.has_value());
    const rmp::scenes::detail::WorldClip got =
        clip.value_or(rmp::scenes::detail::WorldClip{ -1, -1, -1, -1 });
    CHECK(got.x == x);
    CHECK(got.y == y);
    CHECK(got.width == w);
    CHECK(got.height == h);
}

void check_area(const WorldViewport &v, float x, float y, float w, float h, float scale) {
    CHECK(v.area.x == doctest::Approx(x));
    CHECK(v.area.y == doctest::Approx(y));
    CHECK(v.area.width == doctest::Approx(w));
    CHECK(v.area.height == doctest::Approx(h));
    CHECK(v.scale == doctest::Approx(scale));
}

} // namespace

TEST_SUITE("app: stretch") {
    TEST_CASE("the mode starts as the .toml's [window] stretch") {
        // Whenever this runs: every test that sets a mode puts back the one
        // it found, so between tests the mode is the one the program started
        // with -- and a test that leaks one fails here, in one order or the
        // other. The order of rmp::app::Stretch is the generator's
        // (StretchModesAgreeTest in tests/configure_test.py).
        CHECK(rmp::app::stretch() == static_cast<Stretch>(RMP_WINDOW_STRETCH));
        // The framework's .toml says letterbox, so this is not the zero a
        // forgotten initialiser would also give.
        CHECK(RMP_WINDOW_STRETCH != 0);
    }

    TEST_CASE_FIXTURE(Fixture, "the mode is the one set, each of the three") {
        for (const Stretch mode :
             { Stretch::LETTERBOX, Stretch::EXPAND, Stretch::NONE }) {
            rmp::app::set_stretch(mode);
            CHECK(rmp::app::stretch() == mode);
        }
    }

    TEST_CASE("twice the design size: every mode fills it, NONE at 1:1") {
        const Vector2 screen{ 2 * W, 2 * H };
        check_area(world_viewport(Stretch::NONE, screen), 0, 0, 2 * W, 2 * H, 1);
        check_area(world_viewport(Stretch::LETTERBOX, screen), 0, 0, 2 * W, 2 * H, 2);
        check_area(world_viewport(Stretch::EXPAND, screen), 0, 0, 2 * W, 2 * H, 2);
    }

    TEST_CASE("wider than the design: LETTERBOX puts bars left and right") {
        const Vector2 screen{ 3 * W, H };
        check_area(world_viewport(Stretch::LETTERBOX, screen), W, 0, W, H, 1);
        check_area(world_viewport(Stretch::EXPAND, screen), 0, 0, 3 * W, H, 1);
        check_area(world_viewport(Stretch::NONE, screen), 0, 0, 3 * W, H, 1);
    }

    TEST_CASE("taller than the design: LETTERBOX puts bars above and below") {
        const Vector2 screen{ 2 * W, 4 * H };
        check_area(world_viewport(Stretch::LETTERBOX, screen), 0, H, 2 * W, 2 * H, 2);
        check_area(world_viewport(Stretch::EXPAND, screen), 0, 0, 2 * W, 4 * H, 2);
    }

    TEST_CASE("a phone held upright, whatever the design: the area keeps its shape") {
        const Vector2 screen{ 1080, 2400 };
        const float s = std::fmin(1080 / W, 2400 / H);
        const WorldViewport v = world_viewport(Stretch::LETTERBOX, screen);
        check_area(v, (1080 - W * s) / 2, (2400 - H * s) / 2, W * s, H * s, s);
        CHECK(v.area.width / v.area.height == doctest::Approx(W / H));
        // Inside the screen, and centred on it.
        CHECK(v.area.x >= 0);
        CHECK(v.area.y >= 0);
        CHECK(v.area.x + v.area.width <= 1080 + 0.01f);
        CHECK(v.area.y + v.area.height <= 2400 + 0.01f);
        CHECK(v.area.x + v.area.width / 2 == doctest::Approx(540));
        CHECK(v.area.y + v.area.height / 2 == doctest::Approx(1200));
    }

    TEST_CASE("a screen with no area, or not a number, is the design at 1:1") {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        for (const Vector2 screen :
             { Vector2{ 0, 0 }, Vector2{ -5, 100 }, Vector2{ 100, 0 },
               Vector2{ nan, 100 }, Vector2{ 100, inf } }) {
            for (const Stretch mode :
                 { Stretch::NONE, Stretch::LETTERBOX, Stretch::EXPAND }) {
                check_area(world_viewport(mode, screen), 0, 0, W, H, 1);
            }
        }
    }

    TEST_CASE(
        "a screen the design fits at no scale is the design at 1:1, not a scale of 0") {
        // tests/property/app_prop.cpp's counterexample: a denormal's worth of
        // pixels by 1e-20 under EXPAND was a scale of 0 -- and the camera
        // divides by the scale, so the view was infinite.
        const float tiny = std::numeric_limits<float>::denorm_min();
        for (const Stretch mode :
             { Stretch::NONE, Stretch::LETTERBOX, Stretch::EXPAND }) {
            check_area(world_viewport(mode, Vector2{ tiny, 1e-20f }), 0, 0, W, H, 1);
            check_area(world_viewport(mode, Vector2{ tiny, tiny }), 0, 0, W, H, 1);
        }
    }

    TEST_CASE("the largest screen a float holds: the letterbox is finite and inside it") {
        // The design times a scale rounded up was past FLT_MAX: an infinite
        // width, and a bar of -inf.
        const float most = std::numeric_limits<float>::max();
        const WorldViewport v = world_viewport(Stretch::LETTERBOX, Vector2{ most, most });
        CHECK(std::isfinite(v.area.width));
        CHECK(std::isfinite(v.area.height));
        CHECK(v.area.x >= 0);
        CHECK(v.area.y >= 0);
        CHECK(v.area.width <= most);
        CHECK(v.scale > 0);
        CHECK(std::isfinite(v.scale));
    }

    TEST_CASE("a letterbox never reaches past the screen, by a hair or more") {
        // The area is the design times a scale that was rounded: with the
        // framework's 800 x 450 on a 1920 x 1200 monitor its width came out a
        // hair past the screen's 1920 columns, and the bar to its left a hair
        // under 0 -- and the same on a 480 x 854 phone held upright.
        for (const Vector2 screen : { Vector2{ 1920, 1200 }, Vector2{ 480, 854 } }) {
            const WorldViewport v = world_viewport(Stretch::LETTERBOX, screen);
            CHECK(v.area.x >= 0);
            CHECK(v.area.width <= screen.x);
        }
        for (int px = 1; px <= 4096; ++px) {
            const auto side = static_cast<float>(px);
            for (const Vector2 screen :
                 { Vector2{ side, 2 * H }, Vector2{ 2 * W, side } }) {
                const WorldViewport v = world_viewport(Stretch::LETTERBOX, screen);
                CHECK(v.area.x >= 0);
                CHECK(v.area.y >= 0);
                CHECK(v.area.width <= screen.x);
                CHECK(v.area.height <= screen.y);
            }
        }
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a number that is no mode keeps the mode, and is said once") {
        // A settings menu cycling through the modes by number, one past the
        // last. It was stored, and drew as NONE with nothing said.
        rmp::app::set_stretch(Stretch::LETTERBOX);
        rmp::detail::reset_reports_for_tests();
        rmp::app::set_stretch(static_cast<Stretch>(4));
        CHECK(rmp::app::stretch() == Stretch::LETTERBOX);
        CHECK(rmp::detail::report_count() == 1);
        rmp::app::set_stretch(static_cast<Stretch>(-1));
        CHECK(rmp::app::stretch() == Stretch::LETTERBOX);
        CHECK(rmp::detail::report_count() == 1);
        rmp::app::set_stretch(Stretch::EXPAND);
        CHECK(rmp::app::stretch() == Stretch::EXPAND);
        rmp::detail::reset_reports_for_tests();
    }

    TEST_CASE_FIXTURE(Fixture,
                      "with no window the screen is the design size, in every mode") {
        for (const Stretch mode :
             { Stretch::NONE, Stretch::LETTERBOX, Stretch::EXPAND }) {
            rmp::app::set_stretch(mode);
            check_area(world_viewport(), 0, 0, W, H, 1);
        }
    }

    TEST_CASE_FIXTURE(Fixture, "a test can hand the camera a screen, and take it back") {
        using rmp::objects::detail::set_test_screen;
        using rmp::objects::detail::view_rect;
        set_test_screen(1600, 900);
        CHECK(view_rect().x == 0);
        CHECK(view_rect().y == 0);
        CHECK(view_rect().width == 1600);
        CHECK(view_rect().height == 900);
        rmp::app::set_stretch(Stretch::LETTERBOX);
        check_area(world_viewport(), 0, 0, 1600, 900, 1600 / W);
        // Nothing that is not a size: the window again -- with none, the design.
        for (const Vector2 bad :
             { Vector2{ 0, 0 }, Vector2{ -1, 900 }, Vector2{ 1600, 0 },
               Vector2{ std::numeric_limits<float>::quiet_NaN(), 900 },
               Vector2{ 1600, std::numeric_limits<float>::infinity() } }) {
            set_test_screen(1600, 900);
            set_test_screen(bad.x, bad.y);
            CHECK(view_rect().width == W);
            CHECK(view_rect().height == H);
        }
        // And the reset every object test runs between cases takes it back too.
        set_test_screen(1080, 2400);
        rmp::objects::detail::reset_for_tests();
        CHECK(view_rect().width == W);
        CHECK(view_rect().height == H);
    }
}

// ---------------------------------------------------------------------------
// LETTERBOX cuts the world to its area while the map and the objects are
// drawn (scene.cpp), so nothing of the world shows on the bars. The rectangle,
// in the whole pixels BeginScissorMode() takes -- and none at all wherever
// there are no bars, so every other picture is drawn exactly as before.
// ---------------------------------------------------------------------------

TEST_SUITE("app: stretch: the letterbox's clip") {
    using rmp::scenes::detail::world_clip;

    TEST_CASE("no clip where the world may cover the window") {
        for (const Vector2 screen :
             { Vector2{ W, H }, Vector2{ 2 * W, 2 * H }, Vector2{ 3 * W, H },
               Vector2{ 1080, 2400 }, Vector2{ 0, 0 } }) {
            CHECK_FALSE(world_clip(Stretch::NONE, screen).has_value());
            CHECK_FALSE(world_clip(Stretch::EXPAND, screen).has_value());
        }
        // A letterbox with no bars: the design's own shape, at any size.
        CHECK_FALSE(world_clip(Stretch::LETTERBOX, Vector2{ W, H }).has_value());
        CHECK_FALSE(world_clip(Stretch::LETTERBOX, Vector2{ 2 * W, 2 * H }).has_value());
        CHECK_FALSE(world_clip(Stretch::LETTERBOX, Vector2{ W / 2, H / 2 }).has_value());
        // And a screen with no area is the design: nothing to cut.
        CHECK_FALSE(world_clip(Stretch::LETTERBOX, Vector2{ 0, 0 }).has_value());
        CHECK_FALSE(world_clip(Stretch::LETTERBOX,
                               Vector2{ std::numeric_limits<float>::quiet_NaN(), H })
                        .has_value());
    }

    TEST_CASE("bars left and right: the clip is the middle") {
        check_clip(world_clip(Stretch::LETTERBOX, Vector2{ 3 * W, H }),
                   static_cast<int>(W), 0, static_cast<int>(W), static_cast<int>(H));
    }

    TEST_CASE("bars above and below: the clip is the strip, edges rounded") {
        // A phone held upright. The area's edges are rarely whole pixels; each
        // edge is rounded on its own, so the clip neither shows half a pixel
        // of the world on a bar nor cuts one off the game.
        const Vector2 screen{ 1080, 2400 };
        const Rectangle area = world_viewport(Stretch::LETTERBOX, screen).area;
        const auto top = static_cast<int>(std::lround(area.y));
        const auto bottom = static_cast<int>(std::lround(area.y + area.height));
        check_clip(world_clip(Stretch::LETTERBOX, screen), 0, top, 1080, bottom - top);
        CHECK(top > 0);
        CHECK(bottom < 2400);
        // Centred: the two bars differ by a pixel at most.
        CHECK(std::abs(top - (2400 - bottom)) <= 1);
    }

    TEST_CASE("the extremes: a 4096 square, a bar under a pixel, one pixel") {
        // A square monitor: the design across its width, bars above and below
        // of exactly (4096 - 4096 / W * H) / 2 pixels each.
        const auto s = 4096 / W;
        const auto bar = static_cast<int>(std::lround((4096 - H * s) / 2));
        check_clip(world_clip(Stretch::LETTERBOX, Vector2{ 4096, 4096 }), 0, bar, 4096,
                   4096 - 2 * bar);
        // One pixel wider than the design: half a pixel of bar on each side.
        // Each edge is rounded on its own, so the clip is still the design's
        // width, and inside the window.
        const auto wider = world_clip(Stretch::LETTERBOX, Vector2{ W + 1, H });
        REQUIRE(wider.has_value());
        const rmp::scenes::detail::WorldClip got =
            wider.value_or(rmp::scenes::detail::WorldClip{ -1, -1, -1, -1 });
        CHECK(got.width == static_cast<int>(W));
        CHECK(got.x >= 0);
        CHECK(got.x + got.width <= static_cast<int>(W) + 1);
        // A screen past what an int can say is not cut -- the edges were
        // lround()ed out of range of an int, and the clip landed half a
        // float's range from the area (app_prop.cpp: 1600 x FLT_MAX). Nor is
        // one under a pixel, which has no pixel to cut.
        for (const Vector2 screen :
             { Vector2{ 1600, std::numeric_limits<float>::max() }, Vector2{ 2.2e9f, H },
               Vector2{ W, 1e30f }, Vector2{ 0.5f, 0.25f } }) {
            CAPTURE(screen.x);
            CAPTURE(screen.y);
            CHECK_FALSE(world_clip(Stretch::LETTERBOX, screen).has_value());
        }
        // A window of one pixel has no bar a pixel wide: nothing to cut.
        CHECK_FALSE(world_clip(Stretch::LETTERBOX, Vector2{ 1, 1 }).has_value());
        // And what is not a size never cuts, whichever way it is not one.
        const float inf = std::numeric_limits<float>::infinity();
        for (const Vector2 screen : { Vector2{ inf, H }, Vector2{ W, inf },
                                      Vector2{ -W, H }, Vector2{ W, -H } }) {
            CHECK_FALSE(world_clip(Stretch::LETTERBOX, screen).has_value());
        }
    }
}

// ---------------------------------------------------------------------------
// RAY_TEST_WINDOW=WxH, which the CI smoke test may hand RMP_GAME's window so a
// stretch is booted on a screen of another shape. What it takes, and that
// everything else is nothing -- a boot then fails instead of passing at the
// design size, where every mode looks the same.
// ---------------------------------------------------------------------------

TEST_SUITE("app: stretch: RAY_TEST_WINDOW") {
    using rmp::scenes::detail::parse_test_window;

    TEST_CASE("two whole numbers joined by an x, each 16..16384") {
        for (const auto &[text, w, h] :
             { std::tuple{ "1080x2400", 1080, 2400 },
               std::tuple{ "2400x1080", 2400, 1080 }, std::tuple{ "16x16", 16, 16 },
               std::tuple{ "16384x16384", 16384, 16384 },
               std::tuple{ "0800x450", 800, 450 } }) {
            CAPTURE(text);
            const auto size = parse_test_window(text);
            REQUIRE(size.has_value());
            const rmp::scenes::detail::WindowSize got =
                size.value_or(rmp::scenes::detail::WindowSize{ 0, 0 });
            CHECK(got.width == w);
            CHECK(got.height == h);
        }
    }

    TEST_CASE("anything else is nothing") {
        for (const std::string_view text :
             std::to_array<std::string_view>({ "",
                                               "x",
                                               "1080",
                                               "1080x",
                                               "x2400",
                                               "1080 x 2400",
                                               " 1080x2400",
                                               "1080x2400 ",
                                               "1080X2400",
                                               "1080x2400x3",
                                               "-1080x2400",
                                               "+1080x2400",
                                               "1080x-2400",
                                               "15x100",
                                               "100x15",
                                               "16385x100",
                                               "100x16385",
                                               "0x0",
                                               "1e3x100",
                                               "10.5x100",
                                               "99999999999x100",
                                               "100x99999999999",
                                               "000000000800x450",
                                               "wide",
                                               "1080,2400" })) {
            const std::string shown(text);
            CAPTURE(shown);
            CHECK_FALSE(parse_test_window(text).has_value());
        }
    }

    TEST_CASE("what start() is told: nothing, a size, or a refusal") {
        using rmp::scenes::detail::read_test_window;
        using rmp::scenes::detail::TestWindow;
        // Not set, or not the smoke test: the .toml's window, and a boot.
        const TestWindow none = read_test_window("");
        CHECK_FALSE(none.size.has_value());
        CHECK_FALSE(none.refused);
        // A size it reads is the window's.
        const TestWindow tall = read_test_window("1080x2400");
        REQUIRE(tall.size.has_value());
        CHECK(tall.size.value_or(rmp::scenes::detail::WindowSize{ 0, 0 }).width == 1080);
        CHECK(tall.size.value_or(rmp::scenes::detail::WindowSize{ 0, 0 }).height == 2400);
        CHECK_FALSE(tall.refused);
        // Anything else: no size, and the boot is refused rather than run at
        // the design size, where every stretch is the same picture.
        for (const std::string_view bad :
             std::to_array<std::string_view>({ "1080 x 2400", "tall", "x", "15x15" })) {
            const std::string shown(bad);
            CAPTURE(shown);
            const TestWindow got = read_test_window(bad);
            CHECK_FALSE(got.size.has_value());
            CHECK(got.refused);
        }
    }
}
