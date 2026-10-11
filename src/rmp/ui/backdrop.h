#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/ui/backdrop.cpp and its tests: what overlay()'s blur is
// made of, for a test to look at. The hooks the app and overlay() call are
// declared with the rest, in ../ui_internal.h and internal.h.
// ---------------------------------------------------------------------------

namespace rmp::ui::detail {

// Is this frame being drawn into the backdrop's texture -- because the last
// frame's overlay() asked for a blur -- rather than straight onto the screen?
// Never without a window.
bool backdrop_redirected();

} // namespace rmp::ui::detail
