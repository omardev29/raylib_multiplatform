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
// ---------------------------------------------------------------------------

#include <stdlib.h> // exit() on the iOS CI path

#include <raylib.h>
#include <raylib_multiplatform/assets.h>
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
       assets::Init(), which looks for the pack along that same path. */       \
    SmokeTest_Begin();                                                         \
    ChangeDirectory(GetApplicationDirectory());                                \
    assets::Init();                                                            \
    _ready();                                                                  \
    SmokeTest_ReportBoot(assets::FailedLoads(), assets::RequestedLoads());     \
  }                                                                            \
  extern "C" void ios_update(bool /*viewResized*/) {                           \
    _process(GetFrameTime());                                                  \
    if (SmokeTest_Tick()) {                                                    \
      _exit();                                                                 \
      assets::Shutdown();                                                      \
      exit(0);                                                                 \
    }                                                                          \
  }                                                                            \
  extern "C" void ios_destroy() {                                              \
    _exit();                                                                   \
    assets::Shutdown();                                                        \
  }

// Main loop body macro
#if defined(PLATFORM_IOS)
#define RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY IOS_FUNCS
#else
#define RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY                                    \
  int main() {                                                                 \
    SmokeTest_Begin();                                                         \
    assets::Init();                                                            \
    _ready();                                                                  \
    SmokeTest_ReportBoot(assets::FailedLoads(), assets::RequestedLoads());     \
    int smokeDone = 0;                                                         \
    while (!WindowShouldClose() && !smokeDone) {                               \
      _process(GetFrameTime());                                                \
      smokeDone = SmokeTest_Tick();                                            \
    }                                                                          \
    _exit();                                                                   \
    assets::Shutdown();                                                        \
    return 0;                                                                  \
  }
#endif
