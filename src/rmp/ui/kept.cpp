// ===========================================================================
// Widgets you keep: Button, Label, Image, Panel, Checkbox, Slider, Dropdown,
// TextInput, ProgressBar.
//
// Each is the immediate widget of the same name, drawn with three things the
// immediate call has no room for: its own Style, what it remembers between
// frames (KeptState), and its callbacks. show() hands the first to the
// immediate code through detail::Extra, gets back what changed this frame
// (detail::Edges, the same edges the sounds and on_event() hear), keeps the
// state, and calls the callbacks of what happened -- inside show(), on the
// frame it happened, in the order it happened: hover, press, release, click,
// focus, change.
//
// There is no second implementation of any widget here, so a kept Button lays
// out, navigates and is hit-tested exactly as rmp::ui::button() is.
// ===========================================================================

#include "internal.h"
#include "../animation_internal.h" // advance(): an Image's sprite plays

#include <string>
#include <string_view>
#include <vector>

namespace rmp::ui {

namespace {

// What the edges say the widget is doing now, kept.
void keep(KeptState &state, const detail::Edges &e) {
    if (e.hover) state.hovered = true;
    if (e.unhover) state.hovered = false;
    if (e.press) state.pressed = true;
    if (e.release) state.pressed = false;
    if (e.focus) state.focused = true;
    if (e.blur) state.focused = false;
}

// The pointer half of the callbacks, in the order the events happened.
void pointer_calls(const detail::Edges &e, const rmp::Callback<> &on_hover,
                   const rmp::Callback<> &on_unhover) {
    if (e.hover) on_hover();
    if (e.unhover) on_unhover();
}

void press_calls(const detail::Edges &e, const rmp::Callback<> &on_press,
                 const rmp::Callback<> &on_release) {
    if (e.press) on_press();
    if (e.release) on_release();
}

void focus_calls(const detail::Edges &e, const rmp::Callback<> &on_focus,
                 const rmp::Callback<> &on_blur) {
    if (e.focus) on_focus();
    if (e.blur) on_blur();
}

// shake() since the last show(): started, now that the element has an id.
void shakes(KeptState &state, uint32_t element, WidgetKind kind, std::string_view name) {
    if (state.shakes <= 0 || element == 0) return;
    state.shakes = 0;
    detail::start_shake(element, kind, name);
}

// The Extra every kept widget hands its immediate one.
detail::Extra extra_for(const Style &style, KeptState &state, detail::Edges &edges,
                        uint32_t &element, std::string_view id) {
    detail::Extra x;
    x.own = &style;
    x.state = &state;
    x.edges = &edges;
    x.element = &element;
    x.id = id;
    return x;
}

std::string_view name_of(std::string_view text, std::string_view id) {
    return id.empty() ? text : id;
}

// The kept panels open right now, innermost last: whether each one really
// opened (a frame that refused it opened nothing for close_kept_panel() to close).
std::vector<bool> &open_panels() {
    static std::vector<bool> stack;
    return stack;
}

} // namespace

// ---------------------------------------------------------------------------

bool Button::show() {
    detail::Edges e;
    uint32_t element = 0;
    detail::Extra x = extra_for(style, ours, e, element, id);
    if (!icon.empty()) x.icon = &icon;
    x.icon_size = icon_size;
    x.icon_above = icon_above;
    x.width = width;
    x.height = height;
    const ButtonOptions o{ .style = variant, .size = size, .enabled = enabled, .id = id };
    const bool clicked = detail::button_with(text, o, x);
    keep(ours, e);
    shakes(ours, element, WidgetKind::BUTTON, name_of(text, id));
    pointer_calls(e, on_hover, on_unhover);
    press_calls(e, on_press, on_release);
    if (clicked) on_click();
    focus_calls(e, on_focus, on_blur);
    return clicked;
}

void Button::shake() { ++ours.shakes; }

// ---------------------------------------------------------------------------

bool Label::show() {
    detail::Edges e;
    uint32_t element = 0;
    const detail::Extra x = extra_for(style, ours, e, element, id);
    detail::text_with(text, TextOptions{ .color = color, .size = size, .wrap = wrap }, x);
    keep(ours, e);
    shakes(ours, element, WidgetKind::LABEL, name_of(text, id));
    pointer_calls(e, on_hover, on_unhover);
    if (e.click) on_click();
    return e.click;
}

void Label::shake() { ++ours.shakes; }

// ---------------------------------------------------------------------------

bool Image::show() {
    detail::Edges e;
    uint32_t element = 0;
    detail::Extra x = extra_for(style, ours, e, element, id);
    x.width = width;
    x.height = height;
    // A sprite plays: one step of its clock per frame shown, the game's step.
    const bool animated = sprite.sheet.valid() || sprite.texture.valid();
    if (sprite.sheet.valid())
        rmp::animation::detail::advance(sprite, detail::frame_time());
    const Picture shown = animated ? Picture(sprite) : picture;
    detail::image_with(shown, ImageOptions{ .grow = grow }, x, id.empty() ? "image" : id);
    keep(ours, e);
    shakes(ours, element, WidgetKind::IMAGE, id);
    pointer_calls(e, on_hover, on_unhover);
    press_calls(e, on_press, on_release);
    const bool clicked = enabled && e.click;
    if (clicked) on_click();
    return clicked;
}

void Image::shake() { ++ours.shakes; }

// ---------------------------------------------------------------------------

namespace detail {

bool open_kept_panel(Panel &panel) {
    if (!frame_ready("panel", panel.id)) {
        open_panels().push_back(false);
        return false;
    }
    const Clay_ElementId id = element_id(panel.id.empty() ? std::string_view("panel")
                                                          : std::string_view(panel.id),
                                         panel.id);
    Interaction now;
    now.over = pointer_over(id);
    const bool clickable = static_cast<bool>(panel.on_click);
    if (now.over && clickable) set_pointer_over_ui();
    if (now.over && pointer_just_pressed()) set_press_id(id.id);
    now.down = now.over && pointer_down() && press_id() == id.id;
    now.clicked = clickable && now.over && pointer_released() && press_id() == id.id;
    const Edges e = track(id, WidgetKind::PANEL, panel.id, now);

    PanelOptions o{ .box = panel.box };
    open_panel_with(o, &panel.style, id, &now, panel.row, !panel.background);
    push_children(panel.children);
    open_panels().push_back(true);

    keep(panel.ours, e);
    shakes(panel.ours, id.id, WidgetKind::PANEL, panel.id);
    pointer_calls(e, panel.on_hover, panel.on_unhover);
    press_calls(e, panel.on_press, panel.on_release);
    if (now.clicked) panel.on_click();
    return now.clicked;
}

void close_kept_panel() {
    if (open_panels().empty()) return;
    const bool opened = open_panels().back();
    open_panels().pop_back();
    if (!opened || !frame_open()) return;
    pop_children();
    Clay__CloseElement();
}

} // namespace detail

void Panel::shake() { ++ours.shakes; }

// ---------------------------------------------------------------------------

bool Checkbox::show() {
    detail::Edges e;
    uint32_t element = 0;
    const detail::Extra x = extra_for(style, ours, e, element, id);
    const bool changed = detail::checkbox_with(
        text, checked, CheckboxOptions{ .enabled = enabled, .id = id }, x);
    keep(ours, e);
    shakes(ours, element, WidgetKind::CHECKBOX, name_of(text, id));
    pointer_calls(e, on_hover, on_unhover);
    focus_calls(e, on_focus, on_blur);
    if (changed) on_change(checked);
    return changed;
}

void Checkbox::shake() { ++ours.shakes; }

// ---------------------------------------------------------------------------

bool Slider::show() {
    detail::Edges e;
    uint32_t element = 0;
    const detail::Extra x = extra_for(style, ours, e, element, id);
    const SliderOptions o{ .width = width,
                           .step = step,
                           .enabled = enabled,
                           .show_value = show_value,
                           .id = id };
    const bool changed = detail::slider_with(text, value, min, max, o, x);
    keep(ours, e);
    shakes(ours, element, WidgetKind::SLIDER, name_of(text, id));
    pointer_calls(e, on_hover, on_unhover);
    focus_calls(e, on_focus, on_blur);
    if (changed) on_change(value);
    return changed;
}

void Slider::shake() { ++ours.shakes; }

// ---------------------------------------------------------------------------

bool Dropdown::show() {
    detail::Edges e;
    uint32_t element = 0;
    const detail::Extra x = extra_for(style, ours, e, element, id);
    std::vector<std::string_view> views(items.begin(), items.end());
    const bool changed = detail::dropdown_with(
        text, selected, views,
        DropdownOptions{ .width = width, .enabled = enabled, .id = id }, x);
    keep(ours, e);
    shakes(ours, element, WidgetKind::DROPDOWN, name_of(text, id));
    pointer_calls(e, on_hover, on_unhover);
    focus_calls(e, on_focus, on_blur);
    if (changed) on_change(selected);
    return changed;
}

void Dropdown::shake() { ++ours.shakes; }

// ---------------------------------------------------------------------------

bool TextInput::show() {
    detail::Edges e;
    uint32_t element = 0;
    const detail::Extra x = extra_for(style, ours, e, element, id);
    const TextInputOptions o{ .width = width,
                              .enabled = enabled,
                              .placeholder = placeholder,
                              .id = id,
                              .max_length = max_length };
    const bool changed = detail::text_input_with(text, value, o, x);
    keep(ours, e);
    shakes(ours, element, WidgetKind::TEXT_INPUT, name_of(text, id));
    pointer_calls(e, on_hover, on_unhover);
    focus_calls(e, on_focus, on_blur);
    if (changed) on_change(value);
    return changed;
}

void TextInput::shake() { ++ours.shakes; }

// ---------------------------------------------------------------------------

void ProgressBar::show() {
    detail::Edges e;
    uint32_t element = 0;
    const detail::Extra x = extra_for(style, ours, e, element, id);
    const ProgressOptions o{
        .width = width, .height = height, .radius = radius, .fill = fill, .id = id
    };
    detail::progress_with(value, o, fill_picture.empty() ? nullptr : &fill_picture, x);
    keep(ours, e);
    shakes(ours, element, WidgetKind::PROGRESS, id);
    pointer_calls(e, on_hover, on_unhover);
}

void ProgressBar::shake() { ++ours.shakes; }

} // namespace rmp::ui
