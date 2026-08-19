#pragma once
// ---------------------------------------------------------------------------
// The entry point.
//
// You write _ready(), _process(float) and _exit(); the macro below writes the
// runner for your platform. Desktop and Web get a main() with the frame loop;
// iOS gets the three callbacks UIKit expects, because there the run loop
// belongs to the OS and there is nothing to return to.
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

#include <raylib.h>
#include <raylib_multiplatform/assets.h>
#include <raylib_multiplatform/ui.h>
#include <raylib_multiplatform/utils.h>
#include <smoke_test.h>

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

// Main loop body macro
#if defined(PLATFORM_IOS)
#define RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY IOS_FUNCS
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
