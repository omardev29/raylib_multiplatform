// ---------------------------------------------------------------------------
// examples/ads/02_rewarded.cpp
//
// A rewarded ad, end to end: the player asks for a bonus, watches an ad, and
// gets the reward — only if they actually finished it.
//
// This is the flow worth getting right, because it is the one where a mistake
// costs you real money or a real player's trust. An interstitial you can fire
// and forget (see 01_interstitial.cpp); a rewarded ad has a contract with the
// player attached to it.
//
// Real on Android, silently nothing everywhere else. On desktop the buttons
// below are live and the reward never arrives, which is exactly what you want
// while building the rest of the game.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <rmp/ads.h>
#include <rmp/app.h>
#include <rmp/ui.h>

#include <string>

static int coins = 0;
static bool waitingForAd = false;

static void on_ready() {
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);

    // Preload as early as you can. Loading takes seconds, and an ad the player
    // has to wait for is an ad they close.
    rmp::ads::request_rewarded();
}

static void on_frame(float delta) {
    // THE POLL. take_reward_earned() is true exactly once per reward earned and
    // clears itself, so this can live in the frame loop with no bookkeeping. It
    // fires only if the player watched enough of the ad — if they closed it
    // early, nothing happens here and they get nothing, which is the whole
    // point of a rewarded ad.
    if (rmp::ads::take_reward_earned()) {
        coins += rmp::ads::reward_amount();
        waitingForAd = false;

        // Line the next one up straight away, so the button is live again by
        // the time the player wants it.
        rmp::ads::request_rewarded();
    }

    BeginDrawing();
    ClearBackground(rmp::ui::current_theme().background);
    rmp::ui::begin();

    rmp::ui::panel([&] {
        rmp::ui::text("Coins: " + std::to_string(coins));

        if (rmp::ads::is_rewarded_loaded()) {
            if (rmp::ui::button("Watch an ad for 50 coins",
                                { .style = rmp::ui::Variant::PRIMARY })) {
                waitingForAd = true;
                rmp::ads::show_rewarded();
            }
        } else {
            // Never show a live button for an ad that is not there: the player
            // taps it, nothing happens, and they decide your game is broken.
            // A disabled control says "not yet" honestly.
            rmp::ui::button("Ad not ready", { .enabled = false });
        }

        if (waitingForAd) {
            rmp::ui::text("waiting for the reward…",
                          { .color = rmp::ui::ColorRole::MUTED, .size = 14 });
        }

        rmp::ui::text("Ads only exist on Android. Everywhere else the calls do "
                      "nothing and the button above stays disabled.",
                      { .color = rmp::ui::ColorRole::MUTED, .size = 14 });
    });

    rmp::ui::end();
    EndDrawing();
}

static void on_exit() { CloseWindow(); }

RMP_ENTRY_POINT(on_ready, on_frame, on_exit);

// ---------------------------------------------------------------------------
// Before you ship this
// ---------------------------------------------------------------------------
//
// * The ids in [android.admob] are Google's TEST ids. Replace them, or you will
//   be showing test ads to real players. Clicking real ads from your own build
//   is the fastest way to get an AdMob account suspended, so keep the test ids
//   until the day you publish.
//
// * CONSENT IS NOT IMPLEMENTED. Serving ads in the EEA or the UK requires a
//   Google-certified consent platform, and this template does not ship one.
//   See the warning in README.md.
//
// * Do not gate progress behind an ad that may never load. is_rewarded_loaded()
//   can be false for a long time on a bad connection, and a player who cannot
//   continue is a player who uninstalls.
//
// * [android.admob] enabled = false removes AdMob from the build entirely, and
//   this file still compiles and still runs — the calls become the no-ops they
//   already are on desktop.
