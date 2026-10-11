#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/ui/. Not in include/, on purpose: this
// is where Clay is allowed to exist, and a header the user can reach is a
// header the user will end up depending on.
//
// The public surface is include/rmp/ui.h.
// ---------------------------------------------------------------------------

#include <raylib.h>
#include <rmp/assets.h>
#include <rmp/ui.h>

#include "clay.h"
#include "../ui_internal.h" // the hooks the app, the scenes and rmp::input call

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// The question every entry point asks first: is a frame open for `what` (the
// function, "button") labelled `label` to go into? True when it is. When it
// is not, the call is a mistake in the game: it is said once, keyed by what
// and label, and the caller returns doing nothing. While the scene stack is
// updating, or drawing the world under the camera, it is said once for the
// whole phase instead -- the begin() that came first already named it.
bool frame_ready(std::string_view what, std::string_view label);

// The phase half of that, on its own, for begin(): true while the scene stack
// is updating or drawing the world under the camera, where nothing of the
// interface can be described -- said once for the whole phase.
bool phase_refuses(std::string_view what);

// A begin() while a frame is open, inside one of its containers: a mistake
// the game made (begin() went where column() goes), said once by begin(). It
// opens nothing, and the end() that pairs with it closes nothing, so the
// frame around it still closes where it was meant to. Counted per frame.
void note_nested_begin();
bool close_nested_begin(); // true when this end() was that begin()'s

// end(), and how a frame the game left open is closed for it: laid out, its
// interaction resolved, and drawn when `draw` -- not past EndDrawing(), where
// the runner closes RMP_ENTRY_POINT's.
void close_frame(bool draw);

// begin(o), with the root painted `fill` -- a colour with no alpha paints
// nothing, which is begin(). The root covers the whole viewport, so a fill
// takes the pointer everywhere: an overlay's veil.
void open_frame(const FrameOptions &o, Color fill);

// The padding of a frame's root: `pixels` of its own, on top of what each
// side of the viewport keeps clear (content_insets()) -- the safe area, and
// the letterbox bars. And of what stands in for a root, the layer an
// overlay() inside an open frame becomes.
Clay_Padding root_padding(float pixels);

// The pass being described is in front of every pass described before it in
// the frame: from the next frame on, those take no input -- they cannot be
// hovered, clicked or focused -- for as long as a pass keeps saying so. What
// overlay() makes of a frame of its own.
void set_pass_modal();

// Free Clay's arena and any loaded font. Behind rmp::ui::shutdown().
void shutdown_context();

// Recomputed at every begin() from the viewport and the design resolution.
void update_scale();
float ui_scale();
void set_scale_override(float s); // 0 = automatic

// The pointer, through whichever provider is installed.
void read_pointer(Clay_Vector2 &position, bool &down);

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
    // Pressed THIS frame: Enter or the A button -- activate without Space. A
    // text field types a space, so this, and not activate, is what ends its
    // editing.
    bool submit = false;
    // Pressed THIS frame: Escape or the B button. Gives a text field's
    // keyboard back and closes an open dropdown without picking anything.
    bool cancel = false;
    // Pressed THIS frame: Tab, with or without Shift. It moves the focus like
    // `y` -- it is in `y` too -- and it is the one key that walks into a HUD
    // over a game being played (FrameOptions::focus_first). Not the gamepad's
    // Select: on a console the way to the interface is the pause menu.
    bool tab = false;
};
void read_nav(NavState &out);

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

// What the interface takes the pointer over, for when wants_pointer() is
// asked before this frame's UI has run (context.cpp, `claims`): a control the
// pass declared, recorded at its end() by focus.cpp; whether the pointer is
// over anything the last frame drawn claimed -- read now, if the frame has
// not sampled it yet; whether that frame's claims all fit; and whether the UI
// has run this frame at all, which is when its own answer takes over.
void claim_control(uint32_t id);
bool pointer_claimed_now();
bool claims_complete();
bool ui_ran_this_frame();

// Is the pointer over something this pass PAINTED as the interface? Every
// render command but text counts: a background (a panel, a progress bar, an
// open list), a border, an image, and a scroll area -- what is clipped away
// inside a scroll area does not. Plain text with nothing behind it is the one
// exception, on purpose: a score drawn over the game must not stop the game
// being clicked through it. Called by end() with the pass's commands; a hit is
// what wants_pointer() answers with, alongside the controls' own hover.
void claim_pointer_over_painted(Clay_RenderCommandArray commands);

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
// time and has to produce the same numbers every time. It is the game's clock:
// what step_delta() handed the scenes this frame (rmp::app::detail::
// frame_delta(), src/rmp/clock.cpp), so a fixed step fixes the menus too.
// Everything in the UI that animates takes its time from here.
float frame_time();

// What frame_time() answers in test mode instead of 0, so a headless test can
// let a tooltip's delay or a notice's seconds go by. 0 puts it back; a test
// that sets it sets it back.
void set_test_frame_time(float seconds);

// The control declared last in the pass -- a button, a checkbox, a slider, a
// dropdown, a text field -- for what is about it and comes after it
// (tooltip()). Every control notes itself; taking it forgets it, and a new
// pass starts with none. 0 for none.
void note_control(Clay_ElementId id);
uint32_t take_last_control();

// Pixels to keep clear at the edge of the screen. [android.display]
// into_cutout draws the game behind the notch, which is right for a background
// and wrong for a menu.
float safe_area_inset();

// Pixels to keep clear on each side of the viewport: the safe area above, and
// under rmp::app::Stretch::LETTERBOX the bars around the world, so that a
// frame lays out over the game and not over the bars -- a TOP_LEFT HUD at the
// world's corner, a menu centred on it. A side takes whichever is wider: the
// notch is at the edge of the screen, inside a bar as wide as it.
struct Insets {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;
};
Insets content_insets();

// The part of the viewport the game is drawn in: the whole of it, or the
// world's area inside the letterbox bars. What current_breakpoint() reads.
Clay_Dimensions content_area();

// The font to draw and measure text of this many pixels with. A [ui] font is
// baked at exactly that size, once, and kept until the UI shuts down, so
// every size is sharp; raylib's built-in font is the same font at every size,
// as it always was. `::Font` and not `Font`: inside rmp:: the unqualified name
// is rmp::Font, the counted handle from rmp/assets.h. This one is raylib's
// plain struct, valid until the face is let go of -- use it, do not keep it.
::Font ui_font(float pixel_size);

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
// escape hatch when the UI is conditional. An empty explicit_id is none.
Clay_ElementId element_id(std::string_view label, std::string_view explicit_id);

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

// The box an element was drawn with THIS frame, and the scroll area it is
// inside (0 for none) -- for the frame boundary, once every pass has drawn.
// bounds_of_id() is LAST frame's, for a frame still being described.
struct DrawnBox {
    Clay_BoundingBox box;
    uint32_t clip;
};
std::optional<DrawnBox> drawn_box(uint32_t id);

// --- style.cpp -------------------------------------------------------------
//
// Sizes and colours, in one place so that no widget has to invent either.

// Advance the transition clock. Called once from begin(); 0 in test mode, so a
// headless run gives the same numbers every time.
void anim_begin_frame();

// 0..1 for one boolean channel of one element, eased toward `on`. `channel` is
// what lets one element animate more than one thing.
float anim_value(Clay_ElementId id, uint32_t channel, bool on);

// Every transition forgotten: the UI is shutting down, and a control seen
// after it starts again starts where it is going, as on the first frame.
void forget_transitions();

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

// --- containers.cpp --------------------------------------------------------

// A pass starts with no container open: the grids nested so far and the rows
// spacer(n) asks about belong to the last one, and go. Called by begin(), and
// by shutdown().
void begin_pass_containers();

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

// The element with the focus, or 0: what rmp::ui::focused() names. The frame
// boundary keeps it in view inside the scroll areas it is in (context.cpp).
uint32_t focused_id();

// The controls the frame that has just ended put where nobody can reach them:
// a control that takes the focus, not wholly inside the window, and in no
// scroll area that could bring it in. Their labels. For tests/e2e/driver.cpp,
// which fails a script whose example does it -- ui/04_settings had its Apply
// button below the bottom of its own window, at the size it opens in.
std::vector<std::string> controls_out_of_reach();

void begin_focus_frame(); // resolve navigation, using last frame's list
void end_focus_frame(); // swap the lists

// The pass boundary, for focus. A pass that declares focusable widgets and has
// no focus of its own takes its first one, so that Enter and the A button mean
// something the moment a scene appears — without them a pushed game-over menu
// could not be pressed until the player had tapped Down or Tab first.
void begin_pass_focus();
void end_pass_focus();

// The pass's FrameOptions::focus_first, handed over by begin() before the pass
// starts. Unset is the framework's guess: see set_pass_playing() and
// pass_playing(), in ../ui_internal.h and here.
void set_pass_focus_first(std::optional<bool> first);

// Whether the pass being described is over a game being played: what the
// scene stack said with set_pass_playing().
bool pass_playing();

// Everything the focus remembers, gone: the UI is shutting down.
void forget_focus();

// True once per press, for whoever has the focus. Enter, Space, or the
// gamepad's bottom face button.
bool take_activate();

// The same, for the two presses that end a text field's editing: Enter or the
// A button (Space is typed, so it is not one of them), and Escape or the B
// button. take_submit() consumes the activate it came with. Both are pending
// with navigation off too: a field typed into then still has to stop.
bool take_submit();
bool take_cancel();

// -1, 0 or +1 from the arrows, the d-pad or the left stick, for the controls
// where sideways means something (a slider). Repeats while held.
int nav_axis_x();

// The same up and down, for the one control that takes them away from
// navigation: an open dropdown list, which walks its items with them. It
// claims them every frame it is open and focused; the claim is read at the
// next frame boundary, and until then up and down move the focus as usual.
// nav_axis_y() is 0 for everyone else.
void claim_vertical(uint32_t id);
int nav_axis_y();

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

// A text field is typing THIS frame: what wants_keyboard() answers with.
void set_keyboard_captured(bool captured);

// Which text field has the keyboard, across frames. Not the focus: the focus
// can sit on a field that is not typing (after Enter), and a field can type
// with navigation off and nothing focused (after a click). Navigation, focus()
// and a click hand it to what they land on; the field gives it back itself.
// has_keyboard() is false in a pass input cannot reach.
bool has_keyboard(uint32_t id);
void take_keyboard(uint32_t id);
void release_keyboard(uint32_t id); // only if `id` is the one that has it

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

// --- at.cpp ----------------------------------------------------------------

// Is this render command painted by something inside at()? Those do not take
// the pointer (claim_pointer_over_painted() skips them): a health bar must not
// stop the enemy under it being clicked. The controls inside take it as
// controls do, through focus.cpp.
bool world_passthrough(const Clay_RenderCommand &command);

// --- notify.cpp ------------------------------------------------------------

// The very end of every pass, however it was closed -- end(), a scene's
// _draw() that left its frame open, the runner closing one after EndDrawing()
// -- and `drawn` when it was drawn. Where at() and tooltip() forget what they
// kept of the pass (end_at_pass(), end_tooltip_pass(), ui/floating.h): a pass
// that skipped it would leave the next one at()'s z ranges, and a layer
// there would let the pointer through. And, drawn, where a game with no scene
// stack has its notices drawn -- after the frame's FIRST pass
// (current_pass() == 0), there being no app frame to draw them after and no
// telling which end() is the last.
void pass_ended(bool drawn);

// --- backdrop.cpp ----------------------------------------------------------

// overlay() is opening a frame of its own: everything drawn so far this frame
// is the backdrop. With `blur` above 0 it is blurred and put on screen now,
// when this frame was redirected into a texture for it, and the next frame is
// redirected; with 0, the next frame is not.
void blur_backdrop(float blur);

// The texture and the shader go, while the window is still open: the UI is
// shutting down (shutdown_context()).
void release_backdrop();

// --- render.cpp ------------------------------------------------------------

void draw(Clay_RenderCommandArray commands);

// The radius each corner of a box is drawn with, in pixels: what Clay says,
// clamped to half the box's shorter side.
struct Corners {
    float top_left;
    float top_right;
    float bottom_right;
    float bottom_left;
};
Corners corners_of(Clay_CornerRadius radius, Clay_BoundingBox box);

// The outline a box with those corners is drawn along, inset on each side by
// `inset` -- a border's widths; zero for the box itself. CORNER_POINTS points
// per corner, a corner with no radius being that many copies of one point, so
// the outline of a box and of its border's inside pair up point for point. The
// corners go top-left, bottom-left, bottom-right, top-right.
inline constexpr int CORNER_POINTS = 9;
inline constexpr int BOX_OUTLINE = 4 * CORNER_POINTS;
using Outline = std::array<Vector2, BOX_OUTLINE>;
void box_outline(Clay_BoundingBox box, Corners corners, Clay_BorderWidth inset,
                 Outline &out);

// Clay hands out string slices that are NOT null terminated. raylib's
// DrawTextEx and MeasureTextEx both need one, so every slice has to be copied
// into a scratch buffer with a terminator first.
const char *cstr(Clay_StringSlice slice);

// The scissor rectangles open while a pass is drawn, innermost last. raylib
// has ONE scissor, so a scroll area inside another one set its own and the
// outer clip was gone -- and at its end it switched clipping off, so the rest
// of the outer area spilled out of it. Now each one opens cut to every one
// around it, and its end puts the one around it back. Pure, so a test can
// hold it to that without a window.
inline constexpr int MAX_SCISSORS = 8;
struct ScissorStack {
    std::array<Clay_BoundingBox, MAX_SCISSORS> boxes{};
    int depth = 0;
    int overflow = 0; // opened past the limit, so the closes still pair up
};
// What to clip to now that `box` opens: `box`, cut to what is open around it
// (no area at all when they do not meet).
Clay_BoundingBox push_scissor(ScissorStack &stack, Clay_BoundingBox box);
// What to clip to now that the innermost one closes: the one around it, or
// nothing when it was the outermost.
std::optional<Clay_BoundingBox> pop_scissor(ScissorStack &stack);

// --- seams for tests -------------------------------------------------------
//
// With these two replaced, the layout runs with no window and no GPU:
// Clay_EndLayout is pure computation. That is what makes headless layout tests
// possible (tests/ui_layout_test.cpp).

// What the keyboard typed this frame, for the text field that has it: the
// characters in the order they came, and how many times Backspace went down
// -- once when it is pressed, and once for each repeat while it is held. It is
// read by the field typing, when it types, and by nothing else: raylib's
// character queue empties as it is read, and a game reading it itself for a
// name of its own must find it there whenever no field of ours is typing.
inline constexpr int MAX_TYPED = 16; // raylib's own queue holds 16
struct Typed {
    std::array<int, MAX_TYPED> codepoints{};
    int count = 0;
    int backspaces = 0;
};
void read_typed(Typed &out);

using MeasureFn = Clay_Dimensions (*)(Clay_StringSlice, Clay_TextElementConfig *, void *);
using PointerFn = void (*)(Clay_Vector2 &position, bool &down);
using NavFn = void (*)(NavState &out);
using FontFn = rmp::Font (*)(const char *name, int pixel_size);
using TextFn = void (*)(Typed &out);

void set_measure_provider(MeasureFn fn);
void set_pointer_provider(PointerFn fn);
void set_nav_provider(NavFn fn);
void set_text_provider(TextFn fn); // nullptr puts raylib's back

// The [ui] font, and what bakes it at a pixel size: RMP_UI_FONT and
// rmp::assets::load_font, unless a test swaps them -- baking a face needs a GL
// context, and what decides WHICH sizes get baked is what is worth testing.
// Every face baked so far is let go of. nullptr puts each one back.
void set_font_provider(FontFn fn, const char *name);

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

// What the last pass laid out asked to be drawn: Clay's own array, valid until
// the next pass is laid out or the UI shuts down. For a test that holds what is
// drawn to the box it is drawn in -- a line of text to the width its command
// says, above all.
Clay_RenderCommandArray last_pass_commands();

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
void pointer_from_raylib(Clay_Vector2 &position, bool &down);
void nav_from_raylib(NavState &out);
void typed_from_raylib(Typed &out);

// What the keyboard means to navigation, on any keyboard: `down(key)` and
// `pressed(key)` answer for a raylib KeyboardKey, and `more(x, y, submit,
// cancel)` adds whatever else pushes -- the gamepad, nav_from_pad() below.
// One function, so the keys a player navigates with and the keys a scripted
// run presses (tests/e2e/driver.cpp) cannot drift apart. Down and Tab push
// down, Up up, Left and Right sideways; Shift+Tab goes back; Enter submits
// and activates, Space only activates (a text field types it); Escape
// cancels.
template <class Down, class Pressed, class More>
NavState nav_from_keys(Down down, Pressed pressed, More more) {
    int y = 0;
    if (down(KEY_DOWN) || down(KEY_TAB)) y += 1;
    if (down(KEY_UP)) y -= 1;
    int x = 0;
    if (down(KEY_RIGHT)) x += 1;
    if (down(KEY_LEFT)) x -= 1;
    bool submit = pressed(KEY_ENTER) || pressed(KEY_KP_ENTER);
    bool cancel = pressed(KEY_ESCAPE);
    more(x, y, submit, cancel);
    // Shift+Tab is "backwards", which is the one convention people expect
    // without being told.
    if (y > 0 && down(KEY_TAB) && (down(KEY_LEFT_SHIFT) || down(KEY_RIGHT_SHIFT))) y = -1;
    NavState out;
    out.x = x > 0 ? 1 : (x < 0 ? -1 : 0);
    out.y = y > 0 ? 1 : (y < 0 ? -1 : 0);
    out.activate = submit || pressed(KEY_SPACE);
    out.submit = submit;
    out.cancel = cancel;
    out.tab = pressed(KEY_TAB);
    return out;
}

// What the first gamepad adds to it, for nav_from_keys()' `more`: the d-pad,
// or the left stick pushed past half way to be deliberate, push; the bottom
// face button submits (and so activates), the right one cancels. `down(b)`
// and `pressed(b)` answer for a raylib GamepadButton, `axis(a)` for a
// GamepadAxis. One function for the same reason as the keyboard's:
// nav_from_raylib() reads a real pad through it, tests/e2e/driver.cpp a
// scripted one.
template <class Down, class Pressed, class Axis>
void nav_from_pad(Down down, Pressed pressed, Axis axis, int &x, int &y, bool &submit,
                  bool &cancel) {
    if (down(GAMEPAD_BUTTON_LEFT_FACE_DOWN)) y += 1;
    if (down(GAMEPAD_BUTTON_LEFT_FACE_UP)) y -= 1;
    if (down(GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) x += 1;
    if (down(GAMEPAD_BUTTON_LEFT_FACE_LEFT)) x -= 1;
    const float ly = axis(GAMEPAD_AXIS_LEFT_Y);
    if (ly > 0.5f) y += 1;
    if (ly < -0.5f) y -= 1;
    const float lx = axis(GAMEPAD_AXIS_LEFT_X);
    if (lx > 0.5f) x += 1;
    if (lx < -0.5f) x -= 1;
    submit = submit || pressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    cancel = cancel || pressed(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
}

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

// --- controls.cpp: UTF-8, as text_input() edits it --------------------------
//
// Apart from the field so that a test can hold them to the promise the field
// makes: a character is never cut in half, whatever it is cut for. Counting is
// by the bytes that START a character, so a string that is not valid UTF-8 is
// still counted and cut without reading past its end.

// How many characters `text` holds.
int utf8_length(std::string_view text);
// `codepoint` onto the end of `text`, as UTF-8, if it is a character a player
// types (not a control character, not a surrogate, not past U+10FFFF) and the
// text has room for it under `max_length` characters (0 = no limit).
bool utf8_append(std::string &text, int codepoint, int max_length);
// The last character off the end of `text`. False when there was none.
bool utf8_pop(std::string &text);
// `text` cut to `max_length` characters (0 = no limit), at the end of one.
// True when it was longer.
bool utf8_truncate(std::string &text, int max_length);

} // namespace rmp::ui::detail
