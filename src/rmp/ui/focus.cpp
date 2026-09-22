// ===========================================================================
// Focus, keyboard and gamepad navigation.
//
// The point of this file is that no widget implements navigation. A widget
// registers itself as focusable and asks whether it is the focused one; moving
// between them, and deciding what "activate" means on three different input
// devices, happens once, here.
//
// That is also why it can be added after the widgets were written without
// touching their logic — and why a controller build works without anyone
// writing controller code.
// ===========================================================================

#include "internal.h"

#include <cstring>

namespace rmp::ui {

namespace {

// Two lists: the one being built this frame, and last frame's, which is what
// navigation reasons about — the same one-frame-behind rule as hit testing,
// for the same reason. This frame's order is not known until end().
constexpr int kMaxFocusables = 128;

struct Entry {
    uint32_t id = 0;
    char name[48] = { 0 };
};

Entry g_current[kMaxFocusables];
int g_current_count = 0;
Entry g_previous[kMaxFocusables];
int g_previous_count = 0;

uint32_t g_focused_id = 0;
char g_focused_name[48] = { 0 };
// Where the pass being described starts in g_current. See end_pass_focus().
int g_pass_first = 0;
// Is the focus SHOWN? Having one and drawing a ring around it are two
// questions, and they have two answers: everything is focusable now, so a menu
// drawn for a player holding a mouse would wear a ring nobody asked for on its
// first button, every time. It appears the moment the keyboard or the gamepad
// is used -- which is the moment it starts meaning something -- and goes away
// again on a click. The browsers' :focus-visible, for the same reason.
bool g_focus_visible = false;

bool g_navigation_enabled = true;
bool g_activate_pending = false;
int g_nav_x = 0;
bool g_pointer_over_ui = false;
// WHICH element is dragging, or 0. A plain bool meant every slider in the frame
// wrote to the same flag, so the one drawn after the one being dragged handed
// the pointer straight back to the game -- and nothing cleared it at all if the
// dragging slider stopped being drawn.
uint32_t g_pointer_capture_id = 0;
uint32_t g_press_id = 0;
bool g_keyboard_captured = false;

// Held-key repeat, so holding down on a d-pad walks a menu instead of moving
// one item and stopping.
float g_repeat_timer = 0.0f;
int g_last_nav_y = 0;
constexpr float kRepeatDelay = 0.45f;
constexpr float kRepeatInterval = 0.09f;

void copy_name(char *dst, std::string_view s) {
    size_t n = s.size() < 47 ? s.size() : 47;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

int index_of(uint32_t id) {
    for (int i = 0; i < g_previous_count; i++) {
        if (g_previous[i].id == id) return i;
    }
    return -1;
}

void move_focus(int delta) {
    // A local, clamped count. g_previousCount cannot exceed the array today,
    // but reading it into a bounded local is what makes every index below
    // provably inside g_previous[] from this function alone, rather than from
    // an invariant kept somewhere else in the file.
    const int count =
        g_previous_count < kMaxFocusables ? g_previous_count : kMaxFocusables;
    if (count <= 0) return;

    const int at = index_of(g_focused_id);
    int next;
    if (at < 0 || at >= count) {
        // Nothing focused, or what was focused has gone. Entering from the top
        // going down, from the bottom going up, is what a person expects.
        next = delta > 0 ? 0 : count - 1;
    } else {
        // The positive-modulo idiom, rather than adding the count once: that
        // only wraps for a delta of -1, and nothing here says delta is +/-1.
        next = (at + delta) % count;
        if (next < 0) next += count;
    }
    g_focused_id = g_previous[next].id;
    copy_name(g_focused_name, g_previous[next].name);
}

} // namespace

namespace detail {

void begin_focus_frame() {
    g_current_count = 0;
    g_nav_x = 0;
    g_activate_pending = false;

    // A drag and a press both end when the pointer goes up, whatever is or is
    // not being drawn. That is a property of the pointer and not of a pass,
    // which is why they are released here and not with the other two capture
    // flags -- and not on the release frame itself, which is the frame the
    // click is made of.
    if (!pointer_down() && !pointer_released()) {
        g_pointer_capture_id = 0;
        g_press_id = 0;
    }

    if (!g_navigation_enabled) return;

    // Once per frame, through the provider: see NavState. Everything below is
    // logic on top of those three numbers, which is what makes a controller
    // testable without a controller.
    NavState nav{};
    read_nav(&nav);

    // A text field owns the keyboard while it has focus; Tab and the arrows
    // there mean "move the caret", not "leave this field".
    if (!g_keyboard_captured) {
        if (nav.y != 0 && nav.y != g_last_nav_y) {
            move_focus(nav.y);
            g_repeat_timer = kRepeatDelay;
        } else if (nav.y != 0) {
            // frame_time(), not GetFrameTime(): 0 in test mode, so a headless
            // run moves exactly one step per press and always the same way.
            g_repeat_timer -= frame_time();
            if (g_repeat_timer <= 0.0f) {
                move_focus(nav.y);
                g_repeat_timer = kRepeatInterval;
            }
        }
        g_last_nav_y = nav.y;
        g_nav_x = nav.x;
    }

    g_activate_pending = nav.activate;

    // Touching the keyboard or the stick is what makes the focus worth showing;
    // going back to the mouse is what stops it.
    if (nav.x != 0 || nav.y != 0 || nav.activate) g_focus_visible = true;
    if (pointer_just_pressed()) g_focus_visible = false;
}

void end_focus_frame() {
    // This frame's declaration order becomes what the next frame navigates.
    // The count is clamped here rather than trusted. register_focusable() below
    // already refuses to write past the array, so this can only ever be a no-op
    // — but it is the one line that makes the bound a property of the copy
    // instead of something three call sites each have to remember.
    int n = g_current_count;
    if (n > kMaxFocusables) n = kMaxFocusables;
    for (int i = 0; i < n; i++) g_previous[i] = g_current[i];
    g_previous_count = n;

    // If whatever had the focus is no longer on screen, hand it to the first
    // thing that is, rather than leaving a controller with nowhere to go.
    if (g_focused_id != 0 && index_of(g_focused_id) < 0 && g_previous_count > 0) {
        g_focused_id = g_previous[0].id;
        copy_name(g_focused_name, g_previous[0].name);
    }
}

bool focusable(Clay_ElementId id, std::string_view name) {
    // A pass input cannot reach is not focusable either, which is what
    // rmp/ui.h has promised set_pass_input() means all along. Without this the
    // arrows walked into the HUD under an open pause menu, and Enter activated
    // something in a scene the player could not even see the cursor in.
    if (!pass_input()) return false;

    if (g_current_count < kMaxFocusables) {
        g_current[g_current_count].id = id.id;
        copy_name(g_current[g_current_count].name, name);
        g_current_count++;
    }
    return g_navigation_enabled && g_focused_id == id.id;
}

// --- the default focus -----------------------------------------------------
//
// Something has to be focused for Enter or the A button to mean anything, and
// until now nothing was until the player pressed Down or Tab first. A scene
// pushed with one "Play again" button on it could not be pressed with a
// controller at all, and every one of the six example games worked around it by
// calling rmp::ui::focus() in _ready().
//
// So: a pass that declares focusable widgets and has no focus of its own takes
// its FIRST one. "Of its own" is the whole subtlety -- the focus is per frame
// but the passes are not, so what decides is whether the focused element has
// been declared by ANY pass so far this frame. A pause menu over a HUD that
// input cannot reach finds nothing (the HUD registers nothing) and takes its
// own first button; with input_below on, the HUD registers first and keeps it,
// so the menu cannot steal the focus back every frame and Tab still walks
// between the two.

void begin_pass_focus() { g_pass_first = g_current_count; }

void end_pass_focus() {
    if (!g_navigation_enabled) return;
    if (g_current_count <= g_pass_first) return; // nothing focusable in it
    for (int i = 0; i < g_current_count; i++) {
        if (g_current[i].id == g_focused_id) return; // already somebody's
    }
    g_focused_id = g_current[g_pass_first].id;
    copy_name(g_focused_name, g_current[g_pass_first].name);
}

bool take_activate() {
    if (!g_activate_pending) return false;
    g_activate_pending = false; // exactly one widget gets it
    return true;
}

int nav_axis_x() { return g_nav_x; }

void set_pointer_over_ui() { g_pointer_over_ui = true; }

void set_pointer_captured(uint32_t id, bool c) {
    if (c) {
        g_pointer_capture_id = id;
    } else if (g_pointer_capture_id == id) {
        // Only the element that took it can give it back.
        g_pointer_capture_id = 0;
    }
}

void set_keyboard_captured(bool c) { g_keyboard_captured = c; }

void set_focus_visible(bool on) { g_focus_visible = on; }

uint32_t press_id() { return g_press_id; }
void set_press_id(uint32_t id) { g_press_id = id; }

void begin_capture_frame() {
    g_pointer_over_ui = false;
    g_keyboard_captured = false;
}

bool pointer_over_ui() { return g_pointer_over_ui || g_pointer_capture_id != 0; }
bool keyboard_captured() { return g_keyboard_captured; }

bool focus_visible() { return g_focus_visible; }

void focus_by_id(uint32_t id, std::string_view name) {
    g_focused_id = id;
    copy_name(g_focused_name, name);
}

// --- widget scratch --------------------------------------------------------

WidgetState *state_for(uint32_t id) {
    constexpr int kSlots = 64;
    static WidgetState slots[kSlots];
    static int next = 0;
    for (auto &slot : slots) {
        if (slot.id == id) return &slot;
    }
    for (auto &slot : slots) {
        if (slot.id == 0) {
            slot = WidgetState{};
            slot.id = id;
            return &slot;
        }
    }
    // Full. Evicting round-robin loses one dropdown's open flag rather than
    // refusing to draw it, which is the right way round for a UI.
    WidgetState *s = &slots[next];
    next = (next + 1) % kSlots;
    *s = WidgetState{};
    s->id = id;
    return s;
}

} // namespace detail

// --- public ----------------------------------------------------------------

bool wants_pointer() { return detail::pointer_over_ui(); }
bool wants_keyboard() { return detail::keyboard_captured(); }

void focus(std::string_view id) {
    if (id.empty()) {
        detail::focus_by_id(0, "");
        return;
    }
    // Asked for by name, by the game: that is deliberate, so it is shown. The
    // focus a pass gives itself is not, until the player reaches for a key.
    detail::set_focus_visible(true);
    // peek, not element_id: the allocating one would count this label as an
    // occurrence of itself, so the widget it is trying to focus would come out
    // as the NEXT one and the two ids could never match.
    detail::focus_by_id(detail::peek_element_id(id).id, id);
}

std::string_view focused() { return std::string_view{ g_focused_name }; }

void set_navigation_enabled(bool on) { g_navigation_enabled = on; }
bool navigation_enabled() { return g_navigation_enabled; }

} // namespace rmp::ui
