package com.raylib.raymob;  // Don't change the package name (Gradle rewrites it)

import android.app.Activity;

import androidx.annotation.NonNull;

import com.google.android.gms.ads.AdRequest;
import com.google.android.gms.ads.LoadAdError;
import com.google.android.gms.ads.MobileAds;
import com.google.android.gms.ads.OnUserEarnedRewardListener;
import com.google.android.gms.ads.interstitial.InterstitialAd;
import com.google.android.gms.ads.interstitial.InterstitialAdLoadCallback;
import com.google.android.gms.ads.rewarded.RewardItem;
import com.google.android.gms.ads.rewarded.RewardedAd;
import com.google.android.gms.ads.rewarded.RewardedAdLoadCallback;

/**
 * AdMob, the real one. Compiled only when [android.admob] enabled = true in
 * raylib_multiplatform.toml AND "android" is in [targets]; otherwise Gradle
 * compiles the no-op twin in src/noadmob/java instead and the Google Mobile Ads
 * SDK is not a dependency of this app at all.
 *
 * Which is why NativeLoader talks to this class instead of holding the ad code
 * itself: the two versions have to be swappable, and NativeLoader is not.
 *
 * Threading: every field here is written on the UI thread (inside the SDK's
 * load callbacks) and read from the game thread (through JNI). That is what the
 * volatile is for, and leaving it off the two ad handles used to mean the game
 * thread could keep seeing null forever and never show an ad it had loaded.
 */
final class AdmobBridge {

    private static volatile InterstitialAd interstitialAd = null;
    private static volatile RewardedAd rewardedAd = null;
    private static volatile boolean rewardEarned = false;
    private static volatile int rewardAmount = 0;

    private AdmobBridge() {}

    static void initialize(final Activity activity) {
        MobileAds.initialize(activity);   // uses the manifest's APPLICATION_ID
    }

    // ------------------------------------------------------------------
    // Interstitial
    // ------------------------------------------------------------------

    static void requestInterstitial(final Activity activity) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                InterstitialAd.load(activity, BuildConfig.ADMOB_INTERSTITIAL_ID,
                        new AdRequest.Builder().build(),
                        new InterstitialAdLoadCallback() {
                            @Override
                            public void onAdLoaded(@NonNull InterstitialAd ad) {
                                interstitialAd = ad;
                            }
                            @Override
                            public void onAdFailedToLoad(@NonNull LoadAdError loadAdError) {
                                interstitialAd = null;
                            }
                        });
            }
        });
    }

    static boolean isInterstitialLoaded() {
        return interstitialAd != null;
    }

    static void showInterstitial(final Activity activity) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                InterstitialAd ad = interstitialAd;
                if (ad != null) {
                    ad.show(activity);
                    interstitialAd = null;   // consumed: request a new one to show again
                }
            }
        });
    }

    // ------------------------------------------------------------------
    // Rewarded
    // ------------------------------------------------------------------

    static void requestRewarded(final Activity activity) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                RewardedAd.load(activity, BuildConfig.ADMOB_REWARDED_ID,
                        new AdRequest.Builder().build(),
                        new RewardedAdLoadCallback() {
                            @Override
                            public void onAdLoaded(@NonNull RewardedAd ad) {
                                rewardedAd = ad;
                            }
                            @Override
                            public void onAdFailedToLoad(@NonNull LoadAdError loadAdError) {
                                rewardedAd = null;
                            }
                        });
            }
        });
    }

    static boolean isRewardedLoaded() {
        return rewardedAd != null;
    }

    static void showRewarded(final Activity activity) {
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                RewardedAd ad = rewardedAd;
                if (ad != null) {
                    ad.show(activity, new OnUserEarnedRewardListener() {
                        @Override
                        public void onUserEarnedReward(@NonNull RewardItem rewardItem) {
                            rewardAmount = rewardItem.getAmount();
                            rewardEarned = true;   // last: the game polls this one
                        }
                    });
                    rewardedAd = null;   // consumed: request a new one to show again
                }
            }
        });
    }

    // Returns true once per earned reward and clears the flag (poll from the game loop)
    static boolean takeRewardEarned() {
        boolean earned = rewardEarned;
        rewardEarned = false;
        return earned;
    }

    static int getRewardAmount() {
        return rewardAmount;
    }
}
