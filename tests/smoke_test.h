// ---------------------------------------------------------------------------
// tests/smoke_test.h — CI smoke-test hook for the raylib template.
//
// Lets the headless CI boot the game, render a bounded number of frames and
// exit cleanly (exit code 0) instead of looping until the window is closed.
// Enable it by setting the env var RAY_TEST_MAX_FRAMES=<N> before launching.
// In a normal run the variable is unset and the game behaves exactly as before.
//
// This is a single-file, header-only module: include it in EXACTLY ONE
// translation unit (your main.cpp). It is intentionally dependency-light so it
// compiles on every target (desktop, Web, Android, iOS) without wiring extra
// sources into each build system. You normally never need to touch it.
//
// Markers emitted (the CI greps for these):
//   RAY_TEST_BOOT_OK      — the game booted and loaded its assets
//   RAY_TEST_DONE_FRAMES  — the frame budget was rendered, exiting cleanly
// ---------------------------------------------------------------------------

#ifndef SMOKE_TEST_H
#define SMOKE_TEST_H

#include <raylib.h>   // TraceLog, LOG_INFO
#include <stdlib.h>   // getenv, atoi

static int SmokeTest_frame     = 0;
static int SmokeTest_maxFrames = 0;   // 0 = run until the window is closed

// Call once at startup, before entering the game loop. Reads RAY_TEST_MAX_FRAMES.
static inline void SmokeTest_Begin(void) {
    const char* env = getenv("RAY_TEST_MAX_FRAMES");
    if (env && env[0]) SmokeTest_maxFrames = atoi(env);
}

// Call once after your assets have loaded. Emits the boot marker the CI greps
// for. texW/texH are the size of a loaded asset, proving assets loaded.
static inline void SmokeTest_ReportBoot(int texW, int texH) {
    TraceLog(LOG_INFO, "RAY_TEST_BOOT_OK texture=%dx%d testFrames=%d",
             texW, texH, SmokeTest_maxFrames);
}

// Call once per frame, AFTER drawing. Returns non-zero when the frame budget
// is exhausted (and emits the done marker), signalling the app should exit.
static inline int SmokeTest_Tick(void) {
    if (SmokeTest_maxFrames <= 0) return 0;
    if (++SmokeTest_frame >= SmokeTest_maxFrames) {
        TraceLog(LOG_INFO, "RAY_TEST_DONE_FRAMES rendered=%d", SmokeTest_frame);
        return 1;
    }
    return 0;
}

#endif // SMOKE_TEST_H
