#pragma once
// ---------------------------------------------------------------------------
// rmp::utils — the small things that have no other home.
//
// Right now that is exactly one thing: closing the app. More will land here as
// it earns its place; the point of the namespace is that a helper never has to
// go looking for a category to belong to.
// ---------------------------------------------------------------------------

namespace rmp::utils {

// Ask the app to close.
//
// Call it from anywhere — a menu callback, a game-over screen, ten frames deep
// in your own code. It does not need to know where main() is, and there is no
// value to propagate back up through your call stack.
//
//     if (rmp::ui::button("Quit")) rmp::utils::exit();
//
// It RETURNS. What it does is raise a flag that the entry point checks at the
// top of the next iteration, so the frame you are in finishes normally and
// then _exit() runs, the window closes and the asset pack is released — the
// same shutdown as closing the window with the X.
//
// That is the whole reason not to call std::exit() yourself: std::exit ends
// the process immediately, so _exit() never runs, CloseWindow() never runs,
// and on Android the Activity is left behind while its process disappears.
//
// It is not an exception either, and deliberately: exceptions are switched off
// in plenty of game builds, unwinding through raylib's C frames is undefined,
// and throwing mid-frame would leave BeginDrawing() unbalanced and a UI frame
// open. A flag costs one branch per frame and cannot leave anything half done.
//
// Per platform:
//   desktop, BSD, Web  the frame loop ends and main() returns
//   Android            plus ANativeActivity_finish(), so the Activity actually
//                      goes away instead of leaving a task behind
//   iOS                UIKit owns the run loop and never gives it back, so the
//                      app tears down and exits. Apple discourages quitting
//                      programmatically — offer a menu, not a Quit button.
void exit();

// Has exit() been called? The entry point uses this; you are unlikely to need
// it unless you want to skip work on the way out.
bool exit_requested();

} // namespace rmp::utils
