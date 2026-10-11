#pragma once
// ---------------------------------------------------------------------------
// Private to what floats over the rest of the interface -- at() (at.cpp),
// tooltip() (tooltip.cpp) and notify() (notify.cpp) -- and their tests. The
// hooks the app, the scenes and the rest of rmp::ui call are declared with
// the others, in ../ui_internal.h and internal.h.
// ---------------------------------------------------------------------------

#include <raylib.h> // Camera2D

namespace rmp::ui::detail {

// The camera the pass being described sees the world through: what the scene
// stack last set (set_pass_camera()), the identity -- zoom 1, nothing moved
// -- when it set none.
Camera2D pass_camera();

} // namespace rmp::ui::detail
