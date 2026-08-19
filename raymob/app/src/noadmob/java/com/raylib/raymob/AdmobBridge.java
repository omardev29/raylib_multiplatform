package com.raylib.raymob;  // Don't change the package name (Gradle rewrites it)

import android.app.Activity;

/**
 * AdMob, switched off. Compiled instead of src/admob/java/AdmobBridge.java when
 * [android.admob] enabled = false in raylib_multiplatform.toml, or when
 * "android" is not in [targets].
 *
 * Nothing here imports the Google Mobile Ads SDK, and with this version in the
 * build the SDK is not a dependency of the app at all: no ~2 MB of library, no
 * AD_ID permission, no initialisation on startup, and nothing to declare in
 * Play's Data safety form about advertising ids.
 *
 * It answers the same calls as the real one — a game that shows ads still
 * compiles and still runs, it just never gets an ad. That is the same contract
 * <admob.h> already has on desktop.
 */
final class AdmobBridge {

    private AdmobBridge() {}

    static void initialize(Activity activity) {}

    static void requestInterstitial(Activity activity) {}
    static boolean isInterstitialLoaded() { return false; }
    static void showInterstitial(Activity activity) {}

    static void requestRewarded(Activity activity) {}
    static boolean isRewardedLoaded() { return false; }
    static void showRewarded(Activity activity) {}
    static boolean takeRewardEarned() { return false; }
    static int getRewardAmount() { return 0; }
}
