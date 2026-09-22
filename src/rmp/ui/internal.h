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

// Has it started already? Unlike ensure_started(), asking does not make it
// happen — which is the whole point at the frame boundary: an app whose scenes
// draw no UI must not be made to allocate Clay's arena and load a font just
// because something calls begin_frame() sixty times a second.
bool started();

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

// What the UI's own keyboard and gamepad handling needs, and all of it: which
// way the player is pushing, and whether they pressed the button that means
// "do it". Sampled once per frame through a provider, exactly like the pointer
// and for exactly the same reason -- a headless run has no devices, so a test
// that wants to press Enter needs somewhere to say so. It is also what keeps
// rmp::ui from having to ask rmp::input, which already asks us.
struct NavState {
    int x = 0; // -1, 0 or +1 while held: Left/Right, the d-pad, the stick
    int y = 0; // the same up and down, and Tab counts as down
    bool activate = false; // pressed THIS frame: Enter, Space, the A button
};
void read_nav(NavState *out);

// Pointer state for this frame, sampled once at the FRAME boundary so that two
// scenes drawing in the same frame cannot disagree about where the mouse is.
// `present` is false on a touch screen with nothing touching it — see
// touch_only() — and also for a pass that input cannot reach, which is how a
// HUD under an open pause menu goes quiet without either scene saying so.
void update_pointer();
Clay_Vector2 pointer_position();
bool pointer_down();
bool pointer_present();
bool pointer_just_pressed();
bool pointer_released();

// The box an element ended up with LAST FRAME, by id. Immediate mode has no
// other answer: the geometry of the frame being described does not exist until
// it is finished.
//
// It reads a snapshot of ours rather than asking Clay, and that is not
// duplication. Clay_BeginLayout resets Clay's element map, so with two passes
// only the last one survives and the lower scene's grid would size itself from
// the pause menu's geometry. The snapshot is per pass and lasts the whole
// frame.
bool bounds_of_id(Clay_ElementId id, Clay_BoundingBox *out);

// Is the pointer over this element? OURS, from the per-pass snapshot above and
// not from Clay_PointerOver(): Clay hit-tests against whatever tree it holds at
// the time, so with two passes in a frame it answered each of them from the
// other one's geometry and nothing in either scene could be hovered or clicked.
//
// False for a pass input cannot reach, so no widget has to remember that
// either. `slop_y` grows the box up and down, for a slider's rail, which is
// thinner than a finger.
bool pointer_over(Clay_ElementId id);
bool pointer_over(Clay_ElementId id, float slop_y);

// A clipping container is open: everything declared inside it can only be
// hovered where the container itself is. Paired, and reset per pass.
void push_clip(Clay_ElementId id);
void pop_clip();

// This element is in FRONT of the rest of its pass and takes the pointer — an
// open dropdown list. Recorded because a box test has no z-order of its own.
void block_pointer(Clay_ElementId id);

// A stable id derived from another one, for the parts a widget is made of —
// a slider's track, say. Hashing a fixed suffix instead would give every
// slider in the frame the same track. sub_id() also records it in the pass, so
// its box is remembered; peek_sub_id() is for ASKING about an element before it
// is declared, where recording it early would put it in the wrong container.
Clay_ElementId sub_id(Clay_ElementId base, uint32_t which);
Clay_ElementId peek_sub_id(Clay_ElementId base, uint32_t which);

// True on the platforms whose only pointer is a finger. There, hover has to be
// suppressed when nothing is touching the screen, or the last place tapped
// stays lit up forever.
bool touch_only();

// The Size to lay out for: the window, or the test viewport when one is set.
Clay_Dimensions viewport();
bool test_mode();

// Seconds since the last frame, or 0 in test mode — a headless run has no frame
// time and has to produce the same numbers every time. The one place the UI
// reads raylib's clock; everything else takes it from here.
float frame_time();

// Pixels to keep clear at the edge of the screen. [android.display]
// into_cutout draws the game behind the notch, which is right for a background
// and wrong for a menu.
float safe_area_inset();

// The font the UI draws with, and the size it was baked at. `::Font` and not
// `Font`: inside rmp:: the unqualified name is now rmp::Font, the counted
// handle from rmp/assets.h. This one is raylib's plain struct, owned by the UI. For the built-in
// bitmap font the scale is rounded to a whole number, because a pixel font at
// 1.73x is a smeared mess.
::Font ui_font();
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

// The same id, WITHOUT allocating any of it: no occurrence bump, no interning,
// nothing remembered. It answers "what id does the `occurrence`-th widget
// labelled this, in this pass, have?" for code that wants to find an element it
// did not declare.
//
// focus() is why this exists. It used to call element_id(), which is the
// ALLOCATING one: focus("Play") registered "Play" as seen, so the real
// button("Play") in the same pass came out as occurrence 1 and the two ids
// could never match — the focus went to an element nothing owned, and every
// later widget sharing the label was renumbered on the way.
Clay_ElementId peek_element_id(std::string_view label, unsigned occurrence, int pass);

// The first widget with that label in the pass being described. What focus(name)
// means.
Clay_ElementId peek_element_id(std::string_view label);

// --- the frame, and the passes inside it -----------------------------------
//
// A FRAME is one turn of the game loop. A PASS is one begin()/end() pair, and
// there is one per scene that draws UI — so with a pause menu over a game there
// are two passes in one frame. rmp::ui::detail::begin_frame()/end_frame() mark
// the frame; the functions here mark the passes inside it.

// Has the frame boundary been marked? False means nobody called begin_frame(),
// which is the plain-raylib case: begin() marks it itself.
bool frame_marked();
bool frame_self_marked(); // ...and it was begin() that did the marking
void set_frame_self_marked(bool self);

// The half of begin_frame() that needs Clay to be up. Called again by the first
// begin() of the frame, because that is where the UI actually starts; it does
// nothing the second time.
void prepare_frame();

// Start a pass: bump the pass index, and clear the per-pass label counters and
// the z-order. Called from begin().
void begin_pass();

// Which pass this is, counting from 0 within the frame. It is a multiplier on
// the element index, which is what gives each scene its own id space: without
// it, a lower scene showing a button conditionally shifts the indices of every
// scene above it and the hover jumps to a different widget with nothing in that
// scene having changed. It is one multiplication and it removes a whole class
// of bugs that reproduce about one run in twenty.
int current_pass();

// Whether this pass can be interacted with. Behind rmp::ui::detail::
// set_pass_input(), which rmp::app drives from Scene::input_below.
bool pass_input();

// Record the boxes this pass ended up with, so that the next frame's pass can
// read them. See bounds_of_id().
void capture_pass_bounds();

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

// Should the focus ring be DRAWN? Having the focus and showing it are two
// questions since every pass gives itself a focus: a player holding a mouse
// would otherwise find a ring on the first button of every menu that opens.
// True from the moment the keyboard or the gamepad is used, or the game calls
// rmp::ui::focus() itself; false again on a click.
bool focus_visible();
void set_focus_visible(bool on);

void begin_focus_frame(); // resolve navigation, using last frame's list
void end_focus_frame(); // swap the lists

// The pass boundary, for focus. A pass that declares focusable widgets and has
// no focus of its own takes its first one, so that Enter and the A button mean
// something the moment a scene appears — without them a pushed game-over menu
// could not be pressed until the player had tapped Down or Tab first.
void begin_pass_focus();
void end_pass_focus();

// True once per press, for whoever has the focus. Enter, Space, or the
// gamepad's bottom face button.
bool take_activate();

// -1, 0 or +1 from the arrows, the d-pad or the left stick, for the controls
// where sideways means something (a slider). Repeats while held.
int nav_axis_x();

// Someone is dragging, or the pointer is over something interactive. This is
// what wants_pointer() answers with.
void set_pointer_over_ui();

// Which element a press started on, so a click is "released over the element it
// was pressed on". It lives here rather than in widgets.cpp because it is
// pointer state and has to outlive a PASS: end() used to clear it whenever the
// pointer was not down, and a pass input cannot reach reports nothing as down —
// so a HUD drawn under an open menu wiped the press before the menu's own pass
// ran, and its buttons never fired. Cleared at the frame boundary instead, once
// the pointer has been up for a whole frame.
uint32_t press_id();
void set_press_id(uint32_t id);

// A drag belongs to ONE element. It used to be a plain bool, so a slider drawn
// after the one being dragged cleared the capture for everybody and the click
// that was dragging Music also fired the player's weapon. Passing the id means
// only the element that took the pointer can give it back.
void set_pointer_captured(uint32_t id, bool captured);

// A text field has the keyboard.
void set_keyboard_captured(bool captured);

// Both capture flags start empty here, and here only: at the FIRST begin() of a
// frame. Not at the frame boundary — rmp::app marks that before the scenes
// update, so a game asking wants_keyboard() in _update would read a flag that
// had been cleared before anything could set it again, which is the exact
// question the function exists to answer. They therefore hold last frame's
// answer until this frame's UI has had its say.
void begin_capture_frame();

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
using NavFn = void (*)(NavState *out);

void set_measure_provider(MeasureFn fn);
void set_pointer_provider(PointerFn fn);
void set_nav_provider(NavFn fn);

// Test mode: use the viewport given here instead of asking raylib, and skip
// drawing in end(). With this on there is no window and no GL context, and
// Clay_EndLayout is pure computation — which is the whole point.
// Width or height of 0 means "ask raylib", i.e. normal operation.
void set_test_viewport(float width, float height);

// The box an element ended up with in the last completed frame. `occurrence` is
// 0 for the first element with that label, 1 for the second, and so on, and
// `pass` is which begin()/end() pair described it — the same numbering
// element_id() assigns. This asks Clay, which only remembers the LAST pass of
// the frame; bounds_of_id() reads our own per-pass snapshot and remembers all
// of them.
bool bounds_of(std::string_view label, unsigned occurrence, int pass,
               Clay_BoundingBox *out);

// How many errors Clay has reported since the last reset, and the first one's
// type — first, because what follows a capacity failure is its consequences. Clay reports through a handler rather than a return value, so this is
// the only way a test can see that a frame produced a duplicate id or ran out
// of elements.
int clay_error_count();
Clay_ErrorType first_clay_error();
void reset_clay_errors_for_tests();

// The pointer the last image() handed Clay. Clay keeps it until end() draws, so
// the rule worth proving is that it points into the frame arena and not at the
// caller's Texture2D — which may have been a temporary that died at the
// semicolon.
const void *last_image_data();

// The defaults, exposed so a test can put them back.
Clay_Dimensions measure_with_raylib(Clay_StringSlice text, Clay_TextElementConfig *config,
                                    void *user);
void pointer_from_raylib(Clay_Vector2 *position, bool *down);
void nav_from_raylib(NavState *out);

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
