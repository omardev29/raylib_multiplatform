// ===========================================================================
// Controls that own a value: checkbox, slider, dropdown, text input.
//
// Each one takes a pointer to the caller's variable. That is the entire state
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
#include <cstring>

namespace rmp::ui {

namespace {

using detail::px;
using detail::to_clay;

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
    if (!on) return;
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
    float r = px(t.corner_radius);
    d.cornerRadius = Clay_CornerRadius{ r, r, r, r };
    focus_border(d, has_focus);
    return d;
}

} // namespace

// ---------------------------------------------------------------------------
// checkbox
// ---------------------------------------------------------------------------

bool checkbox(std::string_view label, bool *value) {
    return checkbox(label, value, CheckboxOptions{});
}

bool checkbox(std::string_view label, bool *value, const CheckboxOptions &o) {
    if (!detail::frame_open() || value == nullptr) return false;
    const Theme &t = current_theme();

    Clay_ElementId id = detail::element_id(label, o.id);
    const bool over = o.enabled && detail::pointer_present() && Clay_PointerOver(id);
    if (over) detail::set_pointer_over_ui();

    const bool has_focus = o.enabled && detail::focusable(id, label);
    bool toggled = false;
    if (over && detail::pointer_released()) toggled = true;
    if (has_focus && detail::take_activate()) toggled = true;
    if (toggled) *value = !*value;

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
        const float on = detail::anim_value(detail::sub_id(id, 7), 0, *value);
        box.backgroundColor = to_clay(
            !o.enabled ? t.disabled : detail::mix_color(t.surface, t.primary, on));
        float r = px(t.corner_radius * 0.5f);
        box.cornerRadius = Clay_CornerRadius{ r, r, r, r };
        auto bw = static_cast<uint16_t>(px(1.5f));
        box.border.color = to_clay(*value ? t.primary : t.border);
        box.border.width = Clay_BorderWidth{ bw, bw, bw, bw, 0 };

        Clay__OpenElement();
        Clay__ConfigureOpenElement(box);
        if (*value) {
            Clay_ElementDeclaration dot{};
            dot.layout.sizing.width = fixed(t.control_size * 0.4f);
            dot.layout.sizing.height = fixed(t.control_size * 0.4f);
            dot.backgroundColor = to_clay(t.text_on_accent);
            float dr = px(t.control_size * 0.2f);
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

bool slider(std::string_view label, float *value, float min, float max) {
    return slider(label, value, min, max, SliderOptions{});
}

bool slider(std::string_view label, float *value, float min, float max,
            const SliderOptions &o) {
    if (!detail::frame_open() || value == nullptr || max <= min) return false;
    const Theme &t = current_theme();

    Clay_ElementId id = detail::element_id(label, o.id);
    Clay_ElementId track_id = detail::sub_id(id, 0); // the slider's rail

    const bool has_focus = o.enabled && detail::focusable(id, label);
    const float span = max - min;
    const float before = *value;

    // Dragging. The track's box comes from last frame, which is the same
    // tolerance every other interaction here has, and at 60 fps it is invisible
    // even while dragging fast.
    detail::WidgetState *st = detail::state_for(id.id);
    Clay_BoundingBox box{};
    const bool have_box = detail::bounds_of_id(track_id, &box);

    if (o.enabled && have_box && detail::pointer_present()) {
        Clay_Vector2 p = detail::pointer_position();
        const bool inside = p.x >= box.x && p.x <= box.x + box.width &&
            p.y >= box.y - px(8) && p.y <= box.y + box.height + px(8);
        if (inside) detail::set_pointer_over_ui();
        if (inside && detail::pointer_just_pressed()) st->flag = true;
        if (!detail::pointer_down()) st->flag = false;

        if (st->flag) {
            detail::set_pointer_captured(true);
            float fraction = box.width > 0 ? (p.x - box.x) / box.width : 0.0f;
            if (fraction < 0) fraction = 0;
            if (fraction > 1) fraction = 1;
            *value = min + fraction * span;
        }
    } else {
        st->flag = false;
    }
    if (!st->flag) detail::set_pointer_captured(false);

    // Left/right on the keyboard or the stick. A step of 5% keeps a controller
    // usable on a range of any size without needing a per-slider setting.
    if (has_focus && o.enabled) {
        int nav = detail::nav_axis_x();
        if (nav != 0) {
            float step = o.step > 0 ? o.step : span * 0.05f;
            *value += static_cast<float>(nav) * step * GetFrameTime() * 12.0f;
        }
    }

    if (o.step > 0) {
        // std::lround, not (int)(x + 0.5f): the second rounds the wrong way for
        // negative values, and a slider whose range crosses zero has them. The
        // clamps below still put the result back inside [min, max].
        float steps = (*value - min) / o.step;
        *value = min + static_cast<float>(std::lround(steps)) * o.step;
    }
    if (*value < min) *value = min;
    if (*value > max) *value = max;

    const float fraction = (*value - min) / span;

    Clay_ElementDeclaration row = control_row(has_focus);
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
            float rr = px(t.track_thickness * 0.5f);
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

    return *value != before;
}

// ---------------------------------------------------------------------------
// dropdown
// ---------------------------------------------------------------------------

bool dropdown(std::string_view label, int *selected, const char *const *items,
              int count) {
    return dropdown(label, selected, items, count, DropdownOptions{});
}

bool dropdown(std::string_view label, int *selected, const char *const *items, int count,
              const DropdownOptions &o) {
    if (!detail::frame_open() || selected == nullptr || items == nullptr || count <= 0) {
        return false;
    }
    const Theme &t = current_theme();
    if (*selected < 0) *selected = 0;
    if (*selected >= count) *selected = count - 1;

    Clay_ElementId id = detail::element_id(label, o.id);
    detail::WidgetState *st = detail::state_for(id.id);

    const bool over = o.enabled && detail::pointer_present() && Clay_PointerOver(id);
    if (over) detail::set_pointer_over_ui();
    const bool has_focus = o.enabled && detail::focusable(id, label);

    // Item ids are derived from THIS dropdown's id, not from the item text.
    // Hashing the text would give every dropdown with a "Low" in it the same
    // element: hover one, the other lights up, and a click could land in the
    // wrong list entirely.
    bool over_any_item = false;
    if (st->flag && detail::pointer_present()) {
        for (int i = 0; i < count; i++) {
            if (Clay_PointerOver(detail::sub_id(id, static_cast<uint32_t>(i) + 1))) {
                over_any_item = true;
                break;
            }
        }
    }

    bool changed = false;
    if ((over && detail::pointer_released()) || (has_focus && detail::take_activate())) {
        st->flag = !st->flag;
    } else if (st->flag && detail::pointer_released() && !over_any_item) {
        // Released somewhere else entirely. An open list that will not go away
        // when you click past it is the single most irritating thing a dropdown
        // can do.
        st->flag = false;
    }

    Clay_ElementDeclaration row = control_row(has_focus);
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
            detail::state_color(detail::sub_id(id, 8), t.surface, t.surface_hover,
                                t.surface_press, over, over && detail::pointer_down()));
        float r = px(t.corner_radius);
        field.cornerRadius = Clay_CornerRadius{ r, r, r, r };

        Clay__OpenElement();
        Clay__ConfigureOpenElement(field);
        {
            label_text(std::string_view{ items[*selected] },
                       o.enabled ? t.text : t.disabled_text, t.font_size);

            // The open list floats: it has to overlap whatever is underneath
            // rather than shoving it down the screen, which is the one thing a
            // dropdown must not do.
            if (st->flag) {
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

                Clay__OpenElement();
                Clay__ConfigureOpenElement(menu);
                for (int i = 0; i < count; i++) {
                    Clay_ElementId item_id =
                        detail::sub_id(id, static_cast<uint32_t>(i) + 1);
                    const bool item_over =
                        detail::pointer_present() && Clay_PointerOver(item_id);
                    if (item_over) detail::set_pointer_over_ui();
                    // An open list is in front of the game, so it takes the
                    // pointer whether or not this particular item is under it.
                    detail::set_pointer_over_ui();
                    if (item_over && detail::pointer_released()) {
                        if (*selected != i) changed = true;
                        *selected = i;
                        st->flag = false;
                    }

                    Clay_ElementDeclaration item{};
                    item.layout.sizing.width = grow();
                    item.layout.padding =
                        Clay_Padding{ static_cast<uint16_t>(px(t.padding_x * 0.5f)),
                                      static_cast<uint16_t>(px(t.padding_x * 0.5f)),
                                      static_cast<uint16_t>(px(t.padding_y * 0.5f)),
                                      static_cast<uint16_t>(px(t.padding_y * 0.5f)) };
                    item.backgroundColor = to_clay(detail::state_color(
                        item_id, (i == *selected) ? t.surface : t.panel, t.surface_hover,
                        t.surface_press, item_over, item_over && detail::pointer_down()));
                    item.cornerRadius =
                        Clay_CornerRadius{ r * 0.5f, r * 0.5f, r * 0.5f, r * 0.5f };
                    Clay__OpenElementWithId(item_id);
                    Clay__ConfigureOpenElement(item);
                    label_text(std::string_view{ items[i] }, t.text, t.font_size);
                    Clay__CloseElement();
                }
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

bool text_input(std::string_view label, char *buffer, int capacity) {
    return text_input(label, buffer, capacity, TextInputOptions{});
}

bool text_input(std::string_view label, char *buffer, int capacity,
                const TextInputOptions &o) {
    if (!detail::frame_open() || buffer == nullptr || capacity < 2) return false;
    const Theme &t = current_theme();

    Clay_ElementId id = detail::element_id(label, o.id);
    const bool over = o.enabled && detail::pointer_present() && Clay_PointerOver(id);
    if (over) detail::set_pointer_over_ui();

    const bool has_focus = o.enabled && detail::focusable(id, label);
    if (over && detail::pointer_released()) detail::focus_by_id(id.id, label);

    bool changed = false;
    int len = static_cast<int>(std::strlen(buffer));

    if (has_focus && o.enabled) {
        // While a field has focus the keyboard is its own: Tab and the arrows
        // stop meaning "move to the next control", and the game is told to keep
        // its hands off via wants_keyboard().
        detail::set_keyboard_captured(true);

        int c;
        while ((c = GetCharPressed()) != 0) {
            if (c >= 32 && c < 127 && len < capacity - 1) {
                buffer[len++] = static_cast<char>(c);
                buffer[len] = '\0';
                changed = true;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            if (len > 0) {
                buffer[--len] = '\0';
                changed = true;
            }
        }
    }

    Clay_ElementDeclaration row = control_row(has_focus);
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
        float r = px(t.corner_radius);
        field.cornerRadius = Clay_CornerRadius{ r, r, r, r };

        Clay__OpenElement();
        Clay__ConfigureOpenElement(field);
        {
            if (len == 0 && !o.placeholder.empty() && !has_focus) {
                label_text(o.placeholder, t.text_muted, t.font_size);
            } else {
                // The caret is a character rather than a drawn rectangle: it
                // costs no render command, and it blinks by not being appended
                // half the time.
                char shown[512];
                // Clamped in both directions rather than just the top. len is
                // never negative today, but nothing here enforces that, and a
                // negative n turns the memcpy below into a very large one.
                int n = len;
                if (n < 0) n = 0;
                if (n > 500) n = 500;
                std::memcpy(shown, buffer, static_cast<size_t>(n));
                bool caret_on = has_focus && (static_cast<int>(GetTime() * 2.0) % 2) == 0;
                if (caret_on) shown[n++] = '_';
                shown[n] = '\0';
                label_text(std::string_view{ shown, static_cast<size_t>(n) },
                           o.enabled ? t.text : t.disabled_text, t.font_size);
            }
        }
        Clay__CloseElement();
    }
    Clay__CloseElement();

    return changed;
}

} // namespace rmp::ui
