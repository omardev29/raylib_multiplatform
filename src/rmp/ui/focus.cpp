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
constexpr int MAX_FOCUSABLES = 128;

struct Entry {
    uint32_t id = 0;
    char name[48] = { 0 };
};

// The file's state, one struct per concern: the dot at every use says "this
// is file state", and a group cannot collide with focus(), focused() or a
// parameter.
struct {
    Entry current[MAX_FOCUSABLES];
    int current_count = 0;
    Entry previous[MAX_FOCUSABLES];
    int previous_count = 0;
    // Where the pass being described starts in current. See end_pass_focus().
    int pass_first = 0;
} lists;

// The focused element, and whether it is SHOWN. Having one and drawing a ring
// around it are two questions, and they have two answers: everything is
// focusable now, so a menu drawn for a player holding a mouse would wear a ring
// nobody asked for on its first button, every time. It appears the moment the
// keyboard or the gamepad is used -- which is the moment it starts meaning
// something -- and goes away again on a click. The browsers' :focus-visible,
// for the same reason.
struct {
    uint32_t id = 0;
    char name[48] = { 0 };
    bool visible = false;
} target;

// Keyboard and gamepad navigation, and held-key repeat, so holding down on a
// d-pad walks a menu instead of moving one item and stopping.
struct {
    bool enabled = true;
    bool activate_pending = false;
    // The two that end a text field's editing: Enter or A (activate without
    // Space), and Escape or B. Kept whether or not navigation is on, because
    // a field typed into with navigation off still has to be able to stop.
    bool submit_pending = false;
    bool cancel_pending = false;
    int x = 0;
    // One step up or down, handed to an open list instead of moving the focus.
    int y = 0;
    float repeat_timer = 0.0f;
    int last_y = 0;
} navigation;

// Who has the pointer and the keyboard.
struct {
    bool pointer_over_ui = false;
    // WHICH element is dragging, or 0. A plain bool meant every slider in the
    // frame wrote to the same flag, so the one drawn after the one being
    // dragged handed the pointer straight back to the game -- and nothing
    // cleared it at all if the dragging slider stopped being drawn.
    uint32_t pointer_id = 0;
    uint32_t press_id = 0;
    // This FRAME: a text field is typing, which is what wants_keyboard() says.
    bool keyboard = false;
    // ACROSS frames: which text field has the keyboard, or 0. It is not the
    // focus, and it used to be: a field that had the focus kept the keyboard,
    // so it kept Tab and the arrows too and nothing could take the focus back
    // off it. The keyboard follows the focus when the player or the game MOVES
    // it -- navigation, focus(), a click -- and goes back on Enter, Escape, a
    // click elsewhere, or the field not being drawn.
    uint32_t keyboard_id = 0;
    // The element that took up and down for itself last frame -- an open
    // dropdown list -- or 0. Claimed afresh every frame, so a list that closes
    // or stops being drawn gives them back by doing nothing.
    uint32_t vertical_id = 0;
} capture;

constexpr float REPEAT_DELAY = 0.45f;
constexpr float REPEAT_INTERVAL = 0.09f;

void copy_name(char *dst, std::string_view s) {
    size_t n = s.size() < 47 ? s.size() : 47;
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

int index_of(uint32_t id) {
    for (int i = 0; i < lists.previous_count; i++) {
        if (lists.previous[i].id == id) return i;
    }
    return -1;
}

void move_focus(int delta) {
    // A local, clamped count. lists.previous_count cannot exceed the array,
    // but reading it into a bounded local is what makes every index below
    // provably inside lists.previous[] from this function alone, rather than from
    // an invariant kept somewhere else in the file.
    const int count =
        lists.previous_count < MAX_FOCUSABLES ? lists.previous_count : MAX_FOCUSABLES;
    if (count <= 0) return;

    const int at = index_of(target.id);
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
    target.id = lists.previous[next].id;
    copy_name(target.name, lists.previous[next].name);
    // The player moved it here, so a text field it lands on is the one they
    // are about to type into. Anything else holding this id never asks.
    capture.keyboard_id = target.id;
}

} // namespace

namespace detail {

void begin_focus_frame() {
    lists.current_count = 0;
    navigation.x = 0;
    navigation.y = 0;
    const uint32_t vertical = capture.vertical_id;
    capture.vertical_id = 0;
    navigation.activate_pending = false;
    navigation.submit_pending = false;
    navigation.cancel_pending = false;

    // A drag and a press both end when the pointer goes up, whatever is or is
    // not being drawn. That is a property of the pointer and not of a pass,
    // which is why they are released here and not with the other two capture
    // flags -- and not on the release frame itself, which is the frame the
    // click is made of.
    if (!pointer_down() && !pointer_released()) {
        capture.pointer_id = 0;
        capture.press_id = 0;
    }

    // Once per frame, through the provider: see NavState. Everything below is
    // logic on top of those numbers, which is what makes a controller testable
    // without a controller.
    NavState nav{};
    read_nav(&nav);
    navigation.submit_pending = nav.submit;
    navigation.cancel_pending = nav.cancel;

    if (!navigation.enabled) return;

    // One step per press, then a repeat while it is held.
    int step = 0;
    if (nav.y != 0 && nav.y != navigation.last_y) {
        step = nav.y;
        navigation.repeat_timer = REPEAT_DELAY;
    } else if (nav.y != 0) {
        // frame_time(), not GetFrameTime(): 0 in test mode, so a headless
        // run moves exactly one step per press and always the same way.
        navigation.repeat_timer -= frame_time();
        if (navigation.repeat_timer <= 0.0f) {
            step = nav.y;
            navigation.repeat_timer = REPEAT_INTERVAL;
        }
    }
    navigation.last_y = nav.y;
    // Up, down and Tab move the focus whoever has it, a text field included:
    // the field used to keep them, and with them the focus, for good. The one
    // exception is a focused list that is open, where they walk its items.
    if (step != 0 && vertical != 0 && vertical == target.id) {
        navigation.y = step;
    } else if (step != 0) {
        move_focus(step);
    }
    // Left and right belong to the text while a field is typing; for
    // everything else they are a slider's.
    navigation.x = capture.keyboard ? 0 : nav.x;

    navigation.activate_pending = nav.activate;

    // Touching the keyboard or the stick is what makes the focus worth showing;
    // going back to the mouse is what stops it.
    if (nav.x != 0 || nav.y != 0 || nav.activate) target.visible = true;
    if (pointer_just_pressed()) target.visible = false;
}

void end_focus_frame() {
    // This frame's declaration order becomes what the next frame navigates.
    // The count is clamped here rather than trusted. register_focusable() below
    // already refuses to write past the array, so this can only ever be a no-op
    // — but it is the one line that makes the bound a property of the copy
    // instead of something three call sites each have to remember.
    int n = lists.current_count;
    if (n > MAX_FOCUSABLES) n = MAX_FOCUSABLES;
    for (int i = 0; i < n; i++) lists.previous[i] = lists.current[i];
    lists.previous_count = n;

    // A text field that was not drawn this frame -- or was, in a pass input
    // could not reach -- has stopped typing, and must not start again by
    // itself the next time it appears.
    if (capture.keyboard_id != 0 && index_of(capture.keyboard_id) < 0) {
        capture.keyboard_id = 0;
    }

    // If whatever had the focus is no longer on screen, hand it to the first
    // thing that is, rather than leaving a controller with nowhere to go.
    if (target.id != 0 && index_of(target.id) < 0 && lists.previous_count > 0) {
        target.id = lists.previous[0].id;
        copy_name(target.name, lists.previous[0].name);
    }
}

bool focusable(Clay_ElementId id, std::string_view name) {
    // A pass input cannot reach is not focusable either, which is what
    // rmp/ui.h has promised set_pass_input() means all along. Without this the
    // arrows walked into the HUD under an open pause menu, and Enter activated
    // something in a scene the player could not even see the cursor in.
    if (!pass_input()) return false;

    if (lists.current_count < MAX_FOCUSABLES) {
        lists.current[lists.current_count].id = id.id;
        copy_name(lists.current[lists.current_count].name, name);
        lists.current_count++;
    }
    return navigation.enabled && target.id == id.id;
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

void begin_pass_focus() { lists.pass_first = lists.current_count; }

void end_pass_focus() {
    if (!navigation.enabled) return;
    if (lists.current_count <= lists.pass_first) return; // nothing focusable in it
    for (int i = 0; i < lists.current_count; i++) {
        if (lists.current[i].id == target.id) return; // already somebody's
    }
    target.id = lists.current[lists.pass_first].id;
    copy_name(target.name, lists.current[lists.pass_first].name);
}

bool take_activate() {
    if (!navigation.activate_pending) return false;
    navigation.activate_pending = false; // exactly one widget gets it
    return true;
}

bool take_submit() {
    if (!navigation.submit_pending) return false;
    navigation.submit_pending = false;
    navigation.activate_pending = false; // it was this press, so nobody else's
    return true;
}

bool take_cancel() {
    if (!navigation.cancel_pending) return false;
    navigation.cancel_pending = false;
    return true;
}

int nav_axis_x() { return navigation.x; }
int nav_axis_y() { return navigation.y; }

void claim_vertical(uint32_t id) { capture.vertical_id = id; }

void set_pointer_over_ui() { capture.pointer_over_ui = true; }

void set_pointer_captured(uint32_t id, bool c) {
    if (c) {
        capture.pointer_id = id;
    } else if (capture.pointer_id == id) {
        // Only the element that took it can give it back.
        capture.pointer_id = 0;
    }
}

void set_keyboard_captured(bool c) { capture.keyboard = c; }

bool has_keyboard(uint32_t id) { return pass_input() && capture.keyboard_id == id; }
void take_keyboard(uint32_t id) { capture.keyboard_id = id; }
void release_keyboard(uint32_t id) {
    if (capture.keyboard_id == id) capture.keyboard_id = 0;
}

void set_focus_visible(bool on) { target.visible = on; }

uint32_t press_id() { return capture.press_id; }
void set_press_id(uint32_t id) { capture.press_id = id; }

void begin_capture_frame() {
    capture.pointer_over_ui = false;
    capture.keyboard = false;
}

bool pointer_over_ui() { return capture.pointer_over_ui || capture.pointer_id != 0; }
bool keyboard_captured() { return capture.keyboard; }

bool focus_visible() { return target.visible; }

void focus_by_id(uint32_t id, std::string_view name) {
    target.id = id;
    copy_name(target.name, name);
    // Moved there on purpose, by the game or a click: a text field takes the
    // keyboard with it, exactly as when the player navigates onto it.
    capture.keyboard_id = id;
}

// --- widget scratch --------------------------------------------------------

WidgetState *state_for(uint32_t id) {
    constexpr int SLOTS = 64;
    static WidgetState slots[SLOTS];
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
    next = (next + 1) % SLOTS;
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

std::string_view focused() { return std::string_view{ target.name }; }

void set_navigation_enabled(bool on) { navigation.enabled = on; }
bool navigation_enabled() { return navigation.enabled; }

} // namespace rmp::ui
