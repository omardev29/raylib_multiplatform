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

bool g_navigation_enabled = true;
bool g_activate_pending = false;
int g_nav_x = 0;
bool g_pointer_over_ui = false;
bool g_pointer_captured = false;
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

// Down/Up on the keyboard, the d-pad, or the left stick pushed far enough to
// be deliberate.
int read_nav_y() {
    int y = 0;
    if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_TAB)) y += 1;
    if (IsKeyDown(KEY_UP)) y -= 1;
    if (IsGamepadAvailable(0)) {
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) y += 1;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_UP)) y -= 1;
        float ly = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        if (ly > 0.5f) y += 1;
        if (ly < -0.5f) y -= 1;
    }
    // Shift+Tab is "backwards", which is the one convention people expect
    // without being told.
    if (y > 0 && IsKeyDown(KEY_TAB) &&
        (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT))) {
        y = -1;
    }
    return y > 0 ? 1 : (y < 0 ? -1 : 0);
}

int read_nav_x() {
    int x = 0;
    if (IsKeyDown(KEY_RIGHT)) x += 1;
    if (IsKeyDown(KEY_LEFT)) x -= 1;
    if (IsGamepadAvailable(0)) {
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) x += 1;
        if (IsGamepadButtonDown(0, GAMEPAD_BUTTON_LEFT_FACE_LEFT)) x -= 1;
        float lx = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_X);
        if (lx > 0.5f) x += 1;
        if (lx < -0.5f) x -= 1;
    }
    return x > 0 ? 1 : (x < 0 ? -1 : 0);
}

bool read_activate() {
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE))
        return true;
    if (IsGamepadAvailable(0) &&
        IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
        return true;
    return false;
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
    g_pointer_over_ui = false;
    g_nav_x = 0;
    g_activate_pending = false;

    if (!g_navigation_enabled) return;

    // A text field owns the keyboard while it has focus; Tab and the arrows
    // there mean "move the caret", not "leave this field".
    if (!g_keyboard_captured) {
        int y = read_nav_y();
        if (y != 0 && y != g_last_nav_y) {
            move_focus(y);
            g_repeat_timer = kRepeatDelay;
        } else if (y != 0) {
            g_repeat_timer -= GetFrameTime();
            if (g_repeat_timer <= 0.0f) {
                move_focus(y);
                g_repeat_timer = kRepeatInterval;
            }
        }
        g_last_nav_y = y;
        g_nav_x = read_nav_x();
    }

    g_activate_pending = read_activate();
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
    g_keyboard_captured = false;
}

bool focusable(Clay_ElementId id, std::string_view name) {
    if (g_current_count < kMaxFocusables) {
        g_current[g_current_count].id = id.id;
        copy_name(g_current[g_current_count].name, name);
        g_current_count++;
    }
    return g_navigation_enabled && g_focused_id == id.id;
}

bool take_activate() {
    if (!g_activate_pending) return false;
    g_activate_pending = false; // exactly one widget gets it
    return true;
}

int nav_axis_x() { return g_nav_x; }

void set_pointer_over_ui() { g_pointer_over_ui = true; }
void set_pointer_captured(bool c) { g_pointer_captured = c; }
void set_keyboard_captured(bool c) { g_keyboard_captured = c; }

bool pointer_over_ui() { return g_pointer_over_ui || g_pointer_captured; }
bool keyboard_captured() { return g_keyboard_captured; }

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
    detail::focus_by_id(detail::element_id(id, nullptr).id, id);
}

std::string_view focused() { return std::string_view{ g_focused_name }; }

void set_navigation_enabled(bool on) { g_navigation_enabled = on; }
bool navigation_enabled() { return g_navigation_enabled; }

} // namespace rmp::ui
