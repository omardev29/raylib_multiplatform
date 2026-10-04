#pragma once
// ---------------------------------------------------------------------------
// rmp::ads — interstitial and rewarded ads.
//
// Real calls on Android, no-ops everywhere else, so ad code compiles on every
// target without a single #ifdef. And when [android.admob] enabled =
// false in raylib_multiplatform.toml, the Google Mobile Ads SDK is not in the
// build at all and these still compile and still do nothing.
//
// The chain behind them: thirdparty/raymob/admob.c (JNI) -> NativeLoader.java
// -> AdmobBridge.java. The ad unit ids come from [android.admob].
//
// A wrapper, not a layer: src/rmp/ads.cpp forwards these to the C functions in
// <admob.h>, and that header stays out of yours. Those stay C because they are
// the real JNI boundary, and because a game in plain C -- the shape of
// examples/plain_c/src/main.c -- has no namespace to call into and can call
// them directly: #include <admob.h>, which is on a plain C game's include
// path on every target and compiles to no-ops off Android, as these do.
// examples/ads/01_interstitial and examples/ads/02_rewarded are both kinds of
// ad, end to end.
//
// Typical use:
//
//     rmp::ads::request_rewarded();               // preload, e.g. in on_ready()
//     ...
//     if (rmp::ads::is_rewarded_loaded()) rmp::ads::show_rewarded();
//     if (rmp::ads::take_reward_earned()) grant(rmp::ads::reward_amount());
//
// Before shipping with ads on, read the consent (UMP) warning in README.md.
// ---------------------------------------------------------------------------

#include <rmp/config.h>

namespace rmp::ads {

// --- Interstitial ----------------------------------------------------------

// Start preloading one. Do it early; loading takes seconds.
void request_interstitial();

// Has one finished loading?
bool is_interstitial_loaded();

// Show it. The ad is consumed: request another one to show again.
void show_interstitial();

// --- Rewarded --------------------------------------------------------------

// Start preloading a rewarded ad, from [android.admob] rewarded_id. Do it early,
// and again after each one is shown: loading takes seconds.
void request_rewarded();

// Has one finished loading? A button that shows it should be live only then.
bool is_rewarded_loaded();

// Show it. The ad is consumed: request another one to show again. Nothing
// happens when none has loaded, and the reward is not here: it arrives through
// take_reward_earned(), and only if the player watched enough of the ad.
void show_rewarded();

// True once per earned reward, and clears the flag. Poll it from the game
// loop; the amount is then in reward_amount().
bool take_reward_earned();

// The amount of the last reward earned, as set on the ad unit in AdMob. 0 until
// one is earned, and always 0 off Android.
int reward_amount();

} // namespace rmp::ads
