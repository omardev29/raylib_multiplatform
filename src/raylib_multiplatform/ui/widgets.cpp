// ===========================================================================
// begin / end / button / text.
//
// This is the whole MVP from the user's side. Everything it does that looks
// like magic — the menu being centred, the buttons being the right size on a
// phone, the text not crashing when it is a temporary — is decided here or in
// context.cpp, and none of it is asked of the caller.
// ===========================================================================

#include "internal.h"

#include <raylib_multiplatform/generated/app_config.h>

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

// The pointer state this frame, and the previous frame's, so a click can be
// "released over the element it was pressed on" rather than "the button
// happens to be down".
bool     g_pointerDown     = false;
bool     g_pointerWasDown  = false;
uint32_t g_pressedId       = 0;
// On a touch screen there is no pointer when there is no finger: the position
// stays wherever the last tap ended, and a button would sit lit up forever.
bool     g_pointerPresent  = false;

Clay_Padding uniform_padding(float p) {
    auto v = static_cast<uint16_t>(p);
    return Clay_Padding{ v, v, v, v };
}

} // namespace

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void begin() { begin(frame_options{}); }

void begin(const frame_options &o) {
    if (!detail::ensure_started()) return;

    if (detail::frame_open()) {
        TraceLog(LOG_WARNING, "UI: begin() called twice without end(); ignoring the second one");
        return;
    }
    detail::set_frame_open(true);

    detail::reset_frame_arena();
    detail::reset_id_counters();
    detail::update_scale();

    const theme &t = current_theme();

    Clay_SetLayoutDimensions(detail::viewport());

    Clay_Vector2 pointer{};
    bool down = false;
    detail::read_pointer(&pointer, &down);

    g_pointerWasDown = g_pointerDown;
    g_pointerDown    = down;
    // Desktop always has a pointer. Touch only has one while a finger is down —
    // and raylib reports (0,0) before the first ever touch.
    g_pointerPresent = down || !detail::touch_only();

    Clay_SetPointerState(pointer, down);
    Clay_BeginLayout();

    // The root. A centred column, because the case that has to be three
    // functions long is a main menu, and a plain top-left column would put it
    // in the corner.
    float gap     = (o.gap < 0)     ? t.gap : o.gap;
    float padding = (o.padding < 0) ? t.panel_padding : o.padding;

    Clay_ElementDeclaration root{};
    root.layout.sizing.width  = size_grow();
    root.layout.sizing.height = size_grow();
    root.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    root.layout.childAlignment  = alignment_of(o.placement);
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
    content.layout.sizing.width  = size_fit();
    content.layout.sizing.height = size_fit();
    content.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
    content.layout.childGap = static_cast<uint16_t>(px(gap));
    content.layout.childAlignment = Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };

    Clay__OpenElement();
    Clay__ConfigureOpenElement(content);
}

void end() {
    if (!detail::frame_open()) return;
    detail::set_frame_open(false);

    Clay__CloseElement();   // the content column
    Clay__CloseElement();   // the root

    Clay_RenderCommandArray commands = Clay_EndLayout(GetFrameTime());
    // In test mode there is no GL context to draw into; the layout is the
    // whole point and it has already happened.
    if (!detail::test_mode()) detail::draw(commands);

    if (!g_pointerDown) g_pressedId = 0;
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

bool button(std::string_view label) { return button(label, button_options{}); }

bool button(std::string_view label, const button_options &o) {
    if (!detail::frame_open()) return false;

    const theme &t = current_theme();
    Clay_ElementId id = detail::element_id(label, o.id);

    // Hit-testing uses the geometry this element had LAST frame — Clay has not
    // laid out this one yet. It is inherent to immediate mode: the first frame
    // a button exists it cannot be clicked, which is 16 ms at 60 fps.
    const bool over    = o.enabled && g_pointerPresent && Clay_PointerOver(id);
    const bool pressed = over && g_pointerDown;

    if (over && g_pointerDown && !g_pointerWasDown) g_pressedId = id.id;

    // Released over the same element it was pressed on. Drag off and let go and
    // nothing happens, which is what every interface worth using does.
    const bool clicked = over && !g_pointerDown && g_pointerWasDown && g_pressedId == id.id;

    Color background = t.surface;
    Color foreground = t.text;
    if (!o.enabled) {
        background = t.disabled;
        foreground = t.disabled_text;
    } else if (o.style == variant::primary) {
        background = pressed ? t.primary_press : (over ? t.primary_hover : t.primary);
        foreground = t.text_on_accent;
    } else if (o.style == variant::danger) {
        background = pressed ? t.danger_press : (over ? t.danger_hover : t.danger);
        foreground = t.text_on_accent;
    } else {
        background = pressed ? t.surface_press : (over ? t.surface_hover : t.surface);
    }

    Clay_ElementDeclaration decl{};
    // GROW inside the FIT column from begin(): the column takes the width of
    // its widest child, then every button expands to match it.
    decl.layout.sizing.width  = size_grow();
    // min_touch_size is the whole reason a short label still makes a button a
    // thumb can hit.
    decl.layout.sizing.height = size_fit_min(px(t.min_touch_size));
    decl.layout.padding = Clay_Padding{ static_cast<uint16_t>(px(t.padding_x)),
                                        static_cast<uint16_t>(px(t.padding_x)),
                                        static_cast<uint16_t>(px(t.padding_y)),
                                        static_cast<uint16_t>(px(t.padding_y)) };
    decl.layout.childAlignment = Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
    decl.backgroundColor = to_clay(background);
    float r = px(t.corner_radius);
    decl.cornerRadius = Clay_CornerRadius{ r, r, r, r };

    // Clay 0.14 has no id field in the declaration: the id goes in when the
    // element is opened.
    Clay__OpenElementWithId(id);
    Clay__ConfigureOpenElement(decl);
    {
        Clay_TextElementConfig tc{};
        tc.textColor = to_clay(foreground);
        tc.fontSize  = static_cast<uint16_t>(px(t.font_size));
        tc.wrapMode  = CLAY_TEXT_WRAP_NONE;
        Clay__OpenTextElement(detail::intern(label), tc);
    }
    Clay__CloseElement();

    return clicked;
}

void text(std::string_view s) { text(s, text_options{}); }

void text(std::string_view s, const text_options &o) {
    if (!detail::frame_open()) return;

    const theme &t = current_theme();
    Color c = t.text;
    switch (o.color) {
        case color_role::text:    c = t.text;       break;
        case color_role::muted:   c = t.text_muted; break;
        case color_role::primary: c = t.primary;    break;
        case color_role::danger:  c = t.danger;     break;
    }

    Clay_TextElementConfig tc{};
    tc.textColor = to_clay(c);
    tc.fontSize  = static_cast<uint16_t>(px(o.size < 0 ? t.font_size : o.size));
    tc.wrapMode  = o.wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE;

    // Copied on the way in, so text(std::to_string(score)) is safe: Clay keeps
    // the pointer and reads it at layout time, long after a temporary would be
    // gone.
    Clay__OpenTextElement(detail::intern(s), tc);
}

// ---------------------------------------------------------------------------

void shutdown() {
    if (detail::frame_open()) {
        TraceLog(LOG_WARNING, "UI: shutdown() while a frame was open; end() was never called");
        detail::set_frame_open(false);
    }
    detail::shutdown_context();
}

} // namespace rmp::ui
