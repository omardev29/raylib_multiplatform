/*
 *  AdMob bridge for raymob — interstitial + rewarded ads.
 *
 *  On Android these call into NativeLoader.java via JNI (see admob.c). On every
 *  other platform they compile to no-op stubs, so the same game code builds and
 *  runs anywhere without #ifdefs. Banner ads are intentionally not supported.
 *
 *  Typical usage from the game:
 *      RequestInterstitialAd();              // preload (e.g. in _ready)
 *      RequestRewardedAd();
 *      ...
 *      if (IsRewardedAdLoaded()) ShowRewardedAd();
 *      if (TakeRewardEarned()) { int amount = GetRewardAmount(); grant(amount); }
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__ANDROID__)

/* Interstitial */
void RequestInterstitialAd(void);
bool IsInterstitialAdLoaded(void);
void ShowInterstitialAd(void);

/* Rewarded */
void RequestRewardedAd(void);
bool IsRewardedAdLoaded(void);
void ShowRewardedAd(void);
/* Returns true once per earned reward and clears the flag (poll from the game
 * loop); the reward amount is then available via GetRewardAmount(). */
bool TakeRewardEarned(void);
int  GetRewardAmount(void);

#else

/* No-op stubs for non-Android platforms */
static inline void RequestInterstitialAd(void) {}
static inline bool IsInterstitialAdLoaded(void) { return false; }
static inline void ShowInterstitialAd(void) {}
static inline void RequestRewardedAd(void) {}
static inline bool IsRewardedAdLoaded(void) { return false; }
static inline void ShowRewardedAd(void) {}
static inline bool TakeRewardEarned(void) { return false; }
static inline int  GetRewardAmount(void) { return 0; }

#endif

#ifdef __cplusplus
}
#endif
