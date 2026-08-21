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

struct entry {
    uint32_t id       = 0;
    char     name[48] = { 0 };
};

entry g_current[kMaxFocusables];
int   g_currentCount = 0;
entry g_previous[kMaxFocusables];
int   g_previousCount           = 0;

uint32_t g_focusedId            = 0;
char     g_focusedName[48]      = { 0 };

bool g_navigationEnabled        = true;
bool g_activatePending          = false;
int  g_navX                     = 0;
bool g_pointerOverUI            = false;
bool g_pointerCaptured          = false;
bool g_keyboardCaptured         = false;

// Held-key repeat, so holding down on a d-pad walks a menu instead of moving
// one item and stopping.
float           g_repeatTimer   = 0.0f;
int             g_lastNavY      = 0;
constexpr float kRepeatDelay    = 0.45f;
constexpr float kRepeatInterval = 0.09f;

void copy_name(char *dst, std::string_view s) {
    size_t n = s.size() < 47 ? s.size() : 47;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

int index_of(uint32_t id) {
    for (int i = 0; i < g_previousCount; i++) {
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
    const int count = g_previousCount < kMaxFocusables ? g_previousCount : kMaxFocusables;
    if (count <= 0) return;

    const int at = index_of(g_focusedId);
    int       next;
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
    g_focusedId = g_previous[next].id;
    copy_name(g_focusedName, g_previous[next].name);
}

} // namespace

namespace detail {

void begin_focus_frame() {
    g_currentCount    = 0;
    g_pointerOverUI   = false;
    g_navX            = 0;
    g_activatePending = false;

    if (!g_navigationEnabled) return;

    // A text field owns the keyboard while it has focus; Tab and the arrows
    // there mean "move the caret", not "leave this field".
    if (!g_keyboardCaptured) {
        int y = read_nav_y();
        if (y != 0 && y != g_lastNavY) {
            move_focus(y);
            g_repeatTimer = kRepeatDelay;
        } else if (y != 0) {
            g_repeatTimer -= GetFrameTime();
            if (g_repeatTimer <= 0.0f) {
                move_focus(y);
                g_repeatTimer = kRepeatInterval;
            }
        }
        g_lastNavY = y;
        g_navX     = read_nav_x();
    }

    g_activatePending = read_activate();
}

void end_focus_frame() {
    // This frame's declaration order becomes what the next frame navigates.
    // The count is clamped here rather than trusted. register_focusable() below
    // already refuses to write past the array, so this can only ever be a no-op
    // — but it is the one line that makes the bound a property of the copy
    // instead of something three call sites each have to remember.
    int n = g_currentCount;
    if (n > kMaxFocusables) n = kMaxFocusables;
    for (int i = 0; i < n; i++) g_previous[i] = g_current[i];
    g_previousCount = n;

    // If whatever had the focus is no longer on screen, hand it to the first
    // thing that is, rather than leaving a controller with nowhere to go.
    if (g_focusedId != 0 && index_of(g_focusedId) < 0 && g_previousCount > 0) {
        g_focusedId = g_previous[0].id;
        copy_name(g_focusedName, g_previous[0].name);
    }
    g_keyboardCaptured = false;
}

bool focusable(Clay_ElementId id, std::string_view name) {
    if (g_currentCount < kMaxFocusables) {
        g_current[g_currentCount].id = id.id;
        copy_name(g_current[g_currentCount].name, name);
        g_currentCount++;
    }
    return g_navigationEnabled && g_focusedId == id.id;
}

bool take_activate() {
    if (!g_activatePending) return false;
    g_activatePending = false; // exactly one widget gets it
    return true;
}

int nav_axis_x() { return g_navX; }

void set_pointer_over_ui() { g_pointerOverUI = true; }
void set_pointer_captured(bool c) { g_pointerCaptured = c; }
void set_keyboard_captured(bool c) { g_keyboardCaptured = c; }

bool pointer_over_ui() { return g_pointerOverUI || g_pointerCaptured; }
bool keyboard_captured() { return g_keyboardCaptured; }

void focus_by_id(uint32_t id, std::string_view name) {
    g_focusedId = id;
    copy_name(g_focusedName, name);
}

// --- widget scratch --------------------------------------------------------

widget_state *state_for(uint32_t id) {
    constexpr int       kSlots = 64;
    static widget_state slots[kSlots];
    static int          next = 0;
    for (auto &slot : slots) {
        if (slot.id == id) return &slot;
    }
    for (auto &slot : slots) {
        if (slot.id == 0) {
            slot    = widget_state{};
            slot.id = id;
            return &slot;
        }
    }
    // Full. Evicting round-robin loses one dropdown's open flag rather than
    // refusing to draw it, which is the right way round for a UI.
    widget_state *s = &slots[next];
    next            = (next + 1) % kSlots;
    *s              = widget_state{};
    s->id           = id;
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

std::string_view focused() { return std::string_view{ g_focusedName }; }

void set_navigation_enabled(bool on) { g_navigationEnabled = on; }
bool navigation_enabled() { return g_navigationEnabled; }

} // namespace rmp::ui
