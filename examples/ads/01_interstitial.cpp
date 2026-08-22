// ---------------------------------------------------------------------------
// examples/ads/01_interstitial.cpp
//
// Interstitial ads: the full-screen ones you show between levels.
//
// The rewarded flow, which is the one with a contract attached, is in
// 02_rewarded.cpp.
//
// The API is rmp::ads, from <rmp/ads.h>. It is CROSS-PLATFORM: on
// Android it calls the real Google Mobile Ads SDK via JNI; on every other
// platform the functions are no-op stubs. So you write ONE code path with no
// #ifdefs, and ads simply only appear on Android.
//
// Lifecycle of an ad:
//   request_*()          -> start preloading (do it early, e.g. in on_ready)
//   is_*_loaded()        -> has it finished loading?
//   show_*()             -> display it, then request the next one
//   take_reward_earned() -> rewarded only: true once, then reward_amount()
//
// Configure your real ad-unit ids in [android.admob] in
// raylib_multiplatform.toml before shipping (the template defaults to Google's
// official TEST ids). Banner ads are not supported by design.
//
// [android.admob] enabled = false removes AdMob from the Android build
// entirely, and this file still compiles and still runs: the calls become the
// same no-ops they already are on desktop. Ads are opt-in for a reason — see
// the consent (UMP) warning in README.md before shipping with them on.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <raylib.h> // this example writes its own main(), so it needs raylib
#include <rmp/ads.h> // rmp::ads, and nothing else from the framework

static int lastReward = 0;

static void on_ready() {
    InitWindow(800, 450, "admob example");

    // Preload both ad types as early as possible so they are ready when you
    // want to show them. No-ops outside Android.
    rmp::ads::request_interstitial();
    rmp::ads::request_rewarded();
}

static void on_frame() {
    // Show an interstitial when it is loaded (here: on SPACE).
    if (IsKeyPressed(KEY_SPACE) && rmp::ads::is_interstitial_loaded()) {
        rmp::ads::show_interstitial();
        rmp::ads::request_interstitial(); // preload the next one right away
    }

    // Show a rewarded ad (here: on R) and poll for the reward.
    if (IsKeyPressed(KEY_R) && rmp::ads::is_rewarded_loaded()) {
        rmp::ads::show_rewarded();
        rmp::ads::request_rewarded();
    }
    if (rmp::ads::take_reward_earned()) { // true exactly once per earned reward
        lastReward = rmp::ads::reward_amount(); // grant it to the player here
    }

    BeginDrawing();
    ClearBackground(RAYWHITE);
    DrawText("SPACE: interstitial   R: rewarded", 10, 40, 20, DARKGRAY);
    DrawText(rmp::ads::is_interstitial_loaded() ? "interstitial ready"
                                                : "interstitial loading...",
             10, 80, 20, GRAY);
    DrawText(rmp::ads::is_rewarded_loaded() ? "rewarded ready" : "rewarded loading...",
             10, 110, 20, GRAY);
    if (lastReward > 0)
        DrawText(TextFormat("Last reward: %d", lastReward), 10, 150, 20, DARKGREEN);
    EndDrawing();
}

static void on_exit() { CloseWindow(); }

int main() {
    on_ready();
    while (!WindowShouldClose()) on_frame();
    on_exit();
    return 0;
}
