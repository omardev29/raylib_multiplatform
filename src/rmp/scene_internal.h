#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. The scene stack as the app drives it.
//
// The public surface is include/rmp/scene.h. Everything here is the half the
// user never calls: run one frame, apply what the frame asked for, tear the
// stack down. tests/scene_test.cpp includes this directly, which is what lets
// the whole stack be tested without a window — see the note at the bottom.
// ---------------------------------------------------------------------------

#include <raylib.h> // Color, for clear_color()

namespace rmp {
class Scene;
} // namespace rmp

namespace rmp::scenes::detail {

// Put the first scene on the stack and run its _ready(). Called by
// rmp::app::detail::start(), which the RMP_GAME macro wires to the platform's
// ready hook.
void start(Scene *first); // TAKES OWNERSHIP

// The update pass: bottom of the stack upwards, stopping at the lowest scene
// that something above has frozen.
void update(float delta);

// The draw pass, bottom upwards, starting at the lowest scene that something
// above has not covered. The caller has already cleared the frame.
void draw();

// The colour the frame should be cleared with: the background of the lowest
// scene that draws, or the theme's when that scene left it BLANK.
Color clear_color();

// Apply everything change/push/replace/pop/quit recorded during the frame.
// Runs after EndDrawing(), so nothing is destroyed while it is on the stack of
// the call that asked for it.
void apply_pending();

// Unwind the stack, top down, calling _end() on each. Runs while the window is
// still open and before the resource table is released, because a scene can
// hold an rmp::Texture and its destructor has to find its slot still there.
void shutdown();

// True once start() has run and until shutdown() finishes. The UI uses it to
// tell "inside an app" from "someone calling rmp::ui::begin() on their own".
bool running();

// --- the test seam ---------------------------------------------------------
// Everything above is arithmetic and callbacks. Nothing in this file touches
// the GPU, so tests/scene_test.cpp drives update/draw/apply_pending directly
// and compares the transcript of events. draw() is the only one that would,
// and it goes through the same test_mode() switch rmp::ui already has.

} // namespace rmp::scenes::detail
