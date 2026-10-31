// ===========================================================================
// Containers, and the two widgets that need one: image and progress.
//
// The public halves are templates in the header — they have to be, to take the
// body as a lambda — so everything here is the non-template half they call.
// That split is what keeps Clay out of include/.
// ===========================================================================

#include "internal.h"
#include "../internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace rmp::ui {

namespace {

using detail::px;
using detail::px16;

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
    auto v = px16(p);
    return Clay_Padding{ v, v, v, v };
}

Clay_LayoutConfig layout_of(const BoxOptions &o, Clay_LayoutDirection dir,
                            float default_padding) {
    const Theme &t = current_theme();
    Clay_LayoutConfig l{};
    l.sizing.width = axis(o.grow_x, o.width);
    l.sizing.height = axis(o.grow_y, o.height);
    l.padding = pad(o.padding < 0 ? default_padding : o.padding);
    l.childGap = px16(o.gap < 0 ? t.gap : o.gap);
    l.childAlignment = alignment_of(o.items);
    l.layoutDirection = dir;
    return l;
}

bool transparent(Color c) { return c.a == 0 && c.r == 0 && c.g == 0 && c.b == 0; }

// See detail::last_image_data(): what image() handed Clay, so a test can prove
// it is not the caller's address.
struct {
    const void *data = nullptr;
} last_image;

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
constexpr int MAX_GRID_DEPTH = 4;
struct {
    std::array<GridFrame, MAX_GRID_DEPTH> frames;
    int depth = 0;
    // Grids opened past the limit. They push no frame, so their close must not
    // pop one either: it used to, which meant the fifth grid's close consumed
    // the FOURTH grid's row and every close after it was off by one. Counting
    // them is what makes open and close pairs again, and it is exact because
    // grids nest.
    int overflow = 0;
} grids;

// The containers of this pass that lay out left to right -- a row, a cell, a
// grid's row -- by the id Clay gave them, so that spacer(n) can ask whether
// the one it is in is one: Clay_GetOpenElementId() is the container open
// right now, whoever opened it. Every other container is a column. Kept for
// the pass, so an id can only be one of these if it was one when declared.
struct {
    std::vector<uint32_t> ids;
} rows;

void note_row() { rows.ids.push_back(Clay_GetOpenElementId()); }

bool in_row() {
    return std::ranges::find(rows.ids, Clay_GetOpenElementId()) != rows.ids.end();
}

// Named containers get a stable id, the same whatever else is on screen;
// unnamed ones stay anonymous, which is what most of them should be.
void open_with_id(std::string_view id, const Clay_ElementDeclaration &d) {
    if (!id.empty())
        Clay__OpenElementWithId(detail::element_id(id, id));
    else
        Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
}

// A row inside a grid: full width, one line of cells.
void open_grid_row(float gap) {
    Clay_ElementDeclaration d{};
    d.layout.sizing.width = axis(true, 0);
    d.layout.sizing.height = axis(false, 0);
    d.layout.childGap = px16(gap);
    d.layout.layoutDirection = CLAY_LEFT_TO_RIGHT;
    d.layout.childAlignment = Clay_ChildAlignment{ CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_TOP };
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    note_row();
}

} // namespace

namespace detail {

const void *last_image_data() { return last_image.data; }

void begin_pass_containers() {
    grids.depth = 0;
    grids.overflow = 0;
    rows.ids.clear();
}

void open_row(const BoxOptions &o) {
    if (!frame_ready("row", o.id)) return;
    Clay_ElementDeclaration d{};
    d.layout = layout_of(o, CLAY_LEFT_TO_RIGHT, 0);
    open_with_id(o.id, d);
    note_row();
}

void open_column(const BoxOptions &o) {
    if (!frame_ready("column", o.id)) return;
    Clay_ElementDeclaration d{};
    d.layout = layout_of(o, CLAY_TOP_TO_BOTTOM, 0);
    open_with_id(o.id, d);
}

void open_panel(const PanelOptions &o) { open_panel_with(o, {}, {}, {}, false, false); }

void open_panel_with(const PanelOptions &o, rmp::Ref<const Style> own,
                     Clay_ElementId kept, rmp::Ref<const Interaction> now, bool row,
                     bool bare) {
    if (!frame_ready("panel", o.box.id)) return;
    const LookScope scope(WidgetKind::PANEL, own);
    const Theme &t = current_theme();
    const Style &look = detail::look();

    Clay_ElementDeclaration d{};
    // A panel pads itself by default; a bare row or column does not. That is
    // the difference between the two, along with having a background.
    d.layout =
        layout_of(o.box, row ? CLAY_LEFT_TO_RIGHT : CLAY_TOP_TO_BOTTOM,
                  o.box.padding < 0 ? (bare ? 0.0f : t.panel_padding) : o.box.padding);
    if (o.box.gap < 0 && look.gap) d.layout.childGap = px16(*look.gap);
    d.backgroundColor = to_clay(transparent(o.background) ? t.panel : o.background);
    if (bare && !look.background) d.backgroundColor = Clay_Color{ 0, 0, 0, 0 };

    const float r = px(o.radius < 0 ? t.corner_radius : o.radius);
    d.cornerRadius = Clay_CornerRadius{ r, r, r, r };

    if (!transparent(o.border)) {
        auto w = px16(o.border_width < 0 ? 1.0f : o.border_width);
        d.border.color = to_clay(o.border);
        d.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
    } else if (look.border && look.border_width && *look.border_width > 0) {
        auto w = px16(*look.border_width);
        if (w < 1) w = 1;
        d.border.color = to_clay(*look.border);
        d.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
    } else if (t.border_width > 0.0f && !bare) {
        // No border asked for, but the theme draws them. A light Theme has no
        // shadows to separate a white panel from a white page, so it says so
        // once — here — instead of every call site having to remember.
        auto w = px16(t.border_width);
        if (w < 1) w = 1;
        d.border.color = to_clay(t.border);
        d.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
    }

    if (now) {
        // A kept Panel: its own id, and what it is doing.
        decorate(d, kept, *now);
        Clay__OpenElementWithId(kept);
        Clay__ConfigureOpenElement(d);
        if (row) note_row();
        return;
    }
    if (look.skin || look.shadow || look.motion) {
        // Styled through Theme::styles.panel: an id to keep its motion by.
        const Clay_ElementId id =
            !o.box.id.empty() ? element_id(o.box.id, o.box.id) : element_id("panel", {});
        decorate(d, id, Interaction{});
        Clay__OpenElementWithId(id);
        Clay__ConfigureOpenElement(d);
        return;
    }
    open_with_id(o.box.id, d);
}

void open_center() {
    if (!frame_ready("center", {})) return;
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
    if (!frame_ready("stack", {})) return;
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
    if (!frame_ready("layer", {})) return;
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
    if (!frame_ready("grid", o.id)) return;
    const Theme &t = current_theme();

    Clay_ElementDeclaration d{};
    d.layout.sizing.width = axis(o.grow_x, 0);
    d.layout.sizing.height = axis(o.grow_y, 0);
    d.layout.padding = pad(o.padding < 0 ? 0 : o.padding);
    d.layout.childGap = px16(o.gap < 0 ? t.gap : o.gap);
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
    // knowing how wide it was last frame: an unnamed one is "grid" counted by
    // its order in the pass, so it is measured just the same, and only a grid
    // that comes and goes before it can make it borrow another's width.
    const Clay_ElementId grid_id =
        !o.id.empty() ? element_id(o.id, o.id) : element_id("grid", {});

    int columns = o.columns;
    if (columns <= 0) {
        Clay_BoundingBox box{};
        if (bounds_of_id(grid_id, &box) && box.width > 0) {
            const float cell = px(o.min_cell) + px(o.gap < 0 ? t.gap : o.gap);
            columns = static_cast<int>(box.width / (cell > 1 ? cell : 1));
        }
    }
    // At least one, and never more than fit: when not even one cell fits, one
    // column is the honest answer. It used to fall back to four, so the
    // narrower a grid was, the MORE columns it got. A grid with nothing
    // measured yet -- its first frame -- starts at one too, and has its real
    // count from the second.
    if (columns <= 0) columns = 1;

    if (grids.depth < MAX_GRID_DEPTH) {
        grids.frames[grids.depth] =
            GridFrame{ columns, 0, false, o.gap < 0 ? t.gap : o.gap };
        ++grids.depth;
    } else {
        ++grids.overflow;
        RMP_REPORT_ONCE("UI: grids nested more than %d deep; the ones past that lay "
                        "their cells out as plain boxes in a column",
                        MAX_GRID_DEPTH);
    }
    Clay__OpenElementWithId(grid_id);
    Clay__ConfigureOpenElement(d);
}

void close_grid() {
    if (!frame_open()) return;
    if (grids.overflow > 0) {
        // This one never pushed a frame, so it pops nothing. Grids nest, so the
        // overflowing ones are always the innermost and this is exact LIFO.
        --grids.overflow;
    } else if (grids.depth > 0) {
        // A grid whose last row is not full still has that row open. Closing it
        // here is why a grid of five items with four columns does not corrupt
        // everything after it.
        if (grids.frames[grids.depth - 1].row_open) Clay__CloseElement();
        --grids.depth;
    }
    Clay__CloseElement();
}

void open_cell() {
    if (!frame_ready("cell", {})) return;
    if (grids.depth == 0 || grids.overflow > 0) {
        // A cell outside a grid is a plain box rather than an error: it keeps
        // the tree balanced, and the mistake is visible on screen instead of
        // corrupting the frame. A cell inside a grid nested past the limit is
        // the same case — it has no frame of its own, and using the innermost
        // one that has would shuffle THAT grid's rows.
        Clay_ElementDeclaration d{};
        d.layout.sizing.width = axis(false, 0);
        d.layout.sizing.height = axis(false, 0);
        Clay__OpenElement();
        Clay__ConfigureOpenElement(d);
        note_row(); // left to right, Clay's default, as a cell in a grid is
        return;
    }

    GridFrame &g = grids.frames[grids.depth - 1];
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
    note_row(); // its direction is Clay's default: left to right
    ++g.index;
}

void close_cell() {
    if (!frame_open()) return;
    Clay__CloseElement();
}

void open_scroll(const ScrollOptions &o) {
    if (!frame_ready("scroll", o.id)) return;
    const Theme &t = current_theme();
    // Named or not, a scroll area gets an id: hit testing has to know where the
    // area is to decide whether what is inside it is on screen at all.
    const Clay_ElementId clip_id =
        !o.id.empty() ? element_id(o.id, o.id) : element_id("scroll", {});

    Clay_ElementDeclaration d{};
    d.layout.sizing.width = axis(o.grow_x, o.width);
    d.layout.sizing.height = axis(o.grow_y, o.height);
    d.layout.padding = pad(o.padding < 0 ? 0 : o.padding);
    d.layout.childGap = px16(o.gap < 0 ? t.gap : o.gap);
    d.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;

    // clip + childOffset is the whole of scrolling: Clay moves the children by
    // the offset it is tracking, and the clip keeps the ones outside from being
    // drawn. Clay_UpdateScrollContainers, called in begin(), is what advances
    // that offset from the wheel and from dragging.
    //
    // Clay_GetScrollOffset() answers for the element that is OPEN, so it is
    // asked after this one opens -- Clay's own CLAY() macro does the same. Asked
    // before, it answered for the parent: zero, and the list never moved.
    d.clip.horizontal = o.horizontal;
    d.clip.vertical = o.vertical;

    Clay__OpenElementWithId(clip_id);
    d.clip.childOffset = Clay_GetScrollOffset();
    Clay__ConfigureOpenElement(d);
    push_clip(clip_id);
}

void close_scroll() {
    if (!frame_open()) return;
    pop_clip();
    Clay__CloseElement();
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
    if (!detail::frame_ready("spacer", {})) return;
    Clay_ElementDeclaration d{};
    d.layout.sizing.width = axis(true, 0);
    d.layout.sizing.height = axis(true, 0);
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    Clay__CloseElement();
}

void spacer(float fixed) {
    if (!detail::frame_ready("spacer", {})) return;
    // A gap along the container it is in, and nothing across it: n by n made
    // a column n wide and a row n tall, whatever else was in them.
    const bool across = in_row();
    Clay_ElementDeclaration d{};
    d.layout.sizing.width = axis(false, across ? fixed : 0);
    d.layout.sizing.height = axis(false, across ? 0 : fixed);
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    Clay__CloseElement();
}

void image(const Texture2D &texture) { image(texture, ImageOptions{}); }

void image(const Texture2D &texture, const ImageOptions &o) {
    detail::image_with(Picture{}, o, detail::Extra{}, {}, texture);
}

void image(const Picture &picture) { image(picture, ImageOptions{}); }

void image(const Picture &picture, const ImageOptions &o) {
    detail::image_with(picture, o, detail::Extra{}, {});
}

void image(const rmp::Texture &texture) { image(Picture(texture), ImageOptions{}); }

void image(const rmp::Texture &texture, const ImageOptions &o) {
    image(Picture(texture), o);
}

void detail::image_with(const Picture &picture, const ImageOptions &o, const Extra &x,
                        std::string_view id_name, std::optional<Texture2D> raw) {
    if (!detail::frame_ready("image", id_name)) return;
    const LookScope scope(WidgetKind::IMAGE, x.own);
    const Style &look = detail::look();

    Clay_ElementDeclaration d{};
    const Vector2 own = raw
        ? Vector2{ static_cast<float>(raw->width), static_cast<float>(raw->height) }
        : picture.size();
    const float want_w = x.width > 0 ? x.width : o.width;
    const float want_h = x.height > 0 ? x.height : o.height;
    if (o.grow) {
        d.layout.sizing.width = axis(true, 0);
        d.layout.sizing.height = axis(true, 0);
    } else {
        // Its own pixel Size by default, scaled like everything else, so an
        // icon does not shrink into nothing on a big screen.
        d.layout.sizing.width = axis(false, want_w > 0 ? want_w : own.x);
        d.layout.sizing.height = axis(false, want_h > 0 ? want_h : own.y);
    }

    // The tint deliberately does NOT go in backgroundColor. Clay emits a
    // RECTANGLE for any element with a background, *in addition to* the IMAGE
    // and after it in the list — so a tint set that way is a coloured square
    // painted straight over the picture. Cost me a screenshot to find.
    //
    // It travels in the element's record instead (userData), which also holds
    // the part of the texture, the nine-slice margins, and -- for a Picture --
    // the handles that keep the texture loaded until it is drawn: a temporary
    // Picture is safe. A Texture2D you pass is copied: the 20 bytes of it,
    // never the pixels, so the texture itself still has to stay loaded.
    Color tint = look.tint.value_or(o.tint);
    if (look.tint) {
        const Color a = *look.tint;
        tint = Color{ static_cast<unsigned char>(a.r * o.tint.r / 255),
                      static_cast<unsigned char>(a.g * o.tint.g / 255),
                      static_cast<unsigned char>(a.b * o.tint.b / 255),
                      static_cast<unsigned char>(a.a * o.tint.a / 255) };
    }
    rmp::Ref<detail::DrawRecord> rec;
    if (raw) {
        if (raw->id != 0) rec = detail::raw_record(*raw, tint);
    } else {
        rec = detail::picture_record(picture, tint);
    }
    if (rec) d.image.imageData = &rec->raw;
    last_image.data = d.image.imageData;

    const bool kept = x.state != nullptr;
    detail::Interaction now;
    Clay_ElementId id{};
    if (kept || look.shadow || look.motion || look.skin) {
        id = detail::element_id(id_name.empty() ? std::string_view("image") : id_name,
                                x.id);
        if (kept) {
            now.over = detail::pointer_over(id);
            if (now.over) detail::set_pointer_over_ui();
            if (now.over && detail::pointer_just_pressed()) detail::set_press_id(id.id);
            now.down = now.over && detail::pointer_down() && detail::press_id() == id.id;
            now.clicked =
                now.over && detail::pointer_released() && detail::press_id() == id.id;
            const detail::Edges edges = detail::track(id, WidgetKind::IMAGE, x.id, now);
            if (x.edges) *x.edges = edges;
            if (x.element) *x.element = id.id;
        }
    }
    if (look.skin && !look.skin->normal.empty()) {
        // A frame behind the picture: the skin is the box, the picture inside
        // it, as far in as the style's padding.
        const Theme &t = current_theme();
        Clay_ElementDeclaration frame{};
        frame.layout.sizing.width = axis(false, 0);
        frame.layout.sizing.height = axis(false, 0);
        frame.layout.padding = Clay_Padding{ px16(t.padding_x), px16(t.padding_x),
                                             px16(t.padding_y), px16(t.padding_y) };
        frame.layout.childAlignment =
            Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
        detail::decorate(frame, id, now);
        Clay__OpenElementWithId(id);
        Clay__ConfigureOpenElement(frame);
        if (rec) d.userData = &*rec;
        Clay__OpenElement();
        Clay__ConfigureOpenElement(d);
        Clay__CloseElement();
        Clay__CloseElement();
        return;
    }
    if (id.id != 0) {
        detail::decorate(d, id, now, rec);
        if (!rec) d.userData = nullptr;
        Clay__OpenElementWithId(id);
    } else {
        if (rec) d.userData = &*rec;
        Clay__OpenElement();
    }
    Clay__ConfigureOpenElement(d);
    Clay__CloseElement();
}

void progress(float fraction) { progress(fraction, ProgressOptions{}); }

void progress(float fraction, const ProgressOptions &o) {
    detail::progress_with(fraction, o, {}, detail::Extra{});
}

void detail::progress_with(float fraction, const ProgressOptions &o,
                           rmp::Ref<const Picture> fill, const Extra &x) {
    if (!detail::frame_ready("progress", o.id)) return;
    const LookScope scope(WidgetKind::PROGRESS, x.own);
    const Theme &t = current_theme();
    const Style &look = detail::look();

    // Clamped, as the header says; and NaN -- hp / max_hp with no max_hp --
    // is no fraction at all: an empty bar, and the mistake said once.
    if (std::isnan(fraction)) {
        RMP_REPORT_ONCE("UI: rmp::ui::progress() was handed a fraction that is not a "
                        "number; the bar is drawn empty");
        fraction = 0.0f;
    }
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;

    const float h = o.height < 0 ? t.font_size * 0.6f : o.height;
    const float r = o.radius < 0 ? h * 0.5f : o.radius;

    Clay_ElementDeclaration track{};
    track.layout.sizing.width = axis(o.width <= 0, o.width);
    track.layout.sizing.height = axis(false, h);
    track.backgroundColor = to_clay(transparent(o.track) ? t.surface : o.track);
    track.cornerRadius = Clay_CornerRadius{ px(r), px(r), px(r), px(r) };
    if (look.border && look.border_width && *look.border_width > 0) {
        auto w = px16(*look.border_width);
        track.border.color = to_clay(*look.border);
        track.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
    }

    const bool styled = x.state != nullptr || look.skin || look.shadow || look.motion;
    if (styled) {
        const Clay_ElementId id =
            element_id(o.id.empty() ? std::string_view("progress") : o.id, o.id);
        detail::Interaction now;
        if (x.state != nullptr) {
            now.over = detail::pointer_over(id);
            const detail::Edges edges =
                detail::track(id, WidgetKind::PROGRESS, o.id, now);
            if (x.edges) *x.edges = edges;
            if (x.element) *x.element = id.id;
        }
        detail::decorate(track, id, now);
        Clay__OpenElementWithId(id);
        Clay__ConfigureOpenElement(track);
    } else {
        open_with_id(o.id, track);
    }
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
        if (fill && !fill->empty()) {
            // The filled part as a picture: a nine-slice stretches with it.
            if (const auto rec =
                    detail::picture_record(*fill, look.tint.value_or(WHITE))) {
                bar.backgroundColor = Clay_Color{ 0, 0, 0, 0 };
                bar.image.imageData = &rec->raw;
                bar.userData = &*rec;
            }
        }
        Clay__OpenElement();
        Clay__ConfigureOpenElement(bar);
        Clay__CloseElement();
    }
    Clay__CloseElement();
}

} // namespace rmp::ui
