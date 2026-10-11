// ===========================================================================
// overlay(): a frame of its own, in front of everything drawn before it.
//
// The public half is the template in rmp/ui.h; this is what it calls. The
// backdrop -- the frame redirected into a texture and blurred -- is
// backdrop.cpp's; the veil, the modality and the fallback inside an open
// frame are here.
//
// An overlay is begin()/end() with three things more:
//
//   the veil     the frame's root painted `veil`, its alpha scaled by `dim`.
//                The root covers the viewport, so the veil takes the pointer
//                everywhere (open_frame()); with nothing to paint -- dim 0 --
//                a layer that paints nothing takes it instead, because a
//                click outside the menu must not reach the game either way.
//   modality     the pass is in front of every pass described before it, and
//                from the next frame those take no input (set_pass_modal()).
//   the blur     backdrop.cpp, asked for here, after the frame is open: the
//                frame's scale is the one the blur's distance is in.
//
// Its first control takes the focus (FrameOptions::focus_first), so Enter
// presses it whatever the scene underneath reads.
//
// Called while a frame is open, it is the game's mistake -- an overlay is a
// frame, not something in one -- and says so once. Its content still shows:
// a layer over the whole viewport, floating in front of the open frame, with
// the veil and without the blur (a blur needs the frame to start with it).
// The layer takes the pointer from what that frame declared outside it, and
// the frame closes where it was meant to.
// ===========================================================================

#include "internal.h"
#include "../internal.h"

#include <array>
#include <cmath>

namespace rmp::ui::detail {

namespace {

// What each overlay open right now opened: a frame of its own, or a layer in
// a frame somebody else opened -- the first of which, the one in front of the
// frame's own content, is the one that takes the pointer from it. Overlays
// only nest inside an open frame, so this is a stack, innermost last; past its
// depth they are counted, and the ones past it are always layers.
enum class Opened { FRAME, FRONT_LAYER, LAYER };
constexpr int MAX_DEPTH = 8;
struct {
    std::array<Opened, MAX_DEPTH> opened{};
    int depth = 0;
    int overflow = 0;
    int layers = 0; // open now, wherever they are in the stack
} overlays;

// What a layer that paints nothing hands Clay so that it is drawn at all: a
// CUSTOM command, which render.cpp draws as nothing and which takes the
// pointer like every painted command (claim_pointer_over_painted()). Clay
// wants something to point at; this is it, and nothing reads it.
struct {
    char tag = 0;
} catcher;

// Where the content sits, as FrameOptions::placement means it.
Clay_ChildAlignment alignment_of(const Align a) {
    switch (a) {
        case Align::TOP_LEFT:
            return Clay_ChildAlignment{ CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP };
        case Align::TOP_CENTER:
            return Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_TOP };
        case Align::TOP_RIGHT:
            return Clay_ChildAlignment{ CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_TOP };
        case Align::CENTER_LEFT:
            return Clay_ChildAlignment{ CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER };
        case Align::CENTER:
            break;
        case Align::CENTER_RIGHT:
            return Clay_ChildAlignment{ CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_CENTER };
        case Align::BOTTOM_LEFT:
            return Clay_ChildAlignment{ CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_BOTTOM };
        case Align::BOTTOM_CENTER:
            return Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_BOTTOM };
        case Align::BOTTOM_RIGHT:
            return Clay_ChildAlignment{ CLAY_ALIGN_X_RIGHT, CLAY_ALIGN_Y_BOTTOM };
    }
    return Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
}

Clay_SizingAxis fixed(const float pixels) {
    Clay_SizingAxis a{};
    a.type = CLAY__SIZING_TYPE_FIXED;
    a.size.minMax = Clay_SizingMinMax{ pixels, pixels };
    return a;
}

Clay_SizingAxis fit() {
    Clay_SizingAxis a{};
    a.type = CLAY__SIZING_TYPE_FIT;
    return a;
}

// How far what is underneath goes towards the veil, 0..1. Anything else is
// the game's mistake, said once, and made into the nearest thing that is not.
float checked_dim(const float dim) {
    if (dim >= 0.0f && dim <= 1.0f) return dim;
    const float used =
        std::isnan(dim) ? OverlayOptions{}.dim : (dim < 0.0f ? 0.0f : 1.0f);
    RMP_REPORT_ONCE("UI: overlay() dim = %g is not between 0 and 1; %g is used instead",
                    static_cast<double>(dim), static_cast<double>(used));
    return used;
}

// A distance, in design units, or 0 for none.
float checked_blur(const float blur) {
    if (blur >= 0.0f && std::isfinite(blur)) return blur;
    RMP_REPORT_ONCE("UI: overlay() blur = %g is not a distance; drawn without the blur",
                    static_cast<double>(blur));
    return 0.0f;
}

// A floating element over the whole viewport, in front of the frame it is
// declared in, painted `veil` -- and taking the pointer even where `veil`
// paints nothing. Its content goes into a column it opens, centred by
// `placement`; close it with two closes. `id` names it, so that the frame's
// own controls under it can be blocked by it (block_pointer()).
void open_layer_over_viewport(const Color veil, const Align placement,
                              const Clay_ElementId id) {
    const Theme &t = current_theme();
    const Clay_Dimensions v = viewport();
    Clay_ElementDeclaration d{};
    d.layout.sizing.width = fixed(v.width);
    d.layout.sizing.height = fixed(v.height);
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    d.layout.childAlignment = alignment_of(placement);
    // The frame's own padding, as open_frame() gives its root: the theme's,
    // the safe area and the letterbox bars, side by side.
    d.layout.padding = root_padding(px(t.panel_padding));
    d.floating.attachTo = CLAY_ATTACH_TO_ROOT;
    d.floating.zIndex = next_layer_z();
    d.floating.attachPoints = Clay_FloatingAttachPoints{ CLAY_ATTACH_POINT_LEFT_TOP,
                                                         CLAY_ATTACH_POINT_LEFT_TOP };
    if (veil.a > 0) d.backgroundColor = to_clay(veil);
    d.custom.customData = &catcher.tag;
    Clay__OpenElementWithId(id);
    Clay__ConfigureOpenElement(d);

    // The content, a column that fits it and lines its children up, as the
    // frame's own does: what makes the buttons of a menu one width.
    Clay_ElementDeclaration content{};
    content.layout.sizing.width = fit();
    content.layout.sizing.height = fit();
    content.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    content.layout.childGap = px16(t.gap);
    content.layout.childAlignment =
        Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
    Clay__OpenElement();
    Clay__ConfigureOpenElement(content);
}

// The veil, with nothing to paint: a layer that paints nothing and takes the
// pointer all the same. Empty, so it closes at once.
void catch_pointer_over_viewport() {
    const Clay_Dimensions v = viewport();
    Clay_ElementDeclaration d{};
    d.layout.sizing.width = fixed(v.width);
    d.layout.sizing.height = fixed(v.height);
    d.floating.attachTo = CLAY_ATTACH_TO_ROOT;
    d.floating.zIndex = next_layer_z();
    // Not for Clay, which decides by what is under the pointer which scroll
    // area a drag or the wheel moves: a floating layer that captures it hides
    // every area of the overlay's own content from Clay. The game's input is
    // ours to take, and claim_pointer_over_painted() takes it from this
    // command whatever Clay does.
    d.floating.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    d.custom.customData = &catcher.tag;
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    Clay__CloseElement();
}

} // namespace

void open_overlay(const OverlayOptions &o) {
    const float dim = checked_dim(o.dim);
    const float blur = checked_blur(o.blur);
    Color veil = o.veil;
    veil.a = static_cast<unsigned char>(std::lround(dim * static_cast<float>(o.veil.a)));

    Opened opened = Opened::FRAME;
    if (frame_open()) {
        RMP_REPORT_ONCE(
            "UI: overlay() inside begin()/end() -- an overlay is a frame of its "
            "own: call it after end(), or from the _draw() of a scene of its "
            "own. Shown as a layer of the open frame, without the blur");
        const Clay_ElementId id = element_id("rmp::ui::overlay", {});
        open_layer_over_viewport(veil, o.placement, id);
        opened = Opened::LAYER;
        if (overlays.layers == 0) {
            // In front of the frame's own controls, for the pointer as for
            // the eye: what the frame declared outside the layer cannot be
            // hovered through it, and what is inside it -- layers in it
            // included -- can.
            block_pointer(id);
            push_clip(id);
            opened = Opened::FRONT_LAYER;
        }
        ++overlays.layers;
        set_pass_modal();
    } else {
        open_frame(FrameOptions{ .placement = o.placement, .focus_first = true }, veil);
        if (frame_open()) {
            if (veil.a == 0) catch_pointer_over_viewport();
            set_pass_modal();
            blur_backdrop(blur);
        }
    }
    if (overlays.depth < MAX_DEPTH) {
        overlays.opened[static_cast<size_t>(overlays.depth++)] = opened;
    } else {
        ++overlays.overflow;
    }
}

void close_overlay() {
    // What this one opened, off the stack first, whatever happens next.
    Opened opened = Opened::LAYER; // past the stack's depth: a layer
    if (overlays.overflow > 0) {
        --overlays.overflow;
    } else if (overlays.depth > 0) {
        opened = overlays.opened[static_cast<size_t>(--overlays.depth)];
    } else {
        return;
    }
    if (opened != Opened::FRAME && overlays.layers > 0) --overlays.layers;
    // Its frame never opened -- the UI could not start -- or its body called
    // end() itself: nothing is left to close.
    if (!frame_open()) return;
    if (opened == Opened::FRAME) {
        end();
        return;
    }
    if (opened == Opened::FRONT_LAYER) pop_clip();
    Clay__CloseElement(); // the content column
    Clay__CloseElement(); // the layer
}

} // namespace rmp::ui::detail
