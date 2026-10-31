// ===========================================================================
// begin / end / button / text.
//
// This is the whole MVP from the user's side. Everything it does that looks
// like magic — the menu being centred, the buttons being the right size on a
// phone, the text not crashing when it is a temporary — is decided here or in
// context.cpp, and none of it is asked of the caller.
// ===========================================================================

#include "internal.h"
#include "../internal.h" // RMP_REPORT_ONCE

#include <rmp/config.h>

namespace rmp::ui {

namespace {

using detail::px;

// What the last pass asked to be drawn (detail::last_pass_commands()): Clay's
// array, which lives in Clay's arena until the next layout.
struct {
    Clay_RenderCommandArray commands{};
} last_pass;

// Sizing helpers. Clay's own CLAY_SIZING_* are macros with compound literals;
// these are the same thing written as C++ so the macros never come near us.
Clay_SizingAxis size_fit() {
    Clay_SizingAxis a{};
    a.type = CLAY__SIZING_TYPE_FIT;
    a.size.minMax = Clay_SizingMinMax{ 0, 0 };
    return a;
}

Clay_SizingAxis size_grow() {
    Clay_SizingAxis a{};
    a.type = CLAY__SIZING_TYPE_GROW;
    a.size.minMax = Clay_SizingMinMax{ 0, 0 };
    return a;
}

Clay_SizingAxis size_fit_min(float min) {
    Clay_SizingAxis a{};
    a.type = CLAY__SIZING_TYPE_FIT;
    a.size.minMax = Clay_SizingMinMax{ min, 0 };
    return a;
}

// Where the root's content sits. Clay expresses this as child alignment on the
// root, which is exactly the "intent, not coordinates" the API promises.
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

} // namespace

Clay_RenderCommandArray detail::last_pass_commands() { return last_pass.commands; }

Clay_Padding detail::root_padding(const float pixels) {
    const detail::Insets in = detail::content_insets();
    auto side = [pixels](float inset) { return detail::whole_pixels(pixels + inset); };
    return Clay_Padding{ side(in.left), side(in.right), side(in.top), side(in.bottom) };
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void begin() { begin(FrameOptions{}); }

void begin(const FrameOptions &o) { detail::open_frame(o, Color{ 0, 0, 0, 0 }); }

void detail::open_frame(const FrameOptions &o, const Color fill) {
    // In _update(), or under the camera: said once for the phase, and nothing
    // opened -- so what follows says nothing more, and its end() closes
    // nothing.
    if (detail::phase_refuses("begin")) return;
    if (!detail::ensure_started()) return;

    if (detail::frame_open()) {
        // A mistake in the game, so said once and not sixty times a second.
        // What is between this begin() and its end() goes into the frame
        // already open, where it was written, and that frame still closes
        // where it was meant to.
        RMP_REPORT_ONCE(
            "UI: rmp::ui::begin() while a frame is open: end() was not called, "
            "or begin() is inside a container -- column() is what goes there. "
            "Its content went into the frame already open.");
        detail::note_nested_begin();
        return;
    }
    // Marking the frame is the app's job — it happens once, around however many
    // scenes describe UI. With no app there is no one to do it, so the first
    // begin() of the frame does it and end() closes it: rmp::ui in a plain
    // raylib loop behaves exactly as it did before any of this existed.
    if (!detail::frame_marked()) {
        detail::begin_frame();
        detail::set_frame_self_marked(true);
    } else {
        // The app marked this frame, possibly before the UI existed — which is
        // exactly what frame one looks like. ensure_started() has just brought
        // it up, so the half that needs Clay can run now. A no-op every other
        // time.
        detail::prepare_frame();
    }

    detail::set_frame_open(true);
    // Before the pass starts: its default focus depends on it.
    detail::set_pass_focus_first(o.focus_first);
    detail::begin_pass();

    const Theme &t = current_theme();

    Clay_BeginLayout();

    // The root. A centred column, because the case that has to be three
    // functions long is a main menu, and a plain top-left column would put it
    // in the corner.
    const float gap = (o.gap < 0) ? t.gap : o.gap;
    const float padding = (o.padding < 0) ? t.panel_padding : o.padding;

    Clay_ElementDeclaration root{};
    root.layout.sizing.width = size_grow();
    root.layout.sizing.height = size_grow();
    root.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    root.layout.childAlignment = alignment_of(o.placement);
    // The safe area -- [android.display] into_cutout draws the game behind the
    // notch, which is right for a background and wrong for a menu -- and the
    // letterbox bars: the frame is laid out over the world, a HUD in its
    // corner, while the root still covers the viewport, bars and all.
    root.layout.padding = detail::root_padding(px(padding));
    // Painted only when asked: an overlay's veil, which -- covering the
    // viewport -- takes the pointer everywhere.
    if (fill.a > 0) root.backgroundColor = to_clay(fill);

    Clay__OpenElement();
    Clay__ConfigureOpenElement(root);

    // An inner column, and it is not ceremony: it is what makes the buttons
    // come out the same width. The column fits its content, so it ends up as
    // wide as the widest child; the children then grow to fill it. Without it
    // every button is only as wide as its own label, and a menu reading
    // Play / Options / Quit comes out as a ragged staircase.
    //
    // With `grow` the column fills the root instead -- the whole window, less
    // the padding -- so a child that grows reaches the edges, which is what a
    // screen laid out as a sidebar beside a content area needs. The column then
    // places what does not grow, so `placement` keeps its meaning.
    Clay_ElementDeclaration content{};
    content.layout.sizing.width = o.grow ? size_grow() : size_fit();
    content.layout.sizing.height = o.grow ? size_grow() : size_fit();
    content.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    content.layout.childGap = px16(gap);
    content.layout.childAlignment = o.grow
        ? alignment_of(o.placement)
        : Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };

    Clay__OpenElement();
    Clay__ConfigureOpenElement(content);
}

void end() {
    // The end() of a begin() said inside an open frame closes nothing.
    if (detail::close_nested_begin()) return;
    if (!detail::frame_ready("end", {})) return;
    detail::close_frame(true);
}

void detail::close_frame(const bool draw) {
    if (!detail::frame_open()) return;
    detail::set_frame_open(false);

    Clay__CloseElement(); // the content column
    Clay__CloseElement(); // the root

    const Clay_RenderCommandArray commands = Clay_EndLayout(detail::frame_time());
    last_pass.commands = commands;

    // AFTER the layout, not before it. Clay hit-tests against the tree it holds
    // at the time, and this is the only moment it holds THIS pass's: called
    // from begin() it tested pass 0 against last frame's pass 1 and pass 1
    // against this frame's pass 0, which is how a pause menu over a HUD ended
    // up with nothing clickable in either scene.
    //
    // Our own widgets no longer read it -- detail::pointer_over() answers them
    // from the snapshot below -- but Clay still needs it for the one thing it
    // owns: which scroll container the wheel and a drag belong to. A pass that
    // input cannot reach is given a position no element can contain, so a HUD
    // under an open menu cannot be scrolled either.
    const bool reachable = detail::pass_input();
    Clay_SetPointerState(reachable ? detail::pointer_position()
                                   : Clay_Vector2{ -1.0e6f, -1.0e6f },
                         reachable && detail::pointer_down());

    // Now that the layout exists, remember where everything landed. The next
    // frame's matching pass reads it — see bounds_of_id().
    detail::capture_pass_bounds();

    // And whether the pointer is over anything this pass painted, which is
    // what "over the interface" means for wants_pointer(): a panel's background
    // or an image takes the click as surely as a button does.
    detail::claim_pointer_over_painted(commands);

    // And if this pass declared something focusable and nothing in the frame
    // has the focus yet, its first widget takes it.
    detail::end_pass_focus();

    // In test mode there is no GL context to draw into; the layout is the
    // whole point and it has already happened. Nor past EndDrawing(), where
    // the runner closes a frame the game left open.
    if (draw && !detail::test_mode()) detail::draw(commands);

    // Before the boundary below can close: the frame is still this one. Every
    // pass, so that what at() and tooltip() kept of it goes with it; the
    // notices only after one that was drawn -- they are drawn too.
    detail::pass_ended(draw);

    // Only when there was no app to mark it. With one, the boundary closes
    // after the last scene has drawn, not after the first.
    if (detail::frame_self_marked()) detail::end_frame();
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

bool button(std::string_view label) { return button(label, ButtonOptions{}); }

bool button(std::string_view label, const ButtonOptions &o) {
    return detail::button_with(label, o, detail::Extra{});
}

bool image_button(const Picture &picture) {
    return image_button(picture, ImageButtonOptions{});
}

bool image_button(const Picture &picture, const ImageButtonOptions &o) {
    detail::Extra x;
    x.icon = rmp::Ref<const Picture>(picture);
    x.width = o.width;
    x.height = o.height;
    // Its size is the picture's, unless asked: no padding around it, no
    // minimum height -- an icon is as big as it was drawn.
    x.icon_size = o.height > 0 ? o.height : picture.size().y;
    return detail::button_with({}, ButtonOptions{ .enabled = o.enabled, .id = o.id }, x);
}

namespace {

// A picture as an element of its own: an icon in a button, the bar of a
// progress. Sized in design units, and drawn by its record.
void picture_element(const Picture &picture, float width, float height, Color tint) {
    Clay_ElementDeclaration d{};
    d.layout.sizing.width =
        Clay_SizingAxis{ .size = { .minMax = { px(width), px(width) } },
                         .type = CLAY__SIZING_TYPE_FIXED };
    d.layout.sizing.height =
        Clay_SizingAxis{ .size = { .minMax = { px(height), px(height) } },
                         .type = CLAY__SIZING_TYPE_FIXED };
    if (const auto rec = detail::picture_record(picture, tint)) {
        d.image.imageData = &rec->raw;
        d.userData = &*rec;
    }
    Clay__OpenElement();
    Clay__ConfigureOpenElement(d);
    Clay__CloseElement();
}

} // namespace

bool detail::button_with(std::string_view label, const ButtonOptions &o, const Extra &x) {
    if (!detail::frame_ready("button", label.empty() ? o.id : label)) return false;
    const detail::LookScope scope(WidgetKind::BUTTON, x.own);

    const Theme &t = current_theme();
    const bool icon_only = label.empty() && x.icon;
    // A picture has no label to be told apart by: the unnamed ones are counted
    // as "image_button", the way two "Play" buttons are counted.
    const Clay_ElementId id = detail::element_id(
        label.empty() ? std::string_view("image_button") : label, o.id);
    detail::note_control(id);

    // Hit-testing uses the geometry this element had LAST frame — this one has
    // not been laid out yet. It is inherent to immediate mode: the first frame
    // a button exists it cannot be clicked, which is 16 ms at 60 fps.
    const bool over = o.enabled && detail::pointer_over(id);
    const bool pressed = over && detail::pointer_down();

    if (over) detail::set_pointer_over_ui();
    if (over && detail::pointer_just_pressed()) detail::set_press_id(id.id);

    // Released over the same element it was pressed on. Drag off and let go and
    // nothing happens, which is what every interface worth using does.
    bool clicked = over && detail::pointer_released() && detail::press_id() == id.id;

    // Keyboard and gamepad get here without the widget knowing how: it declares
    // itself focusable and asks whether it is the one.
    const bool has_focus =
        o.enabled && detail::focusable(id, label.empty() ? o.id : label);
    if (has_focus && detail::take_activate()) clicked = true;

    detail::Interaction now;
    now.over = over;
    now.down = pressed && detail::press_id() == id.id;
    now.focused = has_focus;
    now.disabled = !o.enabled;
    now.clicked = clicked;
    const detail::Edges edges =
        detail::track(id, WidgetKind::BUTTON, label.empty() ? o.id : label, now);
    if (x.edges) *x.edges = edges;
    if (x.element) *x.element = id.id;

    // Colour. Every Variant travels between the same three states and every one
    // of them goes through state_color(), which is why they all feel the same
    // and why the transition is impossible for a new widget to forget.
    Color background = t.surface;
    Color foreground = t.text;
    const Color outline = t.border;
    // 0 in the dark Theme, 1 in the light one. A light interface has no shadows
    // to separate a pale button from a pale page, so it needs the outline that
    // a dark one does not.
    float outline_w = t.border_width;

    if (!o.enabled) {
        background = t.disabled;
        foreground = t.disabled_text;
    } else {
        switch (o.style) {
            case Variant::PRIMARY:
                background = detail::state_color(id, t.primary, t.primary_hover,
                                                 t.primary_press, over, pressed);
                foreground = t.text_on_accent;
                outline_w = 0;
                break;
            case Variant::DANGER:
                background = detail::state_color(id, t.danger, t.danger_hover,
                                                 t.danger_press, over, pressed);
                foreground = t.text_on_accent;
                outline_w = 0;
                break;
            case Variant::OUTLINE:
                // Nothing at rest but the outline and the label; it fills in
                // under the pointer, which is what says it was a button.
                background =
                    detail::state_color(id, detail::clear_alpha(t.surface),
                                        t.surface_hover, t.surface_press, over, pressed);
                outline_w = (t.border_width > 0) ? t.border_width : 1.0f;
                break;
            case Variant::GHOST:
                // Like outline, without the outline — in either Theme, because
                // a ghost that grew a border in the light Theme would just be
                // an outline button with a different name.
                background =
                    detail::state_color(id, detail::clear_alpha(t.surface),
                                        t.surface_hover, t.surface_press, over, pressed);
                outline_w = 0;
                break;
            case Variant::NORMAL:
                background = detail::state_color(id, t.surface, t.surface_hover,
                                                 t.surface_press, over, pressed);
                break;
        }
    }
    // A picture-only button with no skin and no fill of its own is the
    // picture: no flat box behind it.
    const Style &look = detail::look();
    if (icon_only && !look.background && !look.skin)
        background = detail::clear_alpha(background);

    // Size. The type step drives everything else, so a large button is large
    // all over rather than a normal one with bigger letters in it.
    const float font_units = detail::resolve_size(o.size, t);
    const float ratio = detail::size_ratio(o.size, t);
    float min_height = t.min_touch_size * ratio;
    // ...except on a touch screen, where a small button is still a thumb-sized
    // button. Shrinking below the touch target there is the one case where
    // honouring what was asked for makes the control unusable.
    if (detail::touch_only() && min_height < t.min_touch_size)
        min_height = t.min_touch_size;
    if (icon_only && !detail::touch_only()) min_height = 0;

    Clay_ElementDeclaration decl{};
    // GROW inside the FIT column from begin(): the column takes the width of
    // its widest child, then every button expands to match it.
    // A picture-only button's width and height are its picture's: the button
    // fits around it, padding and all.
    if (icon_only)
        decl.layout.sizing.width = size_fit();
    else if (x.width > 0)
        decl.layout.sizing.width =
            Clay_SizingAxis{ .size = { .minMax = { px(x.width), px(x.width) } },
                             .type = CLAY__SIZING_TYPE_FIXED };
    else
        decl.layout.sizing.width = size_grow();
    // min_touch_size is the whole reason a short label still makes a button a
    // thumb can hit.
    decl.layout.sizing.height = x.height > 0 && !icon_only
        ? Clay_SizingAxis{ .size = { .minMax = { px(x.height), px(x.height) } },
                           .type = CLAY__SIZING_TYPE_FIXED }
        : size_fit_min(px(min_height));
    const float pad_x = icon_only && !look.padding_x ? 0.0f : t.padding_x * ratio;
    const float pad_y = icon_only && !look.padding_y ? 0.0f : t.padding_y * ratio;
    decl.layout.padding =
        Clay_Padding{ px16(pad_x), px16(pad_x), px16(pad_y), px16(pad_y) };
    decl.layout.childAlignment =
        Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
    decl.layout.layoutDirection = x.icon_above ? CLAY_TOP_TO_BOTTOM : CLAY_LEFT_TO_RIGHT;
    decl.layout.childGap = px16(t.gap * 0.5f * ratio);
    decl.backgroundColor = to_clay(background);
    const float r = px(t.corner_radius);
    decl.cornerRadius = Clay_CornerRadius{ r, r, r, r };
    if (has_focus && detail::focus_visible()) {
        // The focus ring is drawn by the framework, not by each widget, because
        // a controller build where one widget forgot it is a controller build
        // that gets stuck. It replaces the ordinary outline rather than sitting
        // next to it: two rings on one control reads as a rendering bug.
        auto w = px16(t.focus_ring);
        decl.border.color = to_clay(t.focus);
        decl.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
    } else if (outline_w > 0.0f) {
        auto w = px16(outline_w);
        if (w < 1) w = 1; // a sub-pixel border is an invisible border
        decl.border.color = to_clay(outline);
        decl.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
    }
    detail::decorate(decl, id, now);

    // Clay 0.14 has no id field in the declaration: the id goes in when the
    // element is opened.
    Clay__OpenElementWithId(id);
    Clay__ConfigureOpenElement(decl);
    if (x.icon && !x.icon->empty()) {
        const Vector2 size = x.icon->size();
        const float h = x.icon_size > 0 ? x.icon_size : (icon_only ? size.y : font_units);
        const float w =
            (x.width > 0 && icon_only) ? x.width : (size.y > 0 ? h * size.x / size.y : h);
        Color tint = look.tint.value_or(WHITE);
        if (!o.enabled) tint = detail::mix_color(tint, Color{ 120, 120, 128, 200 }, 0.5f);
        picture_element(*x.icon, w, x.height > 0 && icon_only ? x.height : h, tint);
    }
    if (!label.empty()) {
        Clay_TextElementConfig tc{};
        tc.textColor = to_clay(foreground);
        tc.fontSize = font_px(font_units);
        tc.wrapMode = CLAY_TEXT_WRAP_NONE;
        detail::open_text(detail::intern(label), tc);
    }
    Clay__CloseElement();

    return clicked;
}

void text(std::string_view s) { text(s, TextOptions{}); }

void text(std::string_view s, const TextOptions &o) {
    detail::text_with(s, o, detail::Extra{});
}

void detail::text_with(std::string_view s, const TextOptions &o, const Extra &x) {
    if (!detail::frame_ready("text", s)) return;
    const detail::LookScope scope(WidgetKind::LABEL, x.own);

    const Theme &t = current_theme();
    Color c = t.text;
    switch (o.color) {
        case ColorRole::TEXT:
            c = t.text;
            break;
        case ColorRole::MUTED:
            c = t.text_muted;
            break;
        case ColorRole::PRIMARY:
            c = t.primary;
            break;
        case ColorRole::DANGER:
            c = t.danger;
            break;
    }
    // A text colour the style sets wins over the role.
    if (const auto &text = detail::look().text) c = *text;

    Clay_TextElementConfig tc{};
    tc.textColor = to_clay(c);
    tc.fontSize = font_px(detail::resolve_size(o.size, t));
    tc.wrapMode = o.wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE;

    // A kept Label is a box with an id around its text -- what can be hovered,
    // clicked, shaken and given a skin; text() is the text alone.
    const bool kept = x.state != nullptr;
    detail::Interaction now;
    Clay_ElementId id{};
    if (kept) {
        id = detail::element_id(s, x.id);
        now.over = detail::pointer_over(id);
        if (now.over && detail::pointer_just_pressed()) detail::set_press_id(id.id);
        now.down = now.over && detail::pointer_down() && detail::press_id() == id.id;
        now.clicked =
            now.over && detail::pointer_released() && detail::press_id() == id.id;
        const detail::Edges edges =
            detail::track(id, WidgetKind::LABEL, x.id.empty() ? s : x.id, now);
        if (x.edges) *x.edges = edges;
        if (x.element) *x.element = id.id;
        Clay_ElementDeclaration box{};
        box.layout.sizing.width = o.wrap ? size_grow() : size_fit();
        box.layout.sizing.height = size_fit();
        const Style &look = detail::look();
        if (look.background) box.backgroundColor = to_clay(*look.background);
        if (look.padding_x || look.padding_y)
            box.layout.padding = Clay_Padding{ px16(t.padding_x), px16(t.padding_x),
                                               px16(t.padding_y), px16(t.padding_y) };
        const float r = look.radius ? px(*look.radius) : 0.0f;
        box.cornerRadius = Clay_CornerRadius{ r, r, r, r };
        if (look.border && look.border_width && *look.border_width > 0) {
            auto w = px16(*look.border_width);
            box.border.color = to_clay(*look.border);
            box.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
        }
        detail::decorate(box, id, now);
        Clay__OpenElementWithId(id);
        Clay__ConfigureOpenElement(box);
    }

    // Copied on the way in, so text(std::to_string(score)) is safe: Clay keeps
    // the pointer and reads it at layout time, long after a temporary would be
    // gone.
    detail::open_text(detail::intern(s), tc);
    if (kept) Clay__CloseElement();
}

// ---------------------------------------------------------------------------

void shutdown() {
    // The game's mistake -- the entry point shuts the UI down between frames
    // -- so it is reported. The frame goes with everything else.
    if (detail::frame_open()) {
        RMP_REPORT_ONCE("UI: rmp::ui::shutdown() while a frame was open: rmp::ui::end() "
                        "was never called.");
    }
    last_pass.commands = Clay_RenderCommandArray{}; // Clay's arena goes below
    detail::shutdown_context();
}

} // namespace rmp::ui
