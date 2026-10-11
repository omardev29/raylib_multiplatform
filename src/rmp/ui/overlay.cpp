// ===========================================================================
// overlay(): a frame of its own, in front of everything drawn before it.
//
// The public half is the template in rmp/ui.h; this is what it calls. The
// backdrop -- the frame redirected into a texture and blurred -- is
// backdrop.cpp's; the veil, the modality and the fallback inside an open
// frame are here.
//
// As it stands an overlay is begin()/end() with its placement and its first
// control focused, and no more: no veil, no blur, nothing made inert behind
// it. Called while a frame is open, its content goes into that frame as a
// plain column -- so the frame it is in still closes where it was meant to.
// ===========================================================================

#include "internal.h"

#include <array>

namespace rmp::ui::detail {

namespace {

// What each overlay open right now opened: a frame of its own, or a column in
// a frame somebody else opened. Overlays only nest inside an open frame, so
// this is a stack, innermost last; past its depth they are counted, and the
// ones past it are always columns.
constexpr int MAX_DEPTH = 8;
struct {
    std::array<bool, MAX_DEPTH> opened_frame{};
    int depth = 0;
    int overflow = 0;
} overlays;

} // namespace

void open_overlay(const OverlayOptions &o) {
    const bool nested = frame_open();
    if (nested) {
        open_column(BoxOptions{});
    } else {
        open_frame(FrameOptions{ .placement = o.placement, .focus_first = true },
                   Color{ 0, 0, 0, 0 });
    }
    if (overlays.depth < MAX_DEPTH) {
        overlays.opened_frame[overlays.depth++] = !nested;
    } else {
        ++overlays.overflow;
    }
}

void close_overlay() {
    if (overlays.overflow > 0) {
        --overlays.overflow;
        close_element();
        return;
    }
    if (overlays.depth == 0) return;
    if (overlays.opened_frame[--overlays.depth]) {
        end();
    } else {
        close_element();
    }
}

} // namespace rmp::ui::detail
