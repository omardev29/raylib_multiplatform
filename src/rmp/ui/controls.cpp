// ===========================================================================
// Controls that own a value: checkbox, slider, dropdown, text input.
//
// Each one takes the caller's variable by reference. That is the entire state
// model — there is nothing of ours to keep in sync, and what is on screen is
// what is in their struct because it was read this frame.
//
// What they do keep is UI state that is nobody's business but ours: whether a
// dropdown is open, where a text caret is. That lives in detail::state_for(),
// keyed by element id, so the caller never has to hold a variable that means
// nothing to their game.
// ===========================================================================

#include "internal.h"

#include <cstdio>
#include <cmath>
#include <string>

namespace rmp::ui {

namespace {

using detail::px;
using detail::to_clay;

// A click is a press AND a release on the same element, exactly as button()
// has it. These controls used to answer any release over them, so a drag that
// started in the game and ended on a settings panel ticked whatever box it
// ended on. Called every frame the element is declared: it records where a
// press starts, and answers true on the release that ends on the same element.
bool clicked(Clay_ElementId id, bool over) {
    if (over && detail::pointer_just_pressed()) detail::set_press_id(id.id);
    return over && detail::pointer_released() && detail::press_id() == id.id;
}

Clay_SizingAxis fixed(float v) {
    Clay_SizingAxis a{};
    a.type = CLAY__SIZING_TYPE_FIXED;
    a.size.minMax = Clay_SizingMinMax{ px(v), px(v) };
    return a;
}

Clay_SizingAxis fit() {
    Clay_SizingAxis a{};
    a.type = CLAY__SIZING_TYPE_FIT;
    return a;
}

Clay_SizingAxis grow() {
    Clay_SizingAxis a{};
    a.type = CLAY__SIZING_TYPE_GROW;
    return a;
}

void focus_border(Clay_ElementDeclaration &d, bool on) {
    if (!on || !detail::focus_visible()) return;
    const Theme &t = current_theme();
    auto w = static_cast<uint16_t>(px(t.focus_ring));
    d.border.color = to_clay(t.focus);
    d.border.width = Clay_BorderWidth{ w, w, w, w, 0 };
}

void label_text(std::string_view s, Color c, float size) {
    Clay_TextElementConfig tc{};
    tc.textColor = to_clay(c);
    tc.fontSize = static_cast<uint16_t>(px(size));
    tc.wrapMode = CLAY_TEXT_WRAP_NONE;
    Clay__OpenTextElement(detail::intern(s), tc);
}

// The row every one of these controls sits in: label on the left, the control
// itself on the right, the whole thing focusable as one unit.
// The sub-id of a dropdown's open list. Far away from the items, which take 1
// to count: a list of nine things would otherwise collide with it.
constexpr uint32_t MENU_SUB = 0x10000u;

Clay_ElementDeclaration control_row(bool has_focus) {
    const Theme &t = current_theme();
    Clay_ElementDeclaration d{};
    d.layout.sizing.width = grow();
    d.layout.sizing.height = fit();
    d.layout.childGap = static_cast<uint16_t>(px(t.gap));
    d.layout.padding = Clay_Padding{ static_cast<uint16_t>(px(t.padding_y)),
                                     static_cast<uint16_t>(px(t.padding_y)),
                                     static_cast<uint16_t>(px(t.padding_y * 0.5f)),
                                     static_cast<uint16_t>(px(t.padding_y * 0.5f)) };
    d.layout.childAlignment =
        Clay_ChildAlignment{ CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER };
    d.layout.layoutDirection = CLAY_LEFT_TO_RIGHT;
    const float r = px(t.corner_radius);
    d.cornerRadius = Clay_CornerRadius{ r, r, r, r };
    focus_border(d, has_focus);
    return d;
}

} // namespace

// ---------------------------------------------------------------------------
// checkbox
// ---------------------------------------------------------------------------

bool checkbox(std::string_view label, bool &value) {
    return checkbox(label, value, CheckboxOptions{});
}

bool checkbox(std::string_view label, bool &value, const CheckboxOptions &o) {
    if (!detail::frame_open()) return false;
    const Theme &t = current_theme();

    const Clay_ElementId id = detail::element_id(label, o.id);
    const bool over = o.enabled && detail::pointer_over(id);
    if (over) detail::set_pointer_over_ui();

    const bool has_focus = o.enabled && detail::focusable(id, label);
    bool toggled = clicked(id, over);
    if (has_focus && detail::take_activate()) toggled = true;
    if (toggled) value = !value;

    Clay_ElementDeclaration row = control_row(has_focus);
    // The row is transparent at rest, so it fades in from surface_hover with
    // nothing in it — from black would flash dark before it lit up.
    row.backgroundColor = to_clay(
        detail::state_color(id, detail::clear_alpha(t.surface_hover), t.surface_hover,
                            t.surface_press, over, over && detail::pointer_down()));

    Clay__OpenElementWithId(id);
    Clay__ConfigureOpenElement(row);
    {
        // The box. Filled when on, outlined when off — a shape you can read at
        // a glance without a tick glyph, which the built-in font does not have.
        Clay_ElementDeclaration box{};
        box.layout.sizing.width = fixed(t.control_size);
        box.layout.sizing.height = fixed(t.control_size);
        box.layout.childAlignment =
            Clay_ChildAlignment{ CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER };
        // The fill follows the value rather than the pointer, so ticking a box
        // reads as the box filling in instead of swapping colour between two
        // frames. Its own sub-id, so it does not share a slot with the row.
        const float on = detail::anim_value(detail::peek_sub_id(id, 7), 0, value);
        box.backgroundColor = to_clay(
            !o.enabled ? t.disabled : detail::mix_color(t.surface, t.primary, on));
        const float r = px(t.corner_radius * 0.5f);
        box.cornerRadius = Clay_CornerRadius{ r, r, r, r };
        auto bw = static_cast<uint16_t>(px(1.5f));
        box.border.color = to_clay(value ? t.primary : t.border);
        box.border.width = Clay_BorderWidth{ bw, bw, bw, bw, 0 };

        Clay__OpenElement();
        Clay__ConfigureOpenElement(box);
        if (value) {
            Clay_ElementDeclaration dot{};
            dot.layout.sizing.width = fixed(t.control_size * 0.4f);
            dot.layout.sizing.height = fixed(t.control_size * 0.4f);
            dot.backgroundColor = to_clay(t.text_on_accent);
            const float dr = px(t.control_size * 0.2f);
            dot.cornerRadius = Clay_CornerRadius{ dr, dr, dr, dr };
            Clay__OpenElement();
            Clay__ConfigureOpenElement(dot);
            Clay__CloseElement();
        }
        Clay__CloseElement();

        label_text(label, o.enabled ? t.text : t.disabled_text, t.font_size);
    }
    Clay__CloseElement();

    return toggled;
}

// ---------------------------------------------------------------------------
// slider
// ---------------------------------------------------------------------------

bool slider(std::string_view label, float &value, float min, float max) {
    return slider(label, value, min, max, SliderOptions{});
}

bool slider(std::string_view label, float &value, float min, float max,
            const SliderOptions &o) {
    if (!detail::frame_open() || max <= min) return false;
    const Theme &t = current_theme();

    const Clay_ElementId id = detail::element_id(label, o.id);
    const Clay_ElementId track_id = detail::sub_id(id, 0); // the slider's rail

    const bool has_focus = o.enabled && detail::focusable(id, label);
    const float span = max - min;
    const float before = value;

    // Dragging. The track's box comes from last frame, which is the same
    // tolerance every other interaction here has, and at 60 fps it is invisible
    // even while dragging fast.
    detail::WidgetState *st = detail::state_for(id.id);
    Clay_BoundingBox box{};
    const bool have_box = detail::bounds_of_id(track_id, &box);

    if (o.enabled && have_box) {
        const Clay_Vector2 p = detail::pointer_position();
        // The rail is thinner than a finger, so the box it is hit-tested
        // against is taller than the rail.
        const bool inside = detail::pointer_over(track_id, px(8));
        if (inside) detail::set_pointer_over_ui();
        if (inside && detail::pointer_just_pressed()) st->flag = true;
        if (!detail::pointer_down()) st->flag = false;

        if (st->flag) {
            detail::set_pointer_captured(id.id, true);
            float fraction = box.width > 0 ? (p.x - box.x) / box.width : 0.0f;
            if (fraction < 0) fraction = 0;
            if (fraction > 1) fraction = 1;
            value = min + fraction * span;
        }
    } else {
        st->flag = false;
    }
    if (!st->flag) detail::set_pointer_captured(id.id, false);

    // Left/right on the keyboard or the stick. A step of 5% keeps a controller
    // usable on a range of any size without needing a per-slider setting.
    //
    // ONE STEP PER PRESS, then a repeat while it is held. It used to be a
    // continuous nudge of step * dt * 12, which for a slider with a step is a
    // fifth of one at 60 fps — less than the half a step the snap below needs,
    // and the remainder was thrown away with the rest of the value. A stepped
    // slider could not be moved by keyboard or gamepad at all, at any frame
    // rate above about 42, which reads as "the controller does not work on this
    // one" and leaves a TV build with no way to change it.
    if (has_focus && o.enabled) {
        const int nav = detail::nav_axis_x();
        const float step = o.step > 0 ? o.step : span * 0.05f;
        if (nav == 0) {
            st->i = 0;
            st->f = 0.0f;
        } else if (nav != st->i) {
            st->i = nav;
            // Negative, so the second step does not follow the first
            // immediately: a quarter of a second at the 12-a-second rate below.
            st->f = -3.0f;
            value += static_cast<float>(nav) * step;
        } else {
            // Held. Whole steps only, and the fraction left over is kept rather
            // than rounded away — that discarded remainder was the bug.
            st->f += detail::frame_time() * 12.0f;
            while (st->f >= 1.0f) {
                st->f -= 1.0f;
                value += static_cast<float>(nav) * step;
            }
        }
    }

    if (o.step > 0) {
        // std::lround, not (int)(x + 0.5f): the second rounds the wrong way for
        // negative values, and a slider whose range crosses zero has them. The
        // clamps below still put the result back inside [min, max].
        const float steps = (value - min) / o.step;
        value = min + static_cast<float>(std::lround(steps)) * o.step;
    }
    if (value < min) value = min;
    if (value > max) value = max;

    const float fraction = (value - min) / span;

    const Clay_ElementDeclaration row = control_row(has_focus);
    Clay__OpenElementWithId(id);
    Clay__ConfigureOpenElement(row);
    {
        label_text(label, o.enabled ? t.text : t.disabled_text, t.font_size);

        Clay_ElementDeclaration track{};
        track.layout.sizing.width = o.width > 0 ? fixed(o.width) : grow();
        track.layout.sizing.height = fixed(t.control_size);
        track.layout.childAlignment =
            Clay_ChildAlignment{ CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER };
        Clay__OpenElementWithId(track_id);
        Clay__ConfigureOpenElement(track);
        {
            // The rail, and the filled part of it. Two elements rather than one
            // so the handle can sit on top of both without arithmetic.
            Clay_ElementDeclaration rail{};
            rail.layout.sizing.width = grow();
            rail.layout.sizing.height = fixed(t.track_thickness);
            rail.backgroundColor = to_clay(t.surface);
            const float rr = px(t.track_thickness * 0.5f);
            rail.cornerRadius = Clay_CornerRadius{ rr, rr, rr, rr };
            Clay__OpenElement();
            Clay__ConfigureOpenElement(rail);
            {
                Clay_ElementDeclaration fill{};
                Clay_SizingAxis w{};
                w.type = CLAY__SIZING_TYPE_PERCENT;
                w.size.percent = fraction;
                fill.layout.sizing.width = w;
                fill.layout.sizing.height = grow();
                fill.backgroundColor = to_clay(o.enabled ? t.primary : t.disabled);
                fill.cornerRadius = Clay_CornerRadius{ rr, rr, rr, rr };
                Clay__OpenElement();
                Clay__ConfigureOpenElement(fill);
                Clay__CloseElement();
            }
            Clay__CloseElement();
        }
        Clay__CloseElement();

        if (o.show_value) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0f%%", fraction * 100.0f);
            label_text(std::string_view{ buf }, t.text_muted, t.font_size_small);
        }
    }
    Clay__CloseElement();

    return value != before;
}

// ---------------------------------------------------------------------------
// dropdown
// ---------------------------------------------------------------------------

bool dropdown(std::string_view label, int &selected,
              std::span<const std::string_view> items) {
    return dropdown(label, selected, items, DropdownOptions{});
}

bool dropdown(std::string_view label, int &selected,
              std::initializer_list<std::string_view> items) {
    return dropdown(label, selected, std::span(items.begin(), items.size()),
                    DropdownOptions{});
}

bool dropdown(std::string_view label, int &selected,
              std::initializer_list<std::string_view> items, const DropdownOptions &o) {
    return dropdown(label, selected, std::span(items.begin(), items.size()), o);
}

bool dropdown(std::string_view label, int &selected,
              std::span<const std::string_view> items, const DropdownOptions &o) {
    if (!detail::frame_open() || items.empty()) return false;
    const Theme &t = current_theme();
    // An int, because that is what a game keeps; a list longer than an int
    // counts is not a dropdown anyone can use.
    const int count = static_cast<int>(items.size());
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;

    const Clay_ElementId id = detail::element_id(label, o.id);
    detail::WidgetState *st = detail::state_for(id.id);

    const bool over = o.enabled && detail::pointer_over(id);
    if (over) detail::set_pointer_over_ui();
    const bool has_focus = o.enabled && detail::focusable(id, label);

    // Item ids are derived from THIS dropdown's id, not from the item text.
    // Hashing the text would give every dropdown with a "Low" in it the same
    // element: hover one, the other lights up, and a click could land in the
    // wrong list entirely.
    bool over_any_item = false;
    if (st->flag && detail::pointer_present()) {
        for (int i = 0; i < count; ++i) {
            if (detail::pointer_over(
                    detail::peek_sub_id(id, static_cast<uint32_t>(i) + 1))) {
                over_any_item = true;
                break;
            }
        }
    }

    // st->flag is open or closed; st->i is the item the keyboard or the
    // gamepad is on while it is open, starting from the value.
    bool changed = false;
    const bool field_clicked = clicked(id, over);
    // Escape or B: closed, and nothing picked. Ahead of everything else.
    const bool cancelled = st->flag && has_focus && detail::take_cancel();
    if (field_clicked && !cancelled) {
        st->flag = !st->flag;
        st->i = selected;
    } else if (!cancelled && has_focus && detail::take_activate()) {
        if (st->flag) {
            // Accept picks what the list was walked to, as a click would.
            if (selected != st->i) changed = true;
            selected = st->i;
            st->flag = false;
        } else {
            st->flag = true;
            st->i = selected;
        }
    } else if (cancelled || (st->flag && detail::pointer_released() && !over_any_item)) {
        // Or released somewhere else entirely. An open list that will not go
        // away when you click past it is the single most irritating thing a
        // dropdown can do.
        st->flag = false;
    }

    // Open and focused, up and down walk the items instead of the focus: a
    // list a controller can open, it has to be able to pick from.
    const bool walking = st->flag && has_focus;
    if (walking) {
        detail::claim_vertical(id.id);
        st->i += detail::nav_axis_y();
        if (st->i < 0) st->i = 0;
        if (st->i >= count) st->i = count - 1;
    }

    const Clay_ElementDeclaration row = control_row(has_focus);
    Clay__OpenElementWithId(id);
    Clay__ConfigureOpenElement(row);
    {
        label_text(label, o.enabled ? t.text : t.disabled_text, t.font_size);

        Clay_ElementDeclaration field{};
        field.layout.sizing.width = o.width > 0 ? fixed(o.width) : grow();
        field.layout.sizing.height = fit();
        field.layout.padding =
            Clay_Padding{ static_cast<uint16_t>(px(t.padding_x * 0.5f)),
                          static_cast<uint16_t>(px(t.padding_x * 0.5f)),
                          static_cast<uint16_t>(px(t.padding_y * 0.5f)),
                          static_cast<uint16_t>(px(t.padding_y * 0.5f)) };
        field.layout.childAlignment =
            Clay_ChildAlignment{ CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER };
        field.backgroundColor = to_clay(
            detail::state_color(detail::peek_sub_id(id, 8), t.surface, t.surface_hover,
                                t.surface_press, over, over && detail::pointer_down()));
        const float r = px(t.corner_radius);
        field.cornerRadius = Clay_CornerRadius{ r, r, r, r };

        Clay__OpenElement();
        Clay__ConfigureOpenElement(field);
        {
            label_text(items[static_cast<std::size_t>(selected)],
                       o.enabled ? t.text : t.disabled_text, t.font_size);

            // The open list floats: it has to overlap whatever is underneath
            // rather than shoving it down the screen, which is the one thing a
            // dropdown must not do.
            if (st->flag) {
                // The open list is in front of everything else in this pass and
                // it takes the pointer, which a box test cannot work out on its
                // own — so it says so. Without it a click meant for the list
                // would also press whatever is behind it.
                const Clay_ElementId menu_id = detail::sub_id(id, MENU_SUB);
                detail::block_pointer(menu_id);

                Clay_ElementDeclaration menu{};
                menu.layout.sizing.width = grow();
                menu.layout.layoutDirection = CLAY_TOP_TO_BOTTOM;
                menu.layout.padding = Clay_Padding{ 2, 2, 2, 2 };
                menu.backgroundColor = to_clay(t.panel);
                menu.cornerRadius = Clay_CornerRadius{ r, r, r, r };
                menu.floating.attachTo = CLAY_ATTACH_TO_PARENT;
                menu.floating.zIndex = 1000;
                menu.floating.attachPoints =
                    Clay_FloatingAttachPoints{ CLAY_ATTACH_POINT_LEFT_TOP,
                                               CLAY_ATTACH_POINT_LEFT_BOTTOM };
                auto bw = static_cast<uint16_t>(px(1));
                menu.border.color = to_clay(t.border);
                menu.border.width = Clay_BorderWidth{ bw, bw, bw, bw, 0 };

                Clay__OpenElementWithId(menu_id);
                Clay__ConfigureOpenElement(menu);
                // Everything in the list belongs to the list, which is what
                // makes the items themselves exempt from the block above.
                detail::push_clip(menu_id);
                for (int i = 0; i < count; ++i) {
                    const Clay_ElementId item_id =
                        detail::sub_id(id, static_cast<uint32_t>(i) + 1);
                    const bool item_over = detail::pointer_over(item_id);
                    if (item_over) detail::set_pointer_over_ui();
                    // An open list is in front of the game, so it takes the
                    // pointer whether or not this particular item is under it.
                    detail::set_pointer_over_ui();
                    if (clicked(item_id, item_over)) {
                        if (selected != i) changed = true;
                        selected = i;
                        st->flag = false;
                    }

                    Clay_ElementDeclaration item{};
                    item.layout.sizing.width = grow();
                    item.layout.padding =
                        Clay_Padding{ static_cast<uint16_t>(px(t.padding_x * 0.5f)),
                                      static_cast<uint16_t>(px(t.padding_x * 0.5f)),
                                      static_cast<uint16_t>(px(t.padding_y * 0.5f)),
                                      static_cast<uint16_t>(px(t.padding_y * 0.5f)) };
                    // The item the keyboard is on lights up like the one under
                    // the pointer, once the player has reached for a key.
                    const bool lit =
                        item_over || (walking && detail::focus_visible() && i == st->i);
                    item.backgroundColor = to_clay(detail::state_color(
                        item_id, (i == selected) ? t.surface : t.panel, t.surface_hover,
                        t.surface_press, lit, item_over && detail::pointer_down()));
                    item.cornerRadius =
                        Clay_CornerRadius{ r * 0.5f, r * 0.5f, r * 0.5f, r * 0.5f };
                    Clay__OpenElementWithId(item_id);
                    Clay__ConfigureOpenElement(item);
                    label_text(items[static_cast<std::size_t>(i)], t.text, t.font_size);
                    Clay__CloseElement();
                }
                detail::pop_clip();
                Clay__CloseElement();
            }
        }
        Clay__CloseElement();
    }
    Clay__CloseElement();

    return changed;
}

// ---------------------------------------------------------------------------
// text input
// ---------------------------------------------------------------------------

bool text_input(std::string_view label, std::string &value) {
    return text_input(label, value, TextInputOptions{});
}

bool text_input(std::string_view label, std::string &value, const TextInputOptions &o) {
    if (!detail::frame_open()) return false;
    const Theme &t = current_theme();

    const Clay_ElementId id = detail::element_id(label, o.id);
    const bool over = o.enabled && detail::pointer_over(id);
    if (over) detail::set_pointer_over_ui();

    const bool has_focus = o.enabled && detail::focusable(id, label);
    const bool click = clicked(id, over);

    // Typing is having the keyboard, not having the focus. The focus can rest
    // on a field that is not typing -- after Enter or Escape -- and Tab, up and
    // down move it on from a field exactly as from anything else.
    bool typing = o.enabled && detail::has_keyboard(id.id);
    bool took = false;
    if (typing) {
        // Given back by Enter or the A button (not Space: that is a character),
        // Escape or the B button, or a press anywhere else. The focus stays.
        if (detail::take_submit() || detail::take_cancel() ||
            (detail::pointer_just_pressed() && !over)) {
            detail::release_keyboard(id.id);
            typing = false;
        }
    } else if (o.enabled && click) {
        detail::focus_by_id(id.id, label); // which hands it the keyboard
        typing = took = true;
    } else if (has_focus && detail::take_activate()) {
        detail::take_keyboard(id.id);
        typing = took = true;
    }

    // A value longer than the field may hold -- handed in that way, or a
    // max_length that came down -- is cut to it here, at the end of a
    // character, and that is a change like any other.
    bool changed = detail::utf8_truncate(value, o.max_length);

    if (typing) {
        // The game is told to keep its hands off via wants_keyboard().
        detail::set_keyboard_captured(true);
    }
    // Not on the frame it took the keyboard: the Space that took it is in the
    // character queue too.
    if (typing && !took) {
        int c = 0;
        while ((c = GetCharPressed()) != 0) {
            if (detail::utf8_append(value, c, o.max_length)) changed = true;
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            if (detail::utf8_pop(value)) changed = true;
        }
    }

    const Clay_ElementDeclaration row = control_row(has_focus);
    Clay__OpenElementWithId(id);
    Clay__ConfigureOpenElement(row);
    {
        if (!label.empty()) {
            label_text(label, o.enabled ? t.text : t.disabled_text, t.font_size);
        }

        Clay_ElementDeclaration field{};
        field.layout.sizing.width = o.width > 0 ? fixed(o.width) : grow();
        field.layout.sizing.height = fit();
        field.layout.padding =
            Clay_Padding{ static_cast<uint16_t>(px(t.padding_x * 0.5f)),
                          static_cast<uint16_t>(px(t.padding_x * 0.5f)),
                          static_cast<uint16_t>(px(t.padding_y * 0.5f)),
                          static_cast<uint16_t>(px(t.padding_y * 0.5f)) };
        field.layout.childAlignment =
            Clay_ChildAlignment{ CLAY_ALIGN_X_LEFT, CLAY_ALIGN_Y_CENTER };
        field.backgroundColor = to_clay(o.enabled ? t.surface : t.disabled);
        const float r = px(t.corner_radius);
        field.cornerRadius = Clay_CornerRadius{ r, r, r, r };

        Clay__OpenElement();
        Clay__ConfigureOpenElement(field);
        {
            if (value.empty() && !o.placeholder.empty() && !typing) {
                label_text(o.placeholder, t.text_muted, t.font_size);
            } else {
                // The caret is a character rather than a drawn rectangle: it
                // costs no render command, and it blinks by not being appended
                // half the time.
                const bool caret_on =
                    typing && (static_cast<int>(GetTime() * 2.0) % 2) == 0;
                label_text(caret_on ? value + "_" : value,
                           o.enabled ? t.text : t.disabled_text, t.font_size);
            }
        }
        Clay__CloseElement();
    }
    Clay__CloseElement();

    return changed;
}

// ---------------------------------------------------------------------------
// UTF-8, for the text field
// ---------------------------------------------------------------------------

namespace detail {

namespace {

// A byte that continues a character rather than starting one: 10xxxxxx.
bool continues(char c) { return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U; }

} // namespace

int utf8_length(std::string_view text) {
    int n = 0;
    for (const char c : text) {
        if (!continues(c)) ++n;
    }
    return n;
}

bool utf8_append(std::string &text, int codepoint, int max_length) {
    // What a player types: no control characters (C0, DEL, C1), no half of
    // a surrogate pair, nothing past the last codepoint there is.
    const bool control = codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0);
    const bool surrogate = codepoint >= 0xD800 && codepoint <= 0xDFFF;
    if (control || surrogate || codepoint > 0x10FFFF) return false;
    if (max_length > 0 && utf8_length(text) >= max_length) return false;

    const auto cp = static_cast<unsigned>(codepoint);
    const auto byte = [](unsigned bits) { return static_cast<char>(bits); };
    if (cp < 0x80U) {
        text += byte(cp);
    } else if (cp < 0x800U) {
        text += byte(0xC0U | (cp >> 6U));
        text += byte(0x80U | (cp & 0x3FU));
    } else if (cp < 0x10000U) {
        text += byte(0xE0U | (cp >> 12U));
        text += byte(0x80U | ((cp >> 6U) & 0x3FU));
        text += byte(0x80U | (cp & 0x3FU));
    } else {
        text += byte(0xF0U | (cp >> 18U));
        text += byte(0x80U | ((cp >> 12U) & 0x3FU));
        text += byte(0x80U | ((cp >> 6U) & 0x3FU));
        text += byte(0x80U | (cp & 0x3FU));
    }
    return true;
}

bool utf8_pop(std::string &text) {
    if (text.empty()) return false;
    // Back over the bytes that continue the last character, then its first.
    while (!text.empty() && continues(text.back())) text.pop_back();
    if (!text.empty()) text.pop_back();
    return true;
}

bool utf8_truncate(std::string &text, int max_length) {
    if (max_length <= 0) return false;
    int seen = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (continues(text[i])) continue;
        if (seen == max_length) {
            text.resize(i); // i starts a character: nothing is cut inside one
            return true;
        }
        ++seen;
    }
    return false;
}

} // namespace detail

} // namespace rmp::ui
