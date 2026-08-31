#pragma once
// ---------------------------------------------------------------------------
// rmp/app.h — the entry point, and closing the app.
//
// You write on_ready(), on_frame(float) and on_exit(); RMP_ENTRY_POINT writes
// the runner for your platform. There are three, and the difference between
// them is who owns the frame loop:
//
//   desktop   we do: a main() with a while loop, the ordinary raylib shape.
//   web       the browser does. See the PLATFORM_WEB block below — this is not
//             a style choice, a while loop there costs you ASYNCIFY.
//   iOS       UIKit does, so there is no loop to write and nothing to return
//             to: three callbacks it calls instead.
//
// All three do the same things around your code: start the smoke test, open the
// asset pack, run you, report to CI whether any asset failed to load, and close
// the pack afterwards. None of it is yours to remember.
//
// WHAT THIS HEADER DOES NOT INCLUDE, and why it matters.
//
// Not one header of ours. Not rmp/ui.h, not rmp/assets.h, not rmp/config.h.
// It used to include all three, because the macros below called
// rmp::ui::shutdown() and rmp::assets::init() by name — and that meant every
// translation unit with an entry point paid for the whole interface layer,
// which is exactly the coupling that splitting the umbrella was meant to end.
//
// The fix is that the macros no longer DO anything: they call six functions in
// rmp::app::detail, compiled once in src/rmp/app.cpp, which is where the
// includes live now. The macro is left as the only thing it has to be — the
// platform's entry-point shape.
//
// The rule that comes out of it, and it governs every header we add:
//   1. Include none of our headers. Forward-declare instead.
//   2. If a namespace or a by-value type makes that impossible, include it and
//      write down here why.
//   3. If two headers end up needing each other, one of them should not exist.
// ---------------------------------------------------------------------------

#include <raylib.h> // GetFrameTime(), for the frame hook. raylib's, not ours.

#if defined(PLATFORM_WEB) || defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

namespace rmp::app {

// Ask the app to close.
//
// Call it from anywhere — a menu callback, a game-over screen, ten frames deep
// in your own code. It does not need to know where main() is, and there is no
// value to propagate back up through your call stack.
//
//     if (rmp::ui::button("Quit")) rmp::app::quit();
//
// It RETURNS. What it does is raise a flag that the entry point checks at the
// top of the next iteration, so the frame you are in finishes normally and then
// the stop hook runs, the window closes and the asset pack is released — the
// same shutdown as closing the window with the X.
//
// That is the whole reason not to call std::exit() yourself: std::exit ends the
// process immediately, so the stop hook never runs, CloseWindow() never runs,
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
//   iOS                NOTHING. It logs a warning and returns.
//
//                      Apple's QA1561 is explicit: an iOS app that terminates
//                      itself "will appear to the user to have crashed", and
//                      App Review rejects apps that crash or appear to. A Quit
//                      control also fails the Human Interface Guidelines by
//                      itself. So the same source ships everywhere and the
//                      button is simply inert on iPhone — hide it with
//                      #if !defined(PLATFORM_IOS) if a dead control bothers you.
void quit();

// Has quit() been called? The entry point uses this; you are unlikely to need it
// unless you want to skip work on the way out.
bool quit_requested();

namespace detail {
// The six halves of a run. Not for you — RMP_ENTRY_POINT calls them, and they
// exist so that this header names nothing from rmp::ui or rmp::assets. Their
// bodies, and the includes they need, are in src/rmp/app.cpp.
void begin_run(); // smoke test on, chdir into the bundle on iOS, assets open
void after_ready(); // report to CI whether any asset failed to load
bool keep_running(); // the window is open, the frame budget is not spent, no quit
void end_frame(); // advance the CI frame budget
void begin_stop(); // the UI closes here, BEFORE your stop hook: see below
void end_stop(); // the asset pack closes here, AFTER it
#if defined(PLATFORM_IOS)
[[noreturn]] void exit_process(); // only iOS needs it, and only under CI
#endif
} // namespace detail

} // namespace rmp::app

// An ordering detail that is not obvious, and is the reason begin_stop and
// end_stop are two functions rather than one. Your stop hook is where
// CloseWindow() lives, so the rule is:
//
//   begin_stop, BEFORE your hook   everything that owns something on the GPU —
//                                  the UI's font, and every live rmp::Texture,
//                                  rmp::Font and rmp::RenderTexture.
//   end_stop, AFTER your hook      everything that does not — the rres pack and
//                                  raylib's loader hook, which are file
//                                  handles and do not care about the window.
//
// This comment used to say the asset layer could close last "because it owns no
// GPU objects". That was true until rmp::Texture existed, and the day it did,
// the release moved to the wrong side of CloseWindow() and the game segfaulted
// on the way out — under xvfb only, because a real driver tolerated it.

// clang-format off
// The one exemption left in the repository, and it is NOT about alignment:
// these are macros whose bodies contain block comments, and every formatter
// mangles the continuation of a /* */ inside a macro. The trailing backslashes
// are a language requirement, not a ruler.

// iOS rcore declares: extern void ios_ready(); ios_update(bool); ios_destroy();
// extern "C" so the symbols match the C declarations in rcore_ios.c.
//
// UIKit owns the run loop and offers no way to return from it, so the CI path
// tears down and exits explicitly. Outside CI that branch never runs.
#define RMP_IOS_FUNCS(READY, FRAME, STOP)                                      \
  extern "C" void ios_ready() {                                                \
    rmp::app::detail::begin_run();                                             \
    READY();                                                                   \
    rmp::app::detail::after_ready();                                           \
  }                                                                            \
  extern "C" void ios_update(bool /*viewResized*/) {                           \
    FRAME(GetFrameTime());                                                     \
    /* Two ways out, and both land here because UIKit never gives the run loop \
       back: the CI frame budget, and rmp::app::quit(). */                     \
    if (!rmp::app::detail::keep_running()) {                                   \
      rmp::app::detail::begin_stop();                                          \
      STOP();                                                                  \
      rmp::app::detail::end_stop();                                            \
      rmp::app::detail::exit_process();                                        \
    }                                                                          \
  }                                                                            \
  extern "C" void ios_destroy() {                                              \
    rmp::app::detail::begin_stop();                                            \
    STOP();                                                                    \
    rmp::app::detail::end_stop();                                              \
  }

// Web. The browser owns the frame loop, and giving it to the browser is worth
// a paragraph because the alternative is expensive in a way that is invisible.
//
// A while loop in a browser only works if you build with -s ASYNCIFY, which
// rewrites the whole program so its stack can be unwound and restored at any
// suspension point. That instrumentation is not billed to the loop: it is
// billed to every function in the binary that might be on the stack when it
// happens, in code size and in speed, whether or not the frame ever yields.
// raylib says so itself, in the comment above WindowShouldClose() on web:
// "WindowShouldClose() is not called on a web-ready raylib application if
// using emscripten_set_main_loop()". That call is the only emscripten_sleep()
// in raylib, so not calling it is what lets ASYNCIFY go away entirely.
//
// fps = 0 means requestAnimationFrame, which is the right answer on web — do
// NOT call SetTargetFPS() here. The 1 is "simulate an infinite loop", so
// main() never returns and nothing after the call runs, which is exactly the
// contract the desktop while loop has.
#define RMP_WEB_FUNCS(READY, FRAME, STOP)                                      \
  static void rmp_web_frame() {                                                \
    FRAME(GetFrameTime());                                                     \
    /* WindowShouldClose() is deliberately never called: on web it does        \
       nothing but emscripten_sleep(12), which is what ASYNCIFY pays for. */   \
    if (!rmp::app::detail::keep_running()) {                                   \
      emscripten_cancel_main_loop();                                           \
      rmp::app::detail::begin_stop();                                          \
      STOP();                                                                  \
      rmp::app::detail::end_stop();                                            \
    }                                                                          \
  }                                                                            \
  int main() {                                                                 \
    rmp::app::detail::begin_run();                                             \
    READY();                                                                   \
    rmp::app::detail::after_ready();                                           \
    emscripten_set_main_loop(rmp_web_frame, 0, 1);                             \
    return 0;                                                                  \
  }

#define RMP_DESKTOP_FUNCS(READY, FRAME, STOP)                                  \
  int main() {                                                                 \
    rmp::app::detail::begin_run();                                             \
    READY();                                                                   \
    rmp::app::detail::after_ready();                                           \
    /* rmp::app::quit() is checked inside keep_running() rather than acted on  \
       where it is called: the frame that asked to quit finishes normally, and \
       then the stop hook and CloseWindow() run exactly as they do when the    \
       window is closed with the X. */                                         \
    while (rmp::app::detail::keep_running()) {                                 \
      FRAME(GetFrameTime());                                                   \
      rmp::app::detail::end_frame();                                           \
    }                                                                          \
    rmp::app::detail::begin_stop();                                            \
    STOP();                                                                    \
    rmp::app::detail::end_stop();                                              \
    return 0;                                                                  \
  }
// clang-format on

// The entry point. Give it your three hooks; it picks the right runner.
//
//     RMP_ENTRY_POINT(on_ready, on_frame, on_exit);
//
#if defined(PLATFORM_IOS)
#define RMP_ENTRY_POINT(READY, FRAME, STOP) RMP_IOS_FUNCS(READY, FRAME, STOP)
#elif defined(PLATFORM_WEB) || defined(__EMSCRIPTEN__)
#define RMP_ENTRY_POINT(READY, FRAME, STOP) RMP_WEB_FUNCS(READY, FRAME, STOP)
#else
#define RMP_ENTRY_POINT(READY, FRAME, STOP) RMP_DESKTOP_FUNCS(READY, FRAME, STOP)
#endif
