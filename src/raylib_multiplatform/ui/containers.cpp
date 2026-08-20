// ===========================================================================
// Containers, and the two widgets that need one: image and progress.
//
// The public halves are templates in the header — they have to be, to take the
// body as a lambda — so everything here is the non-template half they call.
// That split is what keeps Clay out of include/.
// ===========================================================================

#include "internal.h"

namespace rmp::ui {

namespace {

using detail::px;
using detail::to_clay;

Clay_SizingAxis axis(bool grow, float fixed) {
    Clay_SizingAxis a{};
    if (fixed > 0.0f) {
        a.type = CLAY__SIZING_TYPE_FIXED;
        a.size.minMax = Clay_SizingMinMax{ px(fixed), px(fixed) };
    } else if (grow) {
        a.type = CLAY__SIZING_TYPE_GROW;
        a.size.minMax = Clay_SizingMinMax{ 0, 0 };
    } else {
        a.type = CLAY__SIZING_TYPE_FIT;
        a.size.minMax = Clay_SizingMinMax{ 0, 0 };
    }
    return a;
}

Clay_ChildAlignment alignment_of(align a) {
    Clay_LayoutAlignmentX x = CLAY_ALIGN_X_CENTER;
    Clay_LayoutAlignmentY y = CLAY_ALIGN_Y_CENTER;
    switch (a) {
        case align::top_left:      x = CLAY_ALIGN_X_LEFT;   y = CLAY_ALIGN_Y_TOP;    break;
        case align::top_center:    x = CLAY_ALIGN_X_CENTER; y = CLAY_ALIGN_Y_TOP;    break;
        case align::top_right:     x = CLAY_ALIGN_X_RIGHT;  y = CLAY_ALIGN_Y_TOP;    break;
        case align::center_left:   x = CLAY_ALIGN_X_LEFT;   y = CLAY_ALIGN_Y_CENTER; break;
        case align::center:        x = CLAY_ALIGN_X_CENTER; y = CLAY_ALIGN_Y_CENTER; break;
        case align::center_right:  x = CLAY_ALIGN_X_RIGHT;  y = CLAY_ALIGN_Y_CENTER; break;
        case align::bottom_left:   x = CLAY_ALIGN_X_LEFT;   y = CLAY_ALIGN_Y_BOTTOM; break;
        case align::bottom_center: x = CLAY_ALIGN_X_CENTER; y = CLAY_ALIGN_Y_BOTTOM; break;
        case align::bottom_right:  x = CLAY_ALIGN_X_RIGHT;  y = CLAY_ALIGN_Y_BOTTOM; break;
    }
    return Clay_ChildAlignment{ x, y };
}

Clay_Padding pad(float p) {
    auto v = static_cast<uint16_t>(px(p));
    return Clay_Padding{ v, v, v, v };
}

Clay_LayoutConfig layout_of(const box_options &o, Clay_LayoutDirection dir, float default_padding) {
    const theme &t = current_theme();
    Clay_LayoutConfig l{};
    l.sizing.width   = axis(o.grow_x, o.width);
    l.sizing.height  = axis(o.grow_y, o.height);
    l.padding        = pad(o.padding < 0 ? default_padding : o.padding);
    l.childGap       = static_cast<uint16_t>(px(o.gap < 0 ? t.gap : o.gap));
    l.childAlignment = alignment_of(o.items);
    l.layoutDirection = dir;
    return l;
}

bool transparent(Color c) { return c.a == 0 && c.r == 0 && c.g == 0 && c.b == 0; }

} // namespace

namespace {

// Named containers get a stable id so they can be asked about later; unnamed
// ones stay anonymous, which is what most of them should be.
void open_with_id(const char *id, const Clay_ElementDeclaration &d) {
    if (id != nullptr) Clay__OpenElementWithId(detail::element_id(std::string_view{id}, id));
    else               Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
}

} // namespace

namespace detail {

void open_row(const box_options &o) {
    if (!frame_open()) return;
    Clay_ElementDeclaration d{};
    d.layout = layout_of(o, CLAY_LEFT_TO_RIGHT, 0);
    open_with_id(o.id, d);
}

void open_column(const box_options &o) {
    if (!frame_open()) return;
    Clay_ElementDeclaration d{};
    d.layout = layout_of(o, CLAY_TOP_TO_BOTTOM, 0);
    open_with_id(o.id, d);
}

void open_panel(const panel_options &o) {
    if (!frame_open()) return;
    const theme &t = current_theme();

    Clay_ElementDeclaration d{};
    // A panel pads itself by default; a bare row or column does not. That is
    // the difference between the two, along with having a background.
    d.layout = layout_of(o.box, CLAY_TOP_TO_BOTTOM,
                         o.box.padding < 0 ? t.panel_padding : o.box.padding);
    d.backgroundColor = to_clay(transparent(o.background) ? t.panel : o.background);

    float r = px(o.radius < 0 ? t.corner_radius : o.radius);
    d.cornerRadius = Clay_CornerRadius{ r, r, r, r };

    if (!transparent(o.border)) {
        auto w = static_cast<uint16_t>(px(o.border_width < 0 ? 1.0f : o.border_width));
        d.border.color = to_clay(o.border);
        d.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
    }

    open_with_id(o.box.id, d);
}

void open_center() {
    if (!frame_open()) return;
    Clay_ElementDeclaration d{};
    // Grows to fill whatever it was given, then centres its contents inside it.
    // That is all "centre this" needs to mean.
    d.layout.sizing.width   = axis(true, 0);
    d.layout.sizing.height  = axis(true, 0);
    d.layout.childAlignment = Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
}

void open_stack() {
    if (!frame_open()) return;
    // The stack itself is an ordinary box. The layering happens in its
    // children, which is why each one has to be a layer(): Clay stacks by
    // detaching an element from the flow, and there is no way to guess which
    // of a lambda's contents were meant to overlap.
    Clay_ElementDeclaration d{};
    d.layout.sizing.width   = axis(true, 0);
    d.layout.sizing.height  = axis(true, 0);
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
}

void open_layer() {
    if (!frame_open()) return;
    Clay_ElementDeclaration d{};
    d.layout.sizing.width   = axis(true, 0);
    d.layout.sizing.height  = axis(true, 0);
    d.layout.childAlignment = Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    // Floating attached to the parent: it fills the stack and takes no space in
    // it, so every layer lands in the same box. zIndex follows declaration
    // order, so the last layer() written is the one on top — which is the
    // order a person reading the code expects.
    d.floating.attachTo = CLAY_ATTACH_TO_PARENT;
    d.floating.zIndex   = next_layer_z();
    d.floating.attachPoints = Clay_FloatingAttachPoints{ CLAY_ATTACH_POINT_LEFT_TOP,
                                                         CLAY_ATTACH_POINT_LEFT_TOP };
    d.floating.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
}

void close_element() {
    if (!frame_open()) return;
    Clay__CloseElement();
}

} // namespace detail

// ---------------------------------------------------------------------------
// Leaf widgets that needed the layout vocabulary above
// ---------------------------------------------------------------------------

void spacer() {
    if (!detail::frame_open()) return;
    Clay_ElementDeclaration d{};
    d.layout.sizing.width  = axis(true, 0);
    d.layout.sizing.height = axis(true, 0);
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    Clay__CloseElement();
}

void spacer(float fixed) {
    if (!detail::frame_open()) return;
    Clay_ElementDeclaration d{};
    d.layout.sizing.width  = axis(false, fixed);
    d.layout.sizing.height = axis(false, fixed);
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    Clay__CloseElement();
}

void image(const Texture2D &texture) { image(texture, image_options{}); }

void image(const Texture2D &texture, const image_options &o) {
    if (!detail::frame_open()) return;

    Clay_ElementDeclaration d{};
    if (o.grow) {
        d.layout.sizing.width  = axis(true, 0);
        d.layout.sizing.height = axis(true, 0);
    } else {
        // Its own pixel size by default, scaled like everything else, so an
        // icon does not shrink into nothing on a big screen.
        float w = o.width  > 0 ? o.width  : static_cast<float>(texture.width);
        float h = o.height > 0 ? o.height : static_cast<float>(texture.height);
        d.layout.sizing.width  = axis(false, w);
        d.layout.sizing.height = axis(false, h);
    }
    // Clay passes this pointer through to the render command untouched, which
    // is why the texture has to outlive the frame.
    d.image.imageData = const_cast<Texture2D *>(&texture);

    // The tint deliberately does NOT go in backgroundColor. Clay emits a
    // RECTANGLE for any element with a background, *in addition to* the IMAGE
    // and after it in the list — so a tint set that way is a coloured square
    // painted straight over the picture. Cost me a screenshot to find.
    //
    // It travels in userData instead, which Clay hands to the renderer
    // untouched. Null means untinted, so anyone driving Clay by hand gets the
    // obvious behaviour without knowing this exists.
    if (o.tint.r != 255 || o.tint.g != 255 || o.tint.b != 255 || o.tint.a != 255) {
        if (void *mem = detail::frame_alloc(sizeof(Color))) {
            *static_cast<Color *>(mem) = o.tint;
            d.userData = mem;
        }
    }

    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    Clay__CloseElement();
}

void progress(float fraction) { progress(fraction, progress_options{}); }

void progress(float fraction, const progress_options &o) {
    if (!detail::frame_open()) return;
    const theme &t = current_theme();

    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    float h = o.height < 0 ? t.font_size * 0.6f : o.height;
    float r = o.radius < 0 ? h * 0.5f : o.radius;

    Clay_ElementDeclaration track{};
    track.layout.sizing.width  = axis(o.width <= 0, o.width);
    track.layout.sizing.height = axis(false, h);
    track.backgroundColor = to_clay(transparent(o.track) ? t.surface : o.track);
    track.cornerRadius = Clay_CornerRadius{ px(r), px(r), px(r), px(r) };

    open_with_id(o.id, track);
    {
        // A percentage child rather than a fixed width, so the bar is right
        // whatever the track ends up measuring — including inside a grow.
        Clay_ElementDeclaration bar{};
        Clay_SizingAxis w{};
        w.type = CLAY__SIZING_TYPE_PERCENT;
        w.size.percent = fraction;
        bar.layout.sizing.width  = w;
        bar.layout.sizing.height = axis(true, 0);
        bar.backgroundColor = to_clay(transparent(o.fill) ? t.primary : o.fill);
        bar.cornerRadius = Clay_CornerRadius{ px(r), px(r), px(r), px(r) };
        Clay__OpenElement();
        Clay__ConfigureOpenElement(bar);
        Clay__CloseElement();
    }
    Clay__CloseElement();
}

} // namespace rmp::ui
