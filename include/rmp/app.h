#pragma once
// ---------------------------------------------------------------------------
// rmp/app.h — the entry point, and closing the app.
//
// You write on_ready(), on_frame(float) and on_exit(); RMP_ENTRY_POINT writes the
// runner for your platform. There are three, and the difference between them is
// who owns the frame loop:
//
//   desktop   we do: a main() with a while loop, the ordinary raylib shape.
//   web       the browser does. See the PLATFORM_WEB block below — this is not
//             a style choice, a while loop there costs you ASYNCIFY.
//   iOS       UIKit does, so there is no loop to write and nothing to return
//             to: three callbacks it calls instead.
//
// All three do the same things around your code: start the smoke test, open the
// asset pack, run you, report to CI whether any asset failed to load, and close
// the pack afterwards. None of it is yours to remember, and there is nothing you
// have to keep in on_ready() to keep CI happy.
//
// One ordering detail that is not obvious: rmp::ui::shutdown() runs BEFORE the
// stop hook, not after. That hook is where you call CloseWindow(), and releasing
// a font after that is touching a GL context that no longer exists. The UI is
// not used again once the loop ends, so closing it first costs nothing. The
// asset layer is the opposite case — it owns no GPU objects, so it closes after
// you, in case your stop hook still wants to unload something.
//
// The three hooks are macro ARGUMENTS rather than fixed names because the scene
// layer is about to supply its own. See next_architecture/03-app-and-scenes.md.
// ---------------------------------------------------------------------------

#include <stdlib.h> // exit() on the iOS CI path

#if defined(PLATFORM_WEB) || defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#ifdef __ANDROID__
// The Android side of raylib — vibration, the soft keyboard, the activity's JNI
// handles. Android only: it needs the NDK's <jni.h>, which no other target has.
#include <raymob.h>
#endif

#include <raylib.h>
#include <rmp/assets.h>
#include <rmp/config.h>
#include <rmp/ui.h>
#include <smoke_test.h> // the CI gate; every function is a no-op outside CI

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

} // namespace rmp::app

// clang-format off
// The one exemption left in the repository, and it is NOT about alignment:
// these are 40-line macros whose bodies contain block comments, and every
// formatter mangles the continuation of a /* */ inside a macro. The trailing
// backslashes are a language requirement, not a ruler.

// iOS rcore declares: extern void ios_ready(); ios_update(bool); ios_destroy();
// extern "C" so the symbols match the C declarations in rcore_ios.c.
//
// The SmokeTest_Tick() branch is what makes the app terminable under CI. On
// desktop the frame budget just ends the while loop in main(); UIKit owns the
// run loop here and offers no way to return from it, so the CI path tears down
// and exits explicitly. Outside CI (RAY_TEST_MAX_FRAMES unset) Tick() always
// returns 0 and this is dead code.
#define RMP_IOS_FUNCS(READY, FRAME, STOP)                                      \
  extern "C" void ios_ready() {                                                \
    /* iOS starts the process in the app container, not inside the bundle, and \
       raylib's iOS backend does not chdir for you — so the relative           \
       RESOURCES_PATH would resolve to nothing and every asset would silently  \
       load as 0x0. GetApplicationDirectory() is the .app root on iOS, which   \
       is exactly where bundle resources live. This has to happen before       \
       rmp::assets::init(), which looks for the pack along that same path. */  \
    SmokeTest_Begin();                                                         \
    ChangeDirectory(GetApplicationDirectory());                                \
    rmp::assets::init();                                                       \
    READY();                                                                   \
    SmokeTest_ReportBoot(rmp::assets::failed_loads(),                          \
                         rmp::assets::requested_loads());                      \
  }                                                                            \
  extern "C" void ios_update(bool /*viewResized*/) {                           \
    FRAME(GetFrameTime());                                                     \
    /* Two ways out, and both land here because UIKit never gives the run loop \
       back: the CI frame budget, and rmp::app::quit(). Apple discourages      \
       quitting programmatically, but a framework that silently ignored a Quit \
       button on one platform out of fourteen would be worse. */               \
    if (SmokeTest_Tick() || rmp::app::quit_requested()) {                      \
      rmp::ui::shutdown();                                                     \
      STOP();                                                                  \
      rmp::assets::shutdown();                                                 \
      exit(0);                                                                 \
    }                                                                          \
  }                                                                            \
  extern "C" void ios_destroy() {                                              \
    rmp::ui::shutdown();                                                       \
    STOP();                                                                    \
    rmp::assets::shutdown();                                                   \
  }

// Web. The browser owns the frame loop, and giving it to the browser is worth
// a paragraph because the alternative is expensive in a way that is invisible.
//
// A while loop in a browser only works if you build with -s ASYNCIFY, which
// rewrites the whole program so its stack can be unwound and restored at any
// suspension point. That instrumentation is not billed to the loop: it is
// billed to every function in the binary that might be on the stack when it
// happens, in code Size and in speed, whether or not the frame ever yields.
// raylib says so itself, in the comment above WindowShouldClose() on web:
// "WindowShouldClose() is not called on a web-ready raylib application if
// using emscripten_set_main_loop()". That call is the only emscripten_sleep()
// in raylib, so not calling it is what lets ASYNCIFY go away entirely.
//
// One frame per callback is also what requestAnimationFrame wants: the browser
// schedules us with the display instead of us blocking its event loop and
// asking it politely for control back every 12 ms.
//
// fps = 0 means requestAnimationFrame, which is the right answer on web — do
// NOT call SetTargetFPS() here. The 1 is "simulate an infinite loop", so
// main() never returns and nothing after the call runs, which is exactly the
// contract the desktop while loop has.
#define RMP_WEB_FUNCS(READY, FRAME, STOP)                                      \
  static void rmp_web_close() {                                               \
    /* The same order as everywhere else: the UI first, while the window is    \
       still open and its font still has a GL context to live in. */           \
    rmp::ui::shutdown();                                                       \
    STOP();                                                                    \
    rmp::assets::shutdown();                                                   \
  }                                                                            \
  static void rmp_web_frame() {                                               \
    FRAME(GetFrameTime());                                                     \
    /* Same two ways out as the desktop loop, minus the window's X, which a    \
       browser tab does not have. WindowShouldClose() is deliberately never    \
       called: on web it does nothing but emscripten_sleep(12). */             \
    if (SmokeTest_Tick() || rmp::app::quit_requested()) {                      \
      emscripten_cancel_main_loop();                                           \
      rmp_web_close();                                                         \
    }                                                                          \
  }                                                                            \
  int main() {                                                                 \
    SmokeTest_Begin();                                                         \
    rmp::assets::init();                                                       \
    READY();                                                                   \
    SmokeTest_ReportBoot(rmp::assets::failed_loads(),                          \
                         rmp::assets::requested_loads());                      \
    emscripten_set_main_loop(rmp_web_frame, 0, 1);                             \
    return 0;                                                                  \
  }

#define RMP_DESKTOP_FUNCS(READY, FRAME, STOP)                                  \
  int main() {                                                                 \
    SmokeTest_Begin();                                                         \
    rmp::assets::init();                                                       \
    READY();                                                                   \
    SmokeTest_ReportBoot(rmp::assets::failed_loads(),                          \
                         rmp::assets::requested_loads());                      \
    int smokeDone = 0;                                                         \
    /* rmp::app::quit() is checked here rather than acted on where it is       \
       called: the frame that asked to quit finishes normally, and then the    \
       stop hook and CloseWindow() run exactly as they do when the window is   \
       closed with the X. std::exit() in a button handler skips all of that. */\
    while (!WindowShouldClose() && !smokeDone && !rmp::app::quit_requested()) { \
      FRAME(GetFrameTime());                                                   \
      smokeDone = SmokeTest_Tick();                                            \
    }                                                                          \
    rmp::ui::shutdown();                                                       \
    STOP();                                                                    \
    rmp::assets::shutdown();                                                   \
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
