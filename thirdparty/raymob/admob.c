/*
 *  AdMob bridge for raymob — JNI implementation (Android only).
 *
 *  Calls the Java methods defined in NativeLoader.java. Method names and
 *  signatures must match exactly. On non-Android builds this file is empty and
 *  the header provides no-op stubs.
 */

#include "admob.h"

#ifdef __ANDROID__

#include "raymob.h"   /* GetNativeLoaderInstance / AttachCurrentThread / DetachCurrentThread */

/* Look up a method on the NativeLoader instance, leaving the JNIEnv clean.
 *
 * GetMethodID does not merely return NULL when the method is missing: it also
 * leaves a NoSuchMethodError pending on the thread. Ignoring that used to be
 * the difference between "the ad call did nothing" and the VM aborting on the
 * next JNI call it made, which is a crash with no visible cause. And a missing
 * method is a real case now, not a hypothetical one: with AdMob switched off
 * the eight methods still exist, but anyone swapping in their own Activity
 * lands exactly here.
 *
 * That is RaymobGetMethod's whole job now (thirdparty/raymob/helper.c): it
 * deletes the class reference rather than leaving it to the detach that
 * follows, so this stays correct on an already-attached thread, and it logs
 * the name it could not find instead of failing silently.
 *
 * The RETURN of each call needs the same treatment, which it did not have: the
 * Google Mobile Ads SDK throws (MobileAds never initialised, a malformed ad
 * unit id) and a pending exception aborts the VM at the next JNI call from
 * ANY raymob file -- reported as a crash in the display code.
 */
/* Call a no-arg void method on the NativeLoader instance */
static void AdmobCallVoid(const char *name, const char *sig) {
    jobject inst = GetNativeLoaderInstance();
    if (inst == NULL) return;

    JNIEnv *env = AttachCurrentThread();
    jmethodID method = RaymobGetMethod(env, inst, name, sig);
    if (method != NULL) {
        (*env)->CallVoidMethod(env, inst, method);
        RaymobExceptionCheck(env, name);
    }
    DetachCurrentThread();
}

/* Call a no-arg boolean method on the NativeLoader instance */
static bool AdmobCallBool(const char *name, const char *sig) {
    jobject inst = GetNativeLoaderInstance();
    if (inst == NULL) return false;

    JNIEnv *env = AttachCurrentThread();
    jmethodID method = RaymobGetMethod(env, inst, name, sig);
    bool result = false;
    if (method != NULL) {
        result = (bool)(*env)->CallBooleanMethod(env, inst, method);
        if (RaymobExceptionCheck(env, name)) result = false;
    }
    DetachCurrentThread();
    return result;
}

/* Call a no-arg int method on the NativeLoader instance */
static int AdmobCallInt(const char *name, const char *sig) {
    jobject inst = GetNativeLoaderInstance();
    if (inst == NULL) return 0;

    JNIEnv *env = AttachCurrentThread();
    jmethodID method = RaymobGetMethod(env, inst, name, sig);
    int result = 0;
    if (method != NULL) {
        result = (int)(*env)->CallIntMethod(env, inst, method);
        if (RaymobExceptionCheck(env, name)) result = 0;
    }
    DetachCurrentThread();
    return result;
}

/* Interstitial */
void RequestInterstitialAd(void)   { AdmobCallVoid("requestInterstitialAd", "()V"); }
bool IsInterstitialAdLoaded(void)  { return AdmobCallBool("isInterstitialAdLoaded", "()Z"); }
void ShowInterstitialAd(void)      { AdmobCallVoid("showInterstitialAd", "()V"); }

/* Rewarded */
void RequestRewardedAd(void)       { AdmobCallVoid("requestRewardedAd", "()V"); }
bool IsRewardedAdLoaded(void)      { return AdmobCallBool("isRewardedAdLoaded", "()Z"); }
void ShowRewardedAd(void)          { AdmobCallVoid("showRewardedAd", "()V"); }
bool TakeRewardEarned(void)        { return AdmobCallBool("takeRewardEarned", "()Z"); }
int  GetRewardAmount(void)         { return AdmobCallInt("getRewardAmount", "()I"); }

#endif /* __ANDROID__ */
