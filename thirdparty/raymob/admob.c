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

/* Call a no-arg void method on the NativeLoader instance */
static void AdmobCallVoid(const char *name, const char *sig) {
    jobject inst = GetNativeLoaderInstance();
    if (inst == NULL) return;

    JNIEnv *env = AttachCurrentThread();
    jclass cls = (*env)->GetObjectClass(env, inst);
    jmethodID method = (*env)->GetMethodID(env, cls, name, sig);
    if (method != NULL) {
        (*env)->CallVoidMethod(env, inst, method);
    }
    DetachCurrentThread();
}

/* Call a no-arg boolean method on the NativeLoader instance */
static bool AdmobCallBool(const char *name, const char *sig) {
    jobject inst = GetNativeLoaderInstance();
    if (inst == NULL) return false;

    JNIEnv *env = AttachCurrentThread();
    jclass cls = (*env)->GetObjectClass(env, inst);
    jmethodID method = (*env)->GetMethodID(env, cls, name, sig);
    bool result = false;
    if (method != NULL) {
        result = (bool)(*env)->CallBooleanMethod(env, inst, method);
    }
    DetachCurrentThread();
    return result;
}

/* Call a no-arg int method on the NativeLoader instance */
static int AdmobCallInt(const char *name, const char *sig) {
    jobject inst = GetNativeLoaderInstance();
    if (inst == NULL) return 0;

    JNIEnv *env = AttachCurrentThread();
    jclass cls = (*env)->GetObjectClass(env, inst);
    jmethodID method = (*env)->GetMethodID(env, cls, name, sig);
    int result = 0;
    if (method != NULL) {
        result = (int)(*env)->CallIntMethod(env, inst, method);
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
