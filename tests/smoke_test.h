// ---------------------------------------------------------------------------
// tests/smoke_test.h — CI smoke-test hook for the raylib template.
//
// Lets the headless CI boot the game, render a bounded number of frames and
// exit cleanly (exit code 0) instead of looping until the window is closed.
// Enable it by setting the env var RAY_TEST_MAX_FRAMES=<N> before launching.
// In a normal run the variable is unset and every function here is a no-op, so
// shipped builds behave exactly as if this file did not exist.
//
// Header-only, and intentionally dependency-light, so it compiles on every
// target (desktop, Web, Android, iOS) without wiring extra sources into each
// build system. You normally never need to touch it.
//
// In C++ the two counters below are `inline` variables, so every translation
// unit that includes this header shares one set. That is not a detail: a game
// of any size draws from somewhere other than main.cpp, and with per-TU statics
// SmokeTest_CaptureFrame() would be reading a frame counter that nothing ever
// advanced — the render gate would go quiet and CI would pass on a blank
// screen. In C they stay file-scope statics, which is correct there, because
// the C entry point (examples/main.c) is a single translation unit.
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

#include <raylib.h> // TraceLog, LOG_INFO, LoadImageFromScreen
#include <rlgl.h> // rlDrawRenderBatchActive (declaration only)
#include <stdlib.h> // getenv, atoi

#if defined(__cplusplus)
inline int SmokeTest_frame = 0;
inline int SmokeTest_maxFrames = 0; // 0 = run until the window is closed
#else
static int SmokeTest_frame = 0;
static int SmokeTest_maxFrames = 0;
#endif

// Call once at startup, before entering the game loop. Reads RAY_TEST_MAX_FRAMES.
static inline void SmokeTest_Begin(void) {
    const char *env = getenv("RAY_TEST_MAX_FRAMES");
    if (env && env[0]) SmokeTest_maxFrames = atoi(env);
}

// Call once after startup. Emits the boot marker the CI greps for.
//
// It reports asset *failures*, not successes, and that is the whole point. The
// marker used to carry the dimensions of a texture the game had loaded, and CI
// insisted they were non-zero — which quietly made "ship at least one image"
// a requirement of the template. A game drawing nothing but shapes could not
// pass, and there is no reason it should not.
//
// Counting failures keeps the check that mattered (iOS once shipped a bundle
// with no resources/ in it, and every texture came back 0x0) without inventing
// one nobody asked for: request nothing, fail nothing, pass.
static inline void SmokeTest_ReportBoot(int assetsFailed, int assetsRequested) {
    TraceLog(LOG_INFO,
             "RAY_TEST_BOOT_OK assets_failed=%d assets_requested=%d testFrames=%d",
             assetsFailed, assetsRequested, SmokeTest_maxFrames);
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
    if (SmokeTest_maxFrames <= 0) return; // no-op outside CI
    if (SmokeTest_frame != SmokeTest_CaptureAt()) return;

    const int w = GetRenderWidth();
    const int h = GetRenderHeight();
    if (w <= 0 || h <= 0) {
        TraceLog(LOG_WARNING, "RAY_TEST_RENDER_FAIL reason=zero-sized-framebuffer");
        return;
    }

    rlDrawRenderBatchActive(); // submit queued draws
    Image img = LoadImageFromScreen(); // RGBA8, top-left origin
    if (img.data == NULL) {
        TraceLog(LOG_WARNING, "RAY_TEST_RENDER_FAIL reason=readback-returned-null");
        return;
    }

    const unsigned char *px = (const unsigned char *)img.data;
    const long total = (long)img.width * (long)img.height;

    // "Background" is the most common colour in the frame, found with a
    // histogram — not the corner pixel. The corner is the obvious choice and it
    // is fragile: any border, letterbox or overscan row makes (0,0) a colour
    // that matches nothing else, and the check then reports ~100% content on a
    // frame that is actually blank. The modal colour is whatever
    // ClearBackground painted, regardless of what the edges do.
    //
    // Colours are bucketed to RGB565: that keeps the histogram a flat 64K array
    // and folds in a dithering tolerance for free.
    //
    // We deliberately do NOT assert a pixel hash. llvmpipe, ANGLE-on-Metal,
    // SwiftShader and mobile GPUs disagree on text antialiasing and texture
    // filtering, so a hash gate would be red on half the matrix for no reason.
    // The hash is logged as a diagnostic only.
    unsigned int *hist = (unsigned int *)calloc(65536, sizeof(unsigned int));
    if (hist == NULL) {
        UnloadImage(img);
        return;
    }

    unsigned int hash = 2166136261u; // FNV-1a
    for (long i = 0; i < total; ++i) {
        const unsigned char *p = px + i * 4;
        hist[((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3)]++;
        hash = (hash ^ p[0]) * 16777619u;
        hash = (hash ^ p[1]) * 16777619u;
        hash = (hash ^ p[2]) * 16777619u;
    }

    int bg = 0;
    for (int k = 1; k < 65536; ++k)
        if (hist[k] > hist[bg]) bg = k;
    const long differing = total - (long)hist[bg];
    free(hist);
    UnloadImage(img);

    const double ratio = (total > 0) ? (double)differing / (double)total : 0.0;

    // Lower bound: something was drawn. Upper bound: the "background" is not
    // itself the anomaly (a fully garbage or fully overdrawn frame reads as
    // ~100% differing and is just as broken as a blank one).
    if (ratio > 0.0005 && ratio < 0.98) {
        TraceLog(LOG_INFO,
                 "RAY_TEST_RENDER_OK frame=%d Size=%dx%d pixels=%ld ratio=%.5f hash=%08x",
                 SmokeTest_frame, img.width, img.height, differing, ratio, hash);
    } else {
        TraceLog(
            LOG_WARNING,
            "RAY_TEST_RENDER_FAIL frame=%d Size=%dx%d pixels=%ld ratio=%.5f hash=%08x "
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
