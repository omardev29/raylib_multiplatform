/*
 *  raymob License (MIT)
 *
 *  Copyright (c) 2023-2024 Le Juez Victor
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "raymob.h"

#ifdef __ANDROID__

void Vibrate(float seconds) { VibrateMS((uint64_t)(1000 * seconds)); }

void VibrateMS(uint64_t ms) {
  jobject nativeLoaderInst = GetNativeLoaderInstance();
  if (nativeLoaderInst == NULL) return;

  JNIEnv *env = AttachCurrentThread();
  if (env == NULL) return;

  jmethodID getSystemServiceMethod =
      RaymobGetMethod(env, nativeLoaderInst, "getSystemService",
                      "(Ljava/lang/String;)Ljava/lang/Object;");

  if (getSystemServiceMethod == NULL) {
    DetachCurrentThread();
    return;
  }

  jstring vibratorService = (*env)->NewStringUTF(env, "vibrator");
  jobject vibrator = (*env)->CallObjectMethod(
      env, nativeLoaderInst, getSystemServiceMethod, vibratorService);
  (*env)->DeleteLocalRef(env, vibratorService);

  // NOTE: a device with no vibrator service hands back null
  if (RaymobExceptionCheck(env, "getSystemService") || vibrator == NULL) {
    DetachCurrentThread();
    return;
  }

  jmethodID hasVibratorMethod = RaymobGetMethod(env, vibrator, "hasVibrator", "()Z");

  if (hasVibratorMethod == NULL) {
    DetachCurrentThread();
    return;
  }

  jboolean hasVibrator =
      (*env)->CallBooleanMethod(env, vibrator, hasVibratorMethod);

  if (RaymobExceptionCheck(env, "hasVibrator")) hasVibrator = JNI_FALSE;

  if (hasVibrator) {
    jmethodID vibrateMethod = RaymobGetMethod(env, vibrator, "vibrate", "(J)V");

    if (vibrateMethod != NULL) {
      (*env)->CallVoidMethod(env, vibrator, vibrateMethod, (jlong)ms);
      RaymobExceptionCheck(env, "vibrate(J)V");
    }
  }

  DetachCurrentThread();
}

void VibrateEx(float seconds, float intensity) {
  VibrateExMS((uint64_t)(1000 * seconds), intensity);
}

void VibrateExMS(uint64_t ms, float intensity) {
  jobject nativeLoaderInst = GetNativeLoaderInstance();
  if (nativeLoaderInst == NULL) return;

  JNIEnv *env = AttachCurrentThread();
  if (env == NULL) return;

  jmethodID getSystemServiceMethod =
      RaymobGetMethod(env, nativeLoaderInst, "getSystemService",
                      "(Ljava/lang/String;)Ljava/lang/Object;");

  if (getSystemServiceMethod == NULL) {
    DetachCurrentThread();
    return;
  }

  jstring vibratorService = (*env)->NewStringUTF(env, "vibrator");
  jobject vibrator = (*env)->CallObjectMethod(
      env, nativeLoaderInst, getSystemServiceMethod, vibratorService);
  (*env)->DeleteLocalRef(env, vibratorService);

  // NOTE: a device with no vibrator service hands back null
  if (RaymobExceptionCheck(env, "getSystemService") || vibrator == NULL) {
    DetachCurrentThread();
    return;
  }

  jmethodID hasVibratorMethod = RaymobGetMethod(env, vibrator, "hasVibrator", "()Z");

  if (hasVibratorMethod == NULL) {
    DetachCurrentThread();
    return;
  }

  jboolean hasVibrator =
      (*env)->CallBooleanMethod(env, vibrator, hasVibratorMethod);

  if (RaymobExceptionCheck(env, "hasVibrator")) hasVibrator = JNI_FALSE;

  if (hasVibrator) {
    jclass vibrationEffectClass =
        (*env)->FindClass(env, "android/os/VibrationEffect");

    if (vibrationEffectClass == NULL) {
      RaymobExceptionCheck(env, "FindClass(VibrationEffect)");
      DetachCurrentThread();
      return;
    }

    jmethodID createOneShotMethod =
        (*env)->GetStaticMethodID(env, vibrationEffectClass, "createOneShot",
                                  "(JI)Landroid/os/VibrationEffect;");

    if (createOneShotMethod == NULL) {
      RaymobExceptionCheck(env, "createOneShot");
      (*env)->DeleteLocalRef(env, vibrationEffectClass);
      DetachCurrentThread();
      return;
    }

    int intensityValue = (int)(intensity * 255);
    if (intensityValue > 255)
      intensityValue = 255;
    if (intensityValue < 1)
      intensityValue = 1;

    jobject vibrationEffect = (*env)->CallStaticObjectMethod(
        env, vibrationEffectClass, createOneShotMethod, (jlong)ms,
        (jint)intensityValue);
    RaymobExceptionCheck(env, "createOneShot");
    (*env)->DeleteLocalRef(env, vibrationEffectClass);

    if (vibrationEffect != NULL) {
      jmethodID vibrateMethod = RaymobGetMethod(
          env, vibrator, "vibrate", "(Landroid/os/VibrationEffect;)V");

      if (vibrateMethod != NULL) {
        (*env)->CallVoidMethod(env, vibrator, vibrateMethod, vibrationEffect);
        RaymobExceptionCheck(env, "vibrate(VibrationEffect)V");
      }
    }
  }

  DetachCurrentThread();
}

#endif
