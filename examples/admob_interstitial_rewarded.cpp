// ---------------------------------------------------------------------------
// examples/admob_interstitial_rewarded.cpp
//
// How to add AdMob ads (interstitial + rewarded) with the template.
//
// The API lives in <admob.h> (thirdparty/raymob). It is CROSS-PLATFORM: on
// Android it calls the real Google Mobile Ads SDK via JNI; on every other
// platform the functions are no-op stubs. So you write ONE code path with no
// #ifdefs, and ads simply only appear on Android.
//
// Lifecycle of an ad:
//   Request*Ad()      -> start preloading (do it early, e.g. in _ready)
//   Is*AdLoaded()     -> has it finished loading?
//   Show*Ad()         -> display it (then immediately request the next one)
//   TakeRewardEarned()/GetRewardAmount() -> rewarded only: poll the reward
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

#include <raylib.h>
#include <admob.h>

static int lastReward = 0;

static void _ready() {
    InitWindow(800, 450, "admob example");

    // Preload both ad types as early as possible so they are ready when you
    // want to show them. No-ops outside Android.
    RequestInterstitialAd();
    RequestRewardedAd();
}

static void _process() {
    // Show an interstitial when it is loaded (here: on SPACE).
    if (IsKeyPressed(KEY_SPACE) && IsInterstitialAdLoaded()) {
        ShowInterstitialAd();
        RequestInterstitialAd(); // preload the next one right away
    }

    // Show a rewarded ad (here: on R) and poll for the reward.
    if (IsKeyPressed(KEY_R) && IsRewardedAdLoaded()) {
        ShowRewardedAd();
        RequestRewardedAd();
    }
    if (TakeRewardEarned()) {           // true exactly once per earned reward
        lastReward = GetRewardAmount(); // grant it to the player here
    }

    BeginDrawing();
    ClearBackground(RAYWHITE);
    DrawText("SPACE: interstitial   R: rewarded", 10, 40, 20, DARKGRAY);
    DrawText(IsInterstitialAdLoaded() ? "interstitial ready" : "interstitial loading...", 10, 80, 20, GRAY);
    DrawText(IsRewardedAdLoaded() ? "rewarded ready" : "rewarded loading...", 10, 110, 20, GRAY);
    if (lastReward > 0) DrawText(TextFormat("Last reward: %d", lastReward), 10, 150, 20, DARKGREEN);
    EndDrawing();
}

static void _exit() { CloseWindow(); }

int main() {
    _ready();
    while (!WindowShouldClose()) _process();
    _exit();
    return 0;
}
