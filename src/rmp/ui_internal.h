#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. The UI as the rest of the framework drives it: the
// app's frame (app.cpp), the scene stack (scene.cpp) and rmp::input
// (input.cpp) call these; the UI defines them.
//
// The public surface is include/rmp/ui.h, and the UI's own private half --
// where Clay lives -- is src/rmp/ui/internal.h. This one includes no Clay, so
// the files that only call into the UI do not pay for it.
// ---------------------------------------------------------------------------

#include <raylib.h> // Camera2D, and the keys and buttons listed below

#include <array>
#include <string_view>

namespace rmp::ui::detail {

// --- the frame, as the runner marks it (ui/context.cpp) --------------------
// Around a frame hook of RMP_ENTRY_POINT's -- rmp::app::detail::begin_frame()
// and end_frame() -- so that a game with no scenes and two begin()/end() pairs
// (a screen and its overlay()) has one UI frame and not two: the pointer
// sampled once, released once. With RMP_GAME, rmp::app::detail::frame() marks
// it itself, and these leave it alone. end_frame() is also where a frame the
// game left open is reported and closed.
void runner_begin_frame();
void runner_end_frame();

// --- where the interface is being described (ui/context.cpp) ---------------
// The scene stack says which part of a frame is running, so that a widget
// called where nothing can be drawn says so once instead of doing nothing in
// silence: UPDATE around every _update() (the scenes' and their objects'),
// WORLD around the map and the objects' _draw(), which run under the camera,
// DRAW around a scene's own _draw(), and FREE outside the stack -- which is
// all there is without scenes.
enum class Phase { FREE, UPDATE, WORLD, DRAW };
void set_phase(Phase phase);

// After each scene's _draw(): the scene at `index` of the stack (0 the
// bottom, `top` the one on top) has finished describing its interface. A
// frame it left open is reported, naming it by where it is, and closed.
void after_scene_draw(int index, int top);

// --- HUD focus (ui/focus.cpp) ----------------------------------------------
// rmp::input calls note_game_keys() when the game reads one of the keys
// below, by action or by name. The scene stack takes the note around each
// scene's update -- what that scene and its objects read -- and hands it to
// the scene's passes with set_pass_playing(): a pass over a game being played
// gives no focus by itself (FrameOptions::focus_first). Without scenes nobody
// takes it, and the UI decides from what the frame read.
void note_game_keys();
bool take_game_keys(); // and forget it
void set_pass_playing(bool playing);

// The keys the interface navigates with: what nav_from_keys() and
// nav_from_pad() (ui/internal.h) read, without the two that only cancel
// (Escape, the B button) -- a pause bound to Escape is not playing -- and
// Shift, which only turns Tab round. tests/hud_focus_test.cpp holds the two
// to each other, key by key.
inline constexpr std::array UI_KEYS{ KEY_ENTER, KEY_KP_ENTER, KEY_SPACE, KEY_UP,
                                     KEY_DOWN,  KEY_LEFT,     KEY_RIGHT, KEY_TAB };
inline constexpr std::array UI_PAD_BUTTONS{ GAMEPAD_BUTTON_LEFT_FACE_UP,
                                            GAMEPAD_BUTTON_LEFT_FACE_DOWN,
                                            GAMEPAD_BUTTON_LEFT_FACE_LEFT,
                                            GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
                                            GAMEPAD_BUTTON_RIGHT_FACE_DOWN };
inline constexpr std::array UI_PAD_AXES{ GAMEPAD_AXIS_LEFT_X, GAMEPAD_AXIS_LEFT_Y };

// --- at() (ui/at.cpp) ------------------------------------------------------
// The camera the pass about to be described sees the world through: the
// scene's, set before its _draw(), and the identity -- the world is the
// screen -- outside the stack and without scenes.
void set_pass_camera(const Camera2D &camera);

// --- the backdrop (ui/backdrop.cpp) ----------------------------------------
// rmp::app::detail::frame() calls these right after BeginDrawing() and after
// the last scene has drawn. When last frame's overlay() asked for a blur, the
// frame between the two is drawn into a texture, which the overlay blurs and
// puts on screen -- or which finish_backdrop() puts there as it is, when no
// overlay took it this time. Nothing at all without a window, and nothing
// when no overlay blurred.
void begin_backdrop();
void finish_backdrop();

// --- notices (ui/notify.cpp) -----------------------------------------------
// rmp::app::detail::frame() calls draw_notices() after everything else has
// drawn and before the frame is read back: what rmp::ui::notify() queued,
// laid out apart from the game's passes -- in a Clay context of the notices'
// own (ui/notify.cpp says why) -- taking no input and no focus. It starts
// nothing when nothing is queued, so a game that never notifies never starts
// the UI.
void draw_notices();

// The report sink a debug build installs (rmp::detail::set_report_sink, in
// src/rmp/internal.h): a framework warning, on screen. It only queues.
void show_report(std::string_view line);

} // namespace rmp::ui::detail
