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

#include <rmp/app.h>
#include <rmp/config.h>

#include <cmath>
#include <limits>

namespace {

using rmp::app::Stretch;
using rmp::app::detail::world_viewport;
using rmp::app::detail::WorldViewport;

constexpr auto W = static_cast<float>(RMP_WINDOW_WIDTH);
constexpr auto H = static_cast<float>(RMP_WINDOW_HEIGHT);

// The mode the test found, put back however it ends.
struct Fixture {
    Stretch was = rmp::app::stretch();
    Fixture() = default;
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    ~Fixture() { rmp::app::set_stretch(was); }
};

void check_area(const WorldViewport &v, float x, float y, float w, float h, float scale) {
    CHECK(v.area.x == doctest::Approx(x));
    CHECK(v.area.y == doctest::Approx(y));
    CHECK(v.area.width == doctest::Approx(w));
    CHECK(v.area.height == doctest::Approx(h));
    CHECK(v.scale == doctest::Approx(scale));
}

} // namespace

TEST_SUITE("app: stretch") {
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

    TEST_CASE_FIXTURE(Fixture,
                      "with no window the screen is the design size, in every mode") {
        for (const Stretch mode :
             { Stretch::NONE, Stretch::LETTERBOX, Stretch::EXPAND }) {
            rmp::app::set_stretch(mode);
            check_area(world_viewport(), 0, 0, W, H, 1);
        }
    }
}
