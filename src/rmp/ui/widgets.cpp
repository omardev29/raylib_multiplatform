// ===========================================================================
// begin / end / button / text.
//
// This is the whole MVP from the user's side. Everything it does that looks
// like magic — the menu being centred, the buttons being the right size on a
// phone, the text not crashing when it is a temporary — is decided here or in
// context.cpp, and none of it is asked of the caller.
// ===========================================================================

#include "internal.h"

#include <rmp/config.h>

namespace rmp::ui {

namespace {

using detail::px;
using detail::to_clay;

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

// Which element the press started on, so a click is "released over the element
// it was pressed on" rather than "the button happens to be down". The pointer
// state itself is sampled once per frame in context.cpp, so every widget in the
// frame sees the same thing.
uint32_t g_pressed_id = 0;

Clay_Padding uniform_padding(float p) {
    auto v = static_cast<uint16_t>(p);
    return Clay_Padding{ v, v, v, v };
}

} // namespace

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void begin() { begin(FrameOptions{}); }

void begin(const FrameOptions &o) {
    if (!detail::ensure_started()) return;

    if (detail::frame_open()) {
        TraceLog(LOG_WARNING,
                 "UI: begin() called twice without end(); ignoring the second one");
        return;
    }
    detail::set_frame_open(true);

    detail::reset_frame_arena();
    detail::reset_id_counters();
    detail::update_scale();
    detail::anim_begin_frame();
    detail::begin_focus_frame();

    const Theme &t = current_theme();

    Clay_SetLayoutDimensions(detail::viewport());

    detail::update_pointer();
    Clay_SetPointerState(detail::pointer_position(), detail::pointer_down());

    // Scroll containers, before BeginLayout — Clay is explicit that after it
    // the offset arrives a frame late. Drag scrolling is on because on a phone
    // that is the only way to scroll anything; the wheel is the desktop half of
    // the same gesture.
    Vector2 wheel = GetMouseWheelMoveV();
    Clay_UpdateScrollContainers(true, Clay_Vector2{ wheel.x * 30.0f, wheel.y * 30.0f },
                                GetFrameTime());

    Clay_BeginLayout();

    // The root. A centred column, because the case that has to be three
    // functions long is a main menu, and a plain top-left column would put it
    // in the corner.
    float gap = (o.gap < 0) ? t.gap : o.gap;
    float padding = (o.padding < 0) ? t.panel_padding : o.padding;

    Clay_ElementDeclaration root{};
    root.layout.sizing.width = size_grow();
    root.layout.sizing.height = size_grow();
    root.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    root.layout.childAlignment = alignment_of(o.placement);
    // Safe area. [android.display] into_cutout draws the game behind the notch,
    // which is right for a background and wrong for a menu.
    root.layout.padding = uniform_padding(px(padding) + detail::safe_area_inset());

    Clay__OpenElement();
    Clay__ConfigureOpenElement(root);

    // An inner column, and it is not ceremony: it is what makes the buttons
    // come out the same width. The column fits its content, so it ends up as
    // wide as the widest child; the children then grow to fill it. Without it
    // every button is only as wide as its own label, and a menu reading
    // Play / Options / Quit comes out as a ragged staircase.
    Clay_ElementDeclaration content{};
    content.layout.sizing.width = size_fit();
    content.layout.sizing.height = size_fit();
    content.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    content.layout.childGap = static_cast<uint16_t>(px(gap));
    content.layout.childAlignment =
        Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };

    Clay__OpenElement();
    Clay__ConfigureOpenElement(content);
}

void end() {
    if (!detail::frame_open()) return;
    detail::set_frame_open(false);

    Clay__CloseElement(); // the content column
    Clay__CloseElement(); // the root

    detail::end_focus_frame();

    Clay_RenderCommandArray commands = Clay_EndLayout(GetFrameTime());
    // In test mode there is no GL context to draw into; the layout is the
    // whole point and it has already happened.
    if (!detail::test_mode()) detail::draw(commands);

    if (!detail::pointer_down()) g_pressed_id = 0;
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

bool button(std::string_view label) { return button(label, ButtonOptions{}); }

bool button(std::string_view label, const ButtonOptions &o) {
    if (!detail::frame_open()) return false;

    const Theme &t = current_theme();
    Clay_ElementId id = detail::element_id(label, o.id);

    // Hit-testing uses the geometry this element had LAST frame — Clay has not
    // laid out this one yet. It is inherent to immediate mode: the first frame
    // a button exists it cannot be clicked, which is 16 ms at 60 fps.
    const bool over = o.enabled && detail::pointer_present() && Clay_PointerOver(id);
    const bool pressed = over && detail::pointer_down();

    if (over) detail::set_pointer_over_ui();
    if (over && detail::pointer_just_pressed()) g_pressed_id = id.id;

    // Released over the same element it was pressed on. Drag off and let go and
    // nothing happens, which is what every interface worth using does.
    bool clicked = over && detail::pointer_released() && g_pressed_id == id.id;

    // Keyboard and gamepad get here without the widget knowing how: it declares
    // itself focusable and asks whether it is the one.
    const bool has_focus = o.enabled && detail::focusable(id, label);
    if (has_focus && detail::take_activate()) clicked = true;

    // Colour. Every Variant travels between the same three states and every one
    // of them goes through state_color(), which is why they all feel the same
    // and why the transition is impossible for a new widget to forget.
    Color background = t.surface;
    Color foreground = t.text;
    Color outline = t.border;
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

    Clay_ElementDeclaration decl{};
    // GROW inside the FIT column from begin(): the column takes the width of
    // its widest child, then every button expands to match it.
    decl.layout.sizing.width = size_grow();
    // min_touch_size is the whole reason a short label still makes a button a
    // thumb can hit.
    decl.layout.sizing.height = size_fit_min(px(min_height));
    decl.layout.padding = Clay_Padding{ static_cast<uint16_t>(px(t.padding_x * ratio)),
                                        static_cast<uint16_t>(px(t.padding_x * ratio)),
                                        static_cast<uint16_t>(px(t.padding_y * ratio)),
                                        static_cast<uint16_t>(px(t.padding_y * ratio)) };
    decl.layout.childAlignment =
        Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
    decl.backgroundColor = to_clay(background);
    float r = px(t.corner_radius);
    decl.cornerRadius = Clay_CornerRadius{ r, r, r, r };
    if (has_focus) {
        // The focus ring is drawn by the framework, not by each widget, because
        // a controller build where one widget forgot it is a controller build
        // that gets stuck. It replaces the ordinary outline rather than sitting
        // next to it: two rings on one control reads as a rendering bug.
        auto w = static_cast<uint16_t>(px(t.focus_ring));
        decl.border.color = to_clay(t.focus);
        decl.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
    } else if (outline_w > 0.0f) {
        auto w = static_cast<uint16_t>(px(outline_w));
        if (w < 1) w = 1; // a sub-pixel border is an invisible border
        decl.border.color = to_clay(outline);
        decl.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
    }

    // Clay 0.14 has no id field in the declaration: the id goes in when the
    // element is opened.
    Clay__OpenElementWithId(id);
    Clay__ConfigureOpenElement(decl);
    {
        Clay_TextElementConfig tc{};
        tc.textColor = to_clay(foreground);
        tc.fontSize = static_cast<uint16_t>(px(font_units));
        tc.wrapMode = CLAY_TEXT_WRAP_NONE;
        Clay__OpenTextElement(detail::intern(label), tc);
    }
    Clay__CloseElement();

    return clicked;
}

void text(std::string_view s) { text(s, TextOptions{}); }

void text(std::string_view s, const TextOptions &o) {
    if (!detail::frame_open()) return;

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

    Clay_TextElementConfig tc{};
    tc.textColor = to_clay(c);
    tc.fontSize = static_cast<uint16_t>(px(detail::resolve_size(o.size, t)));
    tc.wrapMode = o.wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE;

    // Copied on the way in, so text(std::to_string(score)) is safe: Clay keeps
    // the pointer and reads it at layout time, long after a temporary would be
    // gone.
    Clay__OpenTextElement(detail::intern(s), tc);
}

// ---------------------------------------------------------------------------

void shutdown() {
    if (detail::frame_open()) {
        TraceLog(LOG_WARNING,
                 "UI: shutdown() while a frame was open; end() was never called");
        detail::set_frame_open(false);
    }
    detail::shutdown_context();
}

} // namespace rmp::ui
