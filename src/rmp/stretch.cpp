// ===========================================================================
// rmp::app::stretch() -- see include/rmp/app.h for the three modes, and
// "stretch.cpp" in src/rmp/internal.h for world_viewport(), the numbers the
// camera and the interface both draw from.
//
// In the library and not in app.cpp, which is compiled into each executable
// with its entry point: a test binary has none, and it is the one that has to
// set a mode and read the numbers back.
// ===========================================================================

#include "internal.h"
#include "object_internal.h"

#include <rmp/app.h>
#include <rmp/config.h>

#include <raylib.h>

#include <cmath>

namespace rmp::app {

namespace {

// The mode in use, from [window] stretch until set_stretch() says otherwise.
// RMP_WINDOW_STRETCH is the mode's index in rmp::app::Stretch -- the
// generator writes them in the enum's order, and StretchModesAgreeTest in
// tests/configure_test.py holds the two together. A constant initialiser, so
// the value is there before any static initialiser of any translation unit
// could ask. The dot says "file state".
struct {
    Stretch mode = static_cast<Stretch>(RMP_WINDOW_STRETCH);
} world;

} // namespace

Stretch stretch() { return world.mode; }

// Any int is a Stretch -- a scoped enum holds every value of its underlying
// type -- and a settings menu cycling through the modes by number reaches one
// past the last. That is no mode: the one in use stays, and it is said once.
void set_stretch(const Stretch mode) {
    switch (mode) {
        case Stretch::NONE:
        case Stretch::LETTERBOX:
        case Stretch::EXPAND:
            world.mode = mode;
            return;
    }
    RMP_REPORT_ONCE("APP: set_stretch(%d) is no mode -- Stretch::NONE, LETTERBOX or "
                    "EXPAND; the mode stays what it was.",
                    static_cast<int>(mode));
}

namespace detail {

WorldViewport world_viewport(const Stretch mode, const Vector2 screen) {
    constexpr auto DESIGN_W = static_cast<float>(RMP_WINDOW_WIDTH);
    constexpr auto DESIGN_H = static_cast<float>(RMP_WINDOW_HEIGHT);
    // `!(x > 0)` and not `x <= 0`: NaN is neither, and a screen that is not a
    // number has no area either.
    if (!(screen.x > 0) || !(screen.y > 0) || !std::isfinite(screen.x) ||
        !std::isfinite(screen.y)) {
        return WorldViewport{ Rectangle{ 0, 0, DESIGN_W, DESIGN_H }, 1.0f };
    }
    const float sx = screen.x / DESIGN_W;
    const float sy = screen.y / DESIGN_H;
    const float s = sx < sy ? sx : sy;
    // A screen so small that the design fits it at no scale a float can hold
    // -- a denormal's worth of pixels, which only a test hands in -- is one
    // with no area: a scale of 0 is a camera that divides by it.
    if (!(s > 0)) return WorldViewport{ Rectangle{ 0, 0, DESIGN_W, DESIGN_H }, 1.0f };
    const Rectangle whole{ 0, 0, screen.x, screen.y };
    switch (mode) {
        case Stretch::LETTERBOX: {
            // Never more than the screen: the design times a scale that was
            // rounded can be a hair past it -- and past the largest float,
            // infinite, on a screen near it.
            const float w = std::fmin(DESIGN_W * s, screen.x);
            const float h = std::fmin(DESIGN_H * s, screen.y);
            return WorldViewport{
                Rectangle{ (screen.x - w) / 2, (screen.y - h) / 2, w, h }, s
            };
        }
        case Stretch::EXPAND:
            return WorldViewport{ whole, s };
        case Stretch::NONE:
            break;
    }
    return WorldViewport{ whole, 1.0f };
}

WorldViewport world_viewport() {
    const Rectangle screen = rmp::objects::detail::view_rect();
    return world_viewport(world.mode, Vector2{ screen.width, screen.height });
}

} // namespace detail

} // namespace rmp::app
