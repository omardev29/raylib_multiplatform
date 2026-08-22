#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/ui/. Not in include/, on purpose: this
// is where Clay is allowed to exist, and a header the user can reach is a
// header the user will end up depending on.
//
// The public surface is include/rmp/ui.h.
// ---------------------------------------------------------------------------

#include <raylib.h>
#include <rmp/ui.h>

#include "clay.h"

#include <cstdint>
#include <string_view>

namespace rmp::ui::detail {

// --- context.cpp -----------------------------------------------------------

// Start Clay if this is the first frame. Called by begin(), never by the user:
// it has to happen after InitWindow(), and the first begin() is the earliest
// moment we can be sure of that.
bool ensure_started();

// True between begin() and end(). Used to catch mismatched pairs in debug.
bool frame_open();
void set_frame_open(bool open);

// Free Clay's arena and any loaded font. Behind rmp::ui::shutdown().
void shutdown_context();

// Recomputed at every begin() from the viewport and the design resolution.
void update_scale();
float ui_scale();
void set_scale_override(float s); // 0 = automatic

// The pointer, through whichever provider is installed.
void read_pointer(Clay_Vector2 *position, bool *down);

// Pointer state for this frame, sampled once in begin() so every widget sees
// the same thing. `present` is false on a touch screen with nothing touching
// it — see touch_only().
void update_pointer();
Clay_Vector2 pointer_position();
bool pointer_down();
bool pointer_present();
bool pointer_just_pressed();
bool pointer_released();

// The box an element ended up with last frame, by id.
bool bounds_of_id(Clay_ElementId id, Clay_BoundingBox *out);

// A stable id derived from another one, for the parts a widget is made of —
// a slider's track, say. Hashing a fixed suffix instead would give every
// slider in the frame the same track.
Clay_ElementId sub_id(Clay_ElementId base, uint32_t which);

// True on the platforms whose only pointer is a finger. There, hover has to be
// suppressed when nothing is touching the screen, or the last place tapped
// stays lit up forever.
bool touch_only();

// The Size to lay out for: the window, or the test viewport when one is set.
Clay_Dimensions viewport();
bool test_mode();

// Pixels to keep clear at the edge of the screen. [android.display]
// into_cutout draws the game behind the notch, which is right for a background
// and wrong for a menu.
float safe_area_inset();

// The font the UI draws with, and the size it was baked at. For the built-in
// bitmap font the scale is rounded to a whole number, because a pixel font at
// 1.73x is a smeared mess.
Font ui_font();
float font_scale();

// Text lives in a bump arena that is reset every begin(). Clay does NOT copy
// strings — it keeps the pointer and reads it during Clay_EndLayout — so a
// caller passing std::to_string(score) would otherwise hand it a dangling
// pointer. Copying on the way in removes the whole class of bug.
Clay_String intern(std::string_view s);
void reset_frame_arena();

// Room in the same frame arena for something that is not a string — an image
// tint, say. Same lifetime rule: valid until the next begin(). Returns nullptr
// when the arena is full.
void *frame_alloc(size_t bytes);

// Layers inside a stack() are floating elements, and Clay draws floating
// elements by z-index rather than declaration order. This hands out an
// increasing z per frame so that the last layer() written is the one on top,
// which is the order anyone reading the code expects.
int16_t next_layer_z();

// Element identity. Hashing the label alone would make two "Back" buttons in
// two different screens the same element, so they would highlight together.
// The occurrence counter disambiguates the common case; an explicit id is the
// escape hatch when the UI is conditional.
Clay_ElementId element_id(std::string_view label, const char *explicit_id);
void reset_id_counters();

// --- style.cpp -------------------------------------------------------------
//
// Sizes and colours, in one place so that no widget has to invent either.

// Advance the transition clock. Called once from begin(); 0 in test mode, so a
// headless run gives the same numbers every time.
void anim_begin_frame();

// 0..1 for one boolean channel of one element, eased toward `on`. `channel` is
// what lets one element animate more than one thing.
float anim_value(Clay_ElementId id, uint32_t channel, bool on);

// The colour a control is right now, moving between the three the theme gives
// it. Every widget with a hover state goes through this, which is why they all
// feel the same.
Color state_color(Clay_ElementId id, Color base, Color hover, Color press, bool over,
                  bool pressed);

// Blend two colours, alpha included. `t` is 0..1 and is clamped.
Color mix_color(Color a, Color b, float t);

// A Size step or a raw number, resolved to design units.
float resolve_size(const Sizing &s, const Theme &t);

// The same thing as a multiplier on the theme's base type Size, for the
// metrics that have to move with it: padding, and the minimum touch height.
float size_ratio(const Sizing &s, const Theme &t);

// --- focus.cpp -------------------------------------------------------------
//
// Focus is what makes the same code playable with a controller. Widgets do not
// implement it individually: they register, and the navigation happens here.

// Register an interactive element in declaration order. Returns true if it is
// the one with the focus right now.
bool focusable(Clay_ElementId id, std::string_view name);

void begin_focus_frame(); // resolve navigation, using last frame's list
void end_focus_frame(); // swap the lists

// True once per press, for whoever has the focus. Enter, Space, or the
// gamepad's bottom face button.
bool take_activate();

// -1, 0 or +1 from the arrows, the d-pad or the left stick, for the controls
// where sideways means something (a slider). Repeats while held.
int nav_axis_x();

// Someone is dragging, or the pointer is over something interactive. This is
// what wants_pointer() answers with.
void set_pointer_over_ui();
void set_pointer_captured(bool captured);

// A text field has the keyboard.
void set_keyboard_captured(bool captured);

// Small persistent scratch per widget, keyed by element id — a dropdown's open
// flag, a text field's caret. It is UI state, not application state, which is
// why it lives here rather than being something the caller has to hold.
struct WidgetState {
    uint32_t id = 0;
    int i = 0;
    float f = 0;
    bool flag = false;
};
WidgetState *state_for(uint32_t id);

bool pointer_over_ui();
bool keyboard_captured();
void focus_by_id(uint32_t id, std::string_view name);

// --- render.cpp ------------------------------------------------------------

void draw(Clay_RenderCommandArray commands);

// Clay hands out string slices that are NOT null terminated. raylib's
// DrawTextEx and MeasureTextEx both need one, so every slice has to be copied
// into a scratch buffer with a terminator first.
const char *cstr(Clay_StringSlice slice);

// --- seams for tests -------------------------------------------------------
//
// With these two replaced, the layout runs with no window and no GPU:
// Clay_EndLayout is pure computation. That is what makes headless layout tests
// possible (tests/ui_layout_test.cpp).

using MeasureFn = Clay_Dimensions (*)(Clay_StringSlice, Clay_TextElementConfig *, void *);
using PointerFn = void (*)(Clay_Vector2 *position, bool *down);

void set_measure_provider(MeasureFn fn);
void set_pointer_provider(PointerFn fn);

// Test mode: use the viewport given here instead of asking raylib, and skip
// drawing in end(). With this on there is no window and no GL context, and
// Clay_EndLayout is pure computation — which is the whole point.
// Width or height of 0 means "ask raylib", i.e. normal operation.
void set_test_viewport(float width, float height);

// The box an element ended up with in the last completed frame. `occurrence` is
// 0 for the first element with that label, 1 for the second, and so on — the
// same numbering element_id() assigns.
bool bounds_of(std::string_view label, unsigned occurrence, Clay_BoundingBox *out);

// The defaults, exposed so a test can put them back.
Clay_Dimensions measure_with_raylib(Clay_StringSlice text, Clay_TextElementConfig *config,
                                    void *user);
void pointer_from_raylib(Clay_Vector2 *position, bool *down);

// --- shared conversions ----------------------------------------------------

// The same colour with nothing in it. Fading a transparent element in from the
// colour it is about to become, rather than from black, is the difference
// between a ghost control lighting up and one flashing dark first.
inline Color clear_alpha(Color c) { return Color{ c.r, c.g, c.b, 0 }; }

inline Clay_Color to_clay(Color c) {
    return Clay_Color{ static_cast<float>(c.r), static_cast<float>(c.g),
                       static_cast<float>(c.b), static_cast<float>(c.a) };
}

inline Color from_clay(Clay_Color c) {
    return Color{ static_cast<unsigned char>(c.r), static_cast<unsigned char>(c.g),
                  static_cast<unsigned char>(c.b), static_cast<unsigned char>(c.a) };
}

// Design units -> pixels.
inline float px(float design_units) { return design_units * ui_scale(); }

} // namespace rmp::ui::detail
