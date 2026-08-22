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

Clay_ChildAlignment alignment_of(Align a) {
    Clay_LayoutAlignmentX x = CLAY_ALIGN_X_CENTER;
    Clay_LayoutAlignmentY y = CLAY_ALIGN_Y_CENTER;
    switch (a) {
        case Align::TOP_LEFT:
            x = CLAY_ALIGN_X_LEFT;
            y = CLAY_ALIGN_Y_TOP;
            break;
        case Align::TOP_CENTER:
            x = CLAY_ALIGN_X_CENTER;
            y = CLAY_ALIGN_Y_TOP;
            break;
        case Align::TOP_RIGHT:
            x = CLAY_ALIGN_X_RIGHT;
            y = CLAY_ALIGN_Y_TOP;
            break;
        case Align::CENTER_LEFT:
            x = CLAY_ALIGN_X_LEFT;
            y = CLAY_ALIGN_Y_CENTER;
            break;
        case Align::CENTER:
            x = CLAY_ALIGN_X_CENTER;
            y = CLAY_ALIGN_Y_CENTER;
            break;
        case Align::CENTER_RIGHT:
            x = CLAY_ALIGN_X_RIGHT;
            y = CLAY_ALIGN_Y_CENTER;
            break;
        case Align::BOTTOM_LEFT:
            x = CLAY_ALIGN_X_LEFT;
            y = CLAY_ALIGN_Y_BOTTOM;
            break;
        case Align::BOTTOM_CENTER:
            x = CLAY_ALIGN_X_CENTER;
            y = CLAY_ALIGN_Y_BOTTOM;
            break;
        case Align::BOTTOM_RIGHT:
            x = CLAY_ALIGN_X_RIGHT;
            y = CLAY_ALIGN_Y_BOTTOM;
            break;
    }
    return Clay_ChildAlignment{ x, y };
}

Clay_Padding pad(float p) {
    auto v = static_cast<uint16_t>(px(p));
    return Clay_Padding{ v, v, v, v };
}

Clay_LayoutConfig layout_of(const BoxOptions &o, Clay_LayoutDirection dir,
                            float default_padding) {
    const Theme &t = current_theme();
    Clay_LayoutConfig l{};
    l.sizing.width = axis(o.grow_x, o.width);
    l.sizing.height = axis(o.grow_y, o.height);
    l.padding = pad(o.padding < 0 ? default_padding : o.padding);
    l.childGap = static_cast<uint16_t>(px(o.gap < 0 ? t.gap : o.gap));
    l.childAlignment = alignment_of(o.items);
    l.layoutDirection = dir;
    return l;
}

bool transparent(Color c) { return c.a == 0 && c.r == 0 && c.g == 0 && c.b == 0; }

} // namespace

namespace {

// Grids nest rarely but they must not corrupt each other when they do, so this
// is a small stack rather than one variable.
struct GridFrame {
    int columns = 4;
    int index = 0; // cells emitted so far
    bool row_open = false;
    float gap = 0;
};
constexpr int kMaxGridDepth = 4;
GridFrame g_grids[kMaxGridDepth];
int g_grid_depth = 0;

// Named containers get a stable id so they can be asked about later; unnamed
// ones stay anonymous, which is what most of them should be.
void open_with_id(const char *id, const Clay_ElementDeclaration &d) {
    if (id != nullptr)
        Clay__OpenElementWithId(detail::element_id(std::string_view{ id }, id));
    else
        Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
}

// A row inside a grid: full width, one line of cells.
void open_grid_row(float gap) {
    Clay_ElementDeclaration d{};
    d.layout.sizing.width = axis(true, 0);
    d.layout.sizing.height = axis(false, 0);
    d.layout.childGap = static_cast<uint16_t>(px(gap));
    d.layout.layoutDirection = CLAY_LEFT_TO_RIGHT;
    d.layout.childAlignment = Clay_ChildAlignment{ CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP };
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
}

} // namespace

namespace detail {

void open_row(const BoxOptions &o) {
    if (!frame_open()) return;
    Clay_ElementDeclaration d{};
    d.layout = layout_of(o, CLAY_LEFT_TO_RIGHT, 0);
    open_with_id(o.id, d);
}

void open_column(const BoxOptions &o) {
    if (!frame_open()) return;
    Clay_ElementDeclaration d{};
    d.layout = layout_of(o, CLAY_TOP_TO_BOTTOM, 0);
    open_with_id(o.id, d);
}

void open_panel(const PanelOptions &o) {
    if (!frame_open()) return;
    const Theme &t = current_theme();

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
    } else if (t.border_width > 0.0f) {
        // No border asked for, but the theme draws them. A light Theme has no
        // shadows to separate a white panel from a white page, so it says so
        // once — here — instead of every call site having to remember.
        auto w = static_cast<uint16_t>(px(t.border_width));
        if (w < 1) w = 1;
        d.border.color = to_clay(t.border);
        d.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
    }

    open_with_id(o.box.id, d);
}

void open_center() {
    if (!frame_open()) return;
    Clay_ElementDeclaration d{};
    // Grows to fill whatever it was given, then centres its contents inside it.
    // That is all "centre this" needs to mean.
    d.layout.sizing.width = axis(true, 0);
    d.layout.sizing.height = axis(true, 0);
    d.layout.childAlignment =
        Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
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
    d.layout.sizing.width = axis(true, 0);
    d.layout.sizing.height = axis(true, 0);
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
}

void open_layer() {
    if (!frame_open()) return;
    Clay_ElementDeclaration d{};
    d.layout.sizing.width = axis(true, 0);
    d.layout.sizing.height = axis(true, 0);
    d.layout.childAlignment =
        Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    // Floating attached to the parent: it fills the stack and takes no space in
    // it, so every layer lands in the same box. zIndex follows declaration
    // order, so the last layer() written is the one on top — which is the
    // order a person reading the code expects.
    d.floating.attachTo = CLAY_ATTACH_TO_PARENT;
    d.floating.zIndex = next_layer_z();
    d.floating.attachPoints = Clay_FloatingAttachPoints{ CLAY_ATTACH_POINT_LEFT_TOP,
                                                         CLAY_ATTACH_POINT_LEFT_TOP };
    d.floating.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
}

void open_grid(const GridOptions &o) {
    if (!frame_open()) return;
    const Theme &t = current_theme();

    Clay_ElementDeclaration d{};
    d.layout.sizing.width = axis(o.grow_x, 0);
    d.layout.sizing.height = axis(o.grow_y, 0);
    d.layout.padding = pad(o.padding < 0 ? 0 : o.padding);
    d.layout.childGap = static_cast<uint16_t>(px(o.gap < 0 ? t.gap : o.gap));
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    d.layout.childAlignment = Clay_ChildAlignment{ CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP };

    // Clay has no grid, and it does not need one: a left-to-right container
    // that wraps IS a grid once the children are told to be equal width. The
    // number of columns is the only thing we have to decide.
    //
    // With columns = 0 we work it out from the space available and the minimum
    // cell Size, which is what makes an inventory reflow on a phone in portrait
    // instead of staying stubbornly at the number of columns someone typed on a
    // desktop. It uses last frame's width for the same reason everything else
    // does — it is the only width that exists yet.
    // Named or not, a grid gets an id, because working out the columns means
    // knowing how wide it was last frame.
    Clay_ElementId grid_id = o.id != nullptr
        ? element_id(std::string_view{ o.id }, o.id)
        : element_id(std::string_view{ "grid" }, nullptr);

    int columns = o.columns;
    if (columns <= 0) {
        Clay_BoundingBox box{};
        if (bounds_of_id(grid_id, &box) && box.width > 0) {
            float cell = px(o.min_cell) + px(o.gap < 0 ? t.gap : o.gap);
            columns = static_cast<int>(box.width / (cell > 1 ? cell : 1));
        }
    }
    if (columns <= 0) columns = 4;

    if (g_grid_depth < kMaxGridDepth) {
        g_grids[g_grid_depth] = GridFrame{ columns, 0, false, o.gap < 0 ? t.gap : o.gap };
        g_grid_depth++;
    }
    Clay__OpenElementWithId(grid_id);
    Clay__ConfigureOpenElement(d);
}

void close_grid() {
    if (!frame_open()) return;
    if (g_grid_depth > 0) {
        // A grid whose last row is not full still has that row open. Closing it
        // here is why a grid of five items with four columns does not corrupt
        // everything after it.
        if (g_grids[g_grid_depth - 1].row_open) Clay__CloseElement();
        g_grid_depth--;
    }
    Clay__CloseElement();
}

void open_cell() {
    if (!frame_open()) return;
    if (g_grid_depth == 0) {
        // A cell outside a grid is a plain box rather than an error: it keeps
        // the tree balanced, and the mistake is visible on screen instead of
        // corrupting the frame.
        Clay_ElementDeclaration d{};
        d.layout.sizing.width = axis(false, 0);
        d.layout.sizing.height = axis(false, 0);
        Clay__OpenElement();
        Clay__ConfigureOpenElement(d);
        return;
    }

    GridFrame &g = g_grids[g_grid_depth - 1];
    if (g.index % g.columns == 0) {
        if (g.row_open) Clay__CloseElement();
        open_grid_row(g.gap);
        g.row_open = true;
    }

    Clay_ElementDeclaration d{};
    // A percentage of the row, so every cell is the same width whatever the
    // window is doing. Clay takes the percentage of the parent minus its
    // padding and gaps, which is exactly the space the cells actually share.
    Clay_SizingAxis w{};
    w.type = CLAY__SIZING_TYPE_PERCENT;
    w.size.percent = 1.0f / static_cast<float>(g.columns);
    d.layout.sizing.width = w;
    d.layout.sizing.height = axis(false, 0);
    d.layout.childAlignment =
        Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    g.index++;
}

void close_cell() {
    if (!frame_open()) return;
    Clay__CloseElement();
}

void open_scroll(const ScrollOptions &o) {
    if (!frame_open()) return;
    const Theme &t = current_theme();

    Clay_ElementDeclaration d{};
    d.layout.sizing.width = axis(o.grow_x, o.width);
    d.layout.sizing.height = axis(o.grow_y, o.height);
    d.layout.padding = pad(o.padding < 0 ? 0 : o.padding);
    d.layout.childGap = static_cast<uint16_t>(px(o.gap < 0 ? t.gap : o.gap));
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;

    // clip + childOffset is the whole of scrolling: Clay moves the children by
    // the offset it is tracking, and the clip keeps the ones outside from being
    // drawn. Clay_UpdateScrollContainers, called in begin(), is what advances
    // that offset from the wheel and from dragging.
    d.clip.horizontal = o.horizontal;
    d.clip.vertical = o.vertical;
    d.clip.childOffset = Clay_GetScrollOffset();

    open_with_id(o.id, d);
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
    d.layout.sizing.width = axis(true, 0);
    d.layout.sizing.height = axis(true, 0);
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    Clay__CloseElement();
}

void spacer(float fixed) {
    if (!detail::frame_open()) return;
    Clay_ElementDeclaration d{};
    d.layout.sizing.width = axis(false, fixed);
    d.layout.sizing.height = axis(false, fixed);
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    Clay__CloseElement();
}

void image(const Texture2D &texture) { image(texture, ImageOptions{}); }

void image(const Texture2D &texture, const ImageOptions &o) {
    if (!detail::frame_open()) return;

    Clay_ElementDeclaration d{};
    if (o.grow) {
        d.layout.sizing.width = axis(true, 0);
        d.layout.sizing.height = axis(true, 0);
    } else {
        // Its own pixel Size by default, scaled like everything else, so an
        // icon does not shrink into nothing on a big screen.
        float w = o.width > 0 ? o.width : static_cast<float>(texture.width);
        float h = o.height > 0 ? o.height : static_cast<float>(texture.height);
        d.layout.sizing.width = axis(false, w);
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

void progress(float fraction) { progress(fraction, ProgressOptions{}); }

void progress(float fraction, const ProgressOptions &o) {
    if (!detail::frame_open()) return;
    const Theme &t = current_theme();

    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    float h = o.height < 0 ? t.font_size * 0.6f : o.height;
    float r = o.radius < 0 ? h * 0.5f : o.radius;

    Clay_ElementDeclaration track{};
    track.layout.sizing.width = axis(o.width <= 0, o.width);
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
        bar.layout.sizing.width = w;
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
