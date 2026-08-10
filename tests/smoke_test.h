// ---------------------------------------------------------------------------
// tests/smoke_test.h — CI smoke-test hook for the raylib template.
//
// Lets the headless CI boot the game, render a bounded number of frames and
// exit cleanly (exit code 0) instead of looping until the window is closed.
// Enable it by setting the env var RAY_TEST_MAX_FRAMES=<N> before launching.
// In a normal run the variable is unset and every function here is a no-op, so
// shipped builds behave exactly as if this file did not exist.
//
// This is a single-file, header-only module: include it in EXACTLY ONE
// translation unit (your main.cpp). It is intentionally dependency-light so it
// compiles on every target (desktop, Web, Android, iOS) without wiring extra
// sources into each build system. You normally never need to touch it.
//
// Markers emitted (the CI greps for these):
//   RAY_TEST_BOOT_OK      — the game booted and loaded its assets
//   RAY_TEST_RENDER_OK    — a frame was read back and actually has content
//   RAY_TEST_RENDER_FAIL  — the frame was blank; something stopped drawing
//   RAY_TEST_DONE_FRAMES  — the frame budget was rendered, exiting cleanly
//
// Why RAY_TEST_RENDER_OK exists: booting proves the window and the assets are
// fine, and nothing more. A regression that leaves the screen empty — a broken
// shader, a lost texture binding, a draw call that silently no-ops — still
// boots, still exits 0, and used to sail straight through CI.
// ---------------------------------------------------------------------------

#ifndef SMOKE_TEST_H
#define SMOKE_TEST_H

#include <raylib.h>   // TraceLog, LOG_INFO, LoadImageFromScreen
#include <rlgl.h>     // rlDrawRenderBatchActive (declaration only)
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

// Which frame to read back. Not frame 0: the first frames of a fresh swap
// chain are legitimately blank on some drivers, and a flaky gate is worse than
// no gate. Short runs fall back to their last frame.
static inline int SmokeTest_CaptureAt(void) {
    return (SmokeTest_maxFrames > 6) ? 5 : (SmokeTest_maxFrames - 1);
}

// ---------------------------------------------------------------------------
// Call once per frame, IMMEDIATELY BEFORE EndDrawing().
//
// The placement is load-bearing, in both directions:
//
//   * Not after EndDrawing(). That call ends with SwapScreenBuffer(), so
//     afterwards you are reading the *new* back buffer, whose contents are
//     undefined. Under llvmpipe you tend to get the previous frame and it
//     looks like it works; on Web it is guaranteed to fail, because the build
//     uses -s ASYNCIFY, EndDrawing() yields to the browser, and WebGL clears
//     the drawing buffer once it has composited — glReadPixels then returns
//     all zeros and this gate would fail a perfectly good build.
//
//   * Not without the explicit batch flush. raylib queues draw calls into a
//     render batch and only submits them inside EndDrawing(). Read before that
//     without flushing and all you see is ClearBackground().
// ---------------------------------------------------------------------------
static inline void SmokeTest_CaptureFrame(void) {
    if (SmokeTest_maxFrames <= 0) return;              // no-op outside CI
    if (SmokeTest_frame != SmokeTest_CaptureAt()) return;

    const int w = GetRenderWidth();
    const int h = GetRenderHeight();
    if (w <= 0 || h <= 0) {
        TraceLog(LOG_WARNING, "RAY_TEST_RENDER_FAIL reason=zero-sized-framebuffer");
        return;
    }

    rlDrawRenderBatchActive();                          // submit queued draws
    Image img = LoadImageFromScreen();                  // RGBA8, top-left origin
    if (img.data == NULL) {
        TraceLog(LOG_WARNING, "RAY_TEST_RENDER_FAIL reason=readback-returned-null");
        return;
    }

    const unsigned char* px = (const unsigned char*)img.data;
    const long total = (long)img.width * (long)img.height;

    // Treat the top-left pixel as the background. Everything the game draws is
    // inset from the corner, so anything differing from it is real content.
    // We deliberately do NOT assert a pixel hash: llvmpipe, ANGLE-on-Metal,
    // SwiftShader and mobile GPUs disagree on text antialiasing and texture
    // filtering, so a hash gate would be red on half the matrix for no reason.
    // The hash is logged as a diagnostic only.
    const unsigned char bg[3] = { px[0], px[1], px[2] };

    long differing = 0;
    unsigned int hash = 2166136261u;                    // FNV-1a
    for (long i = 0; i < total; ++i) {
        const unsigned char* p = px + i * 4;
        const int dr = (int)p[0] - (int)bg[0];
        const int dg = (int)p[1] - (int)bg[1];
        const int db = (int)p[2] - (int)bg[2];
        // +/-2 per channel of slack absorbs dithering and rounding.
        if (dr > 2 || dr < -2 || dg > 2 || dg < -2 || db > 2 || db < -2) ++differing;
        hash = (hash ^ p[0]) * 16777619u;
        hash = (hash ^ p[1]) * 16777619u;
        hash = (hash ^ p[2]) * 16777619u;
    }
    UnloadImage(img);

    const double ratio = (total > 0) ? (double)differing / (double)total : 0.0;

    // Lower bound: something was drawn. Upper bound: the "background" is not
    // itself the anomaly (a fully garbage or fully overdrawn frame reads as
    // ~100% differing and is just as broken as a blank one).
    if (ratio > 0.0005 && ratio < 0.98) {
        TraceLog(LOG_INFO,
                 "RAY_TEST_RENDER_OK frame=%d size=%dx%d pixels=%ld ratio=%.5f hash=%08x",
                 SmokeTest_frame, img.width, img.height, differing, ratio, hash);
    } else {
        TraceLog(LOG_WARNING,
                 "RAY_TEST_RENDER_FAIL frame=%d size=%dx%d pixels=%ld ratio=%.5f hash=%08x "
                 "reason=%s",
                 SmokeTest_frame, img.width, img.height, differing, ratio, hash,
                 (ratio <= 0.0005) ? "blank-frame" : "no-background-visible");
    }
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
