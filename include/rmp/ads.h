#pragma once
// ---------------------------------------------------------------------------
// rmp::ads — interstitial and rewarded ads.
//
// Real calls on Android, no-ops everywhere else, so ad code compiles on all
// fourteen targets without a single #ifdef. And when [android.admob] enabled =
// false in raylib_multiplatform.toml, the Google Mobile Ads SDK is not in the
// build at all and these still compile and still do nothing.
//
// The chain behind them: thirdparty/raymob/admob.c (JNI) -> NativeLoader.java
// -> AdmobBridge.java. The ad unit ids come from [android.admob].
//
// A wrapper, not a layer: these forward to the C functions in <admob.h>, which
// stay exactly where they are because they are the real JNI boundary and the
// pure-C entry point (examples/main.c) has no namespaces to call into.
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

#include <admob.h>

namespace rmp::ads {

// --- Interstitial ----------------------------------------------------------

// Start preloading one. Do it early; loading takes seconds.
inline void request_interstitial() { ::RequestInterstitialAd(); }

// Has one finished loading?
inline bool is_interstitial_loaded() { return ::IsInterstitialAdLoaded(); }

// Show it. The ad is consumed: request another one to show again.
inline void show_interstitial() { ::ShowInterstitialAd(); }

// --- Rewarded --------------------------------------------------------------

inline void request_rewarded() { ::RequestRewardedAd(); }
inline bool is_rewarded_loaded() { return ::IsRewardedAdLoaded(); }
inline void show_rewarded() { ::ShowRewardedAd(); }

// True once per earned reward, and clears the flag. Poll it from the game
// loop; the amount is then in reward_amount().
inline bool take_reward_earned() { return ::TakeRewardEarned(); }

inline int reward_amount() { return ::GetRewardAmount(); }

} // namespace rmp::ads
