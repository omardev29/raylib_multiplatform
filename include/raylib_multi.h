#pragma once

#include <raylib.h>
#ifdef __ANDROID__
#include <raymob.h>
#endif // __ANDROID__
#include <admob.h>
#include <assets.h>
#include <smoke_test.h> // CI smoke-test hook (lives in tests/)
#include <test.h>

// cross compiler inlining macro to force the inlining
#if defined(_MSC_VER)
#define inlining static __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define inlining static inline __attribute__((always_inline))
#else
#define inlining static inline
#endif

// More colors
#ifndef ALICEBLUE
#define ALICEBLUE CLITERAL(Color){0, 240, 248, 255}
#endif
#define GIORNOGOLD CLITERAL(Color){238, 207, 34, 255} // The Golden Experience

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
       is exactly where bundle resources live. */                              \
    SmokeTest_Begin();                                                         \
    ChangeDirectory(GetApplicationDirectory());                                \
    _ready();                                                                  \
    SmokeTest_ReportBoot(assets.rabbit.width, assets.rabbit.height);           \
  }                                                                            \
  extern "C" void ios_update(bool /*viewResized*/) {                           \
    _process(GetFrameTime());                                                  \
    if (SmokeTest_Tick()) {                                                    \
      _exit();                                                                 \
      exit(0);                                                                 \
    }                                                                          \
  }                                                                            \
  extern "C" void ios_destroy() { _exit(); }

// Main loop body macro
// 1. Definición condicional de la macro
#if defined(PLATFORM_IOS)
#define RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY IOS_FUNCS
#else
#define RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY                                    \
  int main() {                                                                 \
    SmokeTest_Begin();                                                         \
    _ready();                                                                  \
    int smokeDone = 0;                                                         \
    while (!WindowShouldClose() && !smokeDone) {                               \
      _process(GetFrameTime());                                                \
      smokeDone = SmokeTest_Tick();                                            \
    }                                                                          \
    _exit();                                                                   \
  }
#endif
