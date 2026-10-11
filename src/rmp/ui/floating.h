#pragma once
// ---------------------------------------------------------------------------
// Private to what floats over the rest of the interface -- at() (at.cpp),
// tooltip() (tooltip.cpp) and notify() (notify.cpp) -- and their tests. The
// hooks the app, the scenes and the rest of rmp::ui call are declared with
// the others, in ../ui_internal.h and internal.h.
// ---------------------------------------------------------------------------

#include <raylib.h> // Camera2D, Rectangle, Vector2
#include <rmp/ui.h> // Align, ColorRole

#include <cstdint>
#include <string_view>

namespace rmp::ui::detail {

// --- at.cpp ----------------------------------------------------------------

// The camera the pass being described sees the world through: what the scene
// stack last set (set_pass_camera()), the identity -- zoom 1, nothing moved
// -- when it set none.
Camera2D pass_camera();

// Where at() puts a point of the world on the screen, in pixels: through
// `camera`, as raylib's GetWorldToScreen2D() does. A point that is not a
// number, or is further out than any screen reaches, is kept far off it
// instead, where Clay culls it and nothing converts it to an int.
Vector2 world_to_screen(Vector2 world, const Camera2D &camera);

// The z the last at() box was laid out at. Below 0: a box over the world is
// drawn under the rest of its pass -- the HUD over the health bars, not the
// other way round -- and the boxes among themselves in the order they were
// described.
int16_t last_at_z();

// The pass has ended: what at() remembered about it goes. pass_ended()
// calls it.
void end_at_pass();

// --- tooltip.cpp -----------------------------------------------------------

// Which sub_id() of its control a tooltip's box is: the same every frame, so
// its size is remembered (bounds_of_id()) and the next frame can keep it on
// the screen.
inline constexpr uint32_t TOOLTIP_SUB = 0x20000u;

// Where a tooltip goes, given where its control is and how big the tooltip
// was: on `side` of it, `gap` pixels away -- or on the opposite side, when
// `side` would put it past the edge of the screen and the opposite would
// not; then moved along that side, as little as it takes, to be wholly on
// the screen, or against its left or top edge when it is bigger than the
// screen. TOP_LEFT and TOP_RIGHT are above the control, lined up with its
// left or its right edge; CENTER_LEFT and CENTER_RIGHT beside it; CENTER on
// top of it.
struct TipPlace {
    Align side;
    Vector2 shift; // pixels, on top of where `side` puts it
};
TipPlace place_tooltip(Rectangle control, Vector2 size, Align side, Vector2 screen,
                       float gap);

// The pass has ended: a control the pointer was resting on that the pass did
// not describe is not rested on any more. pass_ended() calls it.
void end_tooltip_pass();

// --- notify.cpp ------------------------------------------------------------

// How many notices are kept: the newest, the oldest one making room.
inline constexpr int NOTICE_RING = 8;
// The longest a notice is, in characters; past that it is cut, at the end of
// one. A report's line can be a kilobyte, and the log has all of it.
inline constexpr int NOTICE_MAX_CHARS = 240;

// A notice as the next draw_notices() shows it, for the tests.
struct NoticeView {
    std::string_view text;
    ColorRole color;
    float alpha; // 1 until its fade starts, then down towards 0
    Rectangle box; // where draw_notices() last laid it out; 0 x 0 before that
};
int notice_count();
NoticeView notice_at(int index); // 0 = the oldest
void forget_notices();

// How many errors Clay has reported laying the notices out. Always 0: they
// are the framework's own layout, sized for the most the ring holds.
int notice_layout_errors();

} // namespace rmp::ui::detail
