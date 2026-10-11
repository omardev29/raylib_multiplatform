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

// The mode in use. The dot says "file state".
struct {
    Stretch mode = Stretch::NONE;
} world;

} // namespace

Stretch stretch() { return world.mode; }

void set_stretch(const Stretch mode) { world.mode = mode; }

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
    const Rectangle whole{ 0, 0, screen.x, screen.y };
    switch (mode) {
        case Stretch::LETTERBOX: {
            const float w = DESIGN_W * s;
            const float h = DESIGN_H * s;
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
