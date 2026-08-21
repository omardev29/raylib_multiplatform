#pragma once
// ---------------------------------------------------------------------------
// The entry point.
//
// You write _ready(), _process(float) and _exit(); the macro below writes the
// runner for your platform. There are three, and the difference between them is
// who owns the frame loop:
//
//   desktop   we do: a main() with a while loop, the ordinary raylib shape.
//   web       the browser does. See the PLATFORM_WEB block below — this is not
//             a style choice, a while loop there costs you ASYNCIFY.
//   iOS       UIKit does, so there is no loop to write and nothing to return
//             to: three callbacks it calls instead.
//
// Both spellings do the same things around your code: start the smoke test,
// open the asset pack, run you, report to CI whether any asset failed to load,
// and close the pack afterwards. None of it is yours to remember, and there is
// nothing you have to keep in _ready() to keep CI happy.
//
// One ordering detail that is not obvious: rmp::ui::shutdown() runs BEFORE
// _exit(), not after. _exit() is where you call CloseWindow(), and releasing a
// font after that is touching a GL context that no longer exists. The UI is
// not used again after the loop ends, so closing it first costs nothing. The
// asset layer is the opposite case — it owns no GPU objects, so it closes
// after you, in case _exit() still wants to unload something.
// ---------------------------------------------------------------------------

#include <stdlib.h> // exit() on the iOS CI path

#if defined(PLATFORM_WEB) || defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

#include <raylib.h>
#include <raylib_multiplatform/assets.h>
#include <raylib_multiplatform/ui.h>
#include <raylib_multiplatform/utils.h>
#include <smoke_test.h>

// Hand-laid, and exempt from clang-format on purpose: these are 40-line macros
// whose bodies are block comments, and no formatter aligns that well. The
// continuation backslashes are a table, so they are kept as one.
// clang-format off

// iOS rcore declares: extern void ios_ready(); ios_update(bool); ios_destroy();
// extern "C" so the symbols match the C declarations in rcore_ios.c.
//
// The SmokeTest_Tick() branch is what makes the app terminable under CI. On
// desktop the frame budget just ends the while loop in main(); UIKit owns the
// run loop here and offers no way to return from it, so the CI path tears down
// and exits explicitly. Outside CI (RAY_TEST_MAX_FRAMES unset) Tick() always
// returns 0 and this is dead code.
#define IOS_FUNCS                                                              \
  extern "C" void ios_ready() {                                                \
    /* iOS starts the process in the app container, not inside the bundle, and \
       raylib's iOS backend does not chdir for you — so the relative         \
       RESOURCES_PATH would resolve to nothing and every asset would silently  \
       load as 0x0. GetApplicationDirectory() is the .app root on iOS, which   \
       is exactly where bundle resources live. This has to happen before       \
       rmp::assets::init(), which looks for the pack along that same path. */       \
    SmokeTest_Begin();                                                         \
    ChangeDirectory(GetApplicationDirectory());                                \
    rmp::assets::init();                                                            \
    _ready();                                                                  \
    SmokeTest_ReportBoot(rmp::assets::failed_loads(), rmp::assets::requested_loads());     \
  }                                                                            \
  extern "C" void ios_update(bool /*viewResized*/) {                           \
    _process(GetFrameTime());                                                  \
    /* Two ways out, and both land here because UIKit never gives the run loop \
       back: the CI frame budget, and rmp::utils::exit(). Apple discourages    \
       quitting programmatically, but a template that silently ignored a Quit  \
       button on one platform out of fourteen would be worse. */               \
    if (SmokeTest_Tick() || rmp::utils::exit_requested()) {                    \
      rmp::ui::shutdown();                                                     \
      _exit();                                                                 \
      rmp::assets::shutdown();                                                 \
      exit(0);                                                                 \
    }                                                                          \
  }                                                                            \
  extern "C" void ios_destroy() {                                              \
    rmp::ui::shutdown();                                                       \
    _exit();                                                                   \
    rmp::assets::shutdown();                                                   \
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
// One frame per callback is also what requestAnimationFrame wants: the browser
// schedules us with the display instead of us blocking its event loop and
// asking it politely for control back every 12 ms.
//
// fps = 0 means requestAnimationFrame, which is the right answer on web — do
// NOT call SetTargetFPS() here. The 1 is "simulate an infinite loop", so
// main() never returns and nothing after the call runs, which is exactly the
// contract the desktop while loop has.
#define WEB_FUNCS                                                              \
  static void rmp_web_close() {                                                \
    /* The same order as everywhere else: the UI first, while the window is    \
       still open and its font still has a GL context to live in. */           \
    rmp::ui::shutdown();                                                       \
    _exit();                                                                   \
    rmp::assets::shutdown();                                                   \
  }                                                                            \
  static void rmp_web_frame() {                                                \
    _process(GetFrameTime());                                                  \
    /* Same two ways out as the desktop loop, minus the window's X, which a    \
       browser tab does not have. WindowShouldClose() is deliberately never    \
       called: on web it does nothing but emscripten_sleep(12). */             \
    if (SmokeTest_Tick() || rmp::utils::exit_requested()) {                    \
      emscripten_cancel_main_loop();                                           \
      rmp_web_close();                                                         \
    }                                                                          \
  }                                                                            \
  int main() {                                                                 \
    SmokeTest_Begin();                                                         \
    rmp::assets::init();                                                       \
    _ready();                                                                  \
    SmokeTest_ReportBoot(rmp::assets::failed_loads(), rmp::assets::requested_loads()); \
    emscripten_set_main_loop(rmp_web_frame, 0, 1);                             \
    return 0;                                                                  \
  }

// Main loop body macro
#if defined(PLATFORM_IOS)
#define RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY IOS_FUNCS
#elif defined(PLATFORM_WEB) || defined(__EMSCRIPTEN__)
#define RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY WEB_FUNCS
#else
#define RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY                                    \
  int main() {                                                                 \
    SmokeTest_Begin();                                                         \
    rmp::assets::init();                                                            \
    _ready();                                                                  \
    SmokeTest_ReportBoot(rmp::assets::failed_loads(), rmp::assets::requested_loads());     \
    int smokeDone = 0;                                                         \
    /* rmp::utils::exit() is checked here rather than acted on where it is     \
       called: the frame that asked to quit finishes normally, and then        \
       _exit() and CloseWindow() run exactly as they do when the window is     \
       closed with the X. std::exit() in a button handler skips all of that. */\
    while (!WindowShouldClose() && !smokeDone && !rmp::utils::exit_requested()) { \
      _process(GetFrameTime());                                                \
      smokeDone = SmokeTest_Tick();                                            \
    }                                                                          \
    rmp::ui::shutdown();                                                       \
    _exit();                                                                   \
    rmp::assets::shutdown();                                                   \
    return 0;                                                                  \
  }
#endif

// clang-format on
