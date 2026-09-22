/*
 *  raymob License (MIT)
 *
 *  Copyright (c) 2023-2024 Le Juez Victor
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 */

#include "raymob.h"

#ifdef __ANDROID__

void KeepScreenOn(bool keepOn)
{
    jobject nativeLoaderInst = GetNativeLoaderInstance();

    if (nativeLoaderInst != NULL) {
        JNIEnv* env = AttachCurrentThread();

        // [rmp patch] through the NULL-safe helpers: the field and the method
        // are looked up by name, and R8 renames both in the release AAB unless
        // proguard-rules.pro keeps them
        jobject displayManager = RaymobGetObjectField(env, nativeLoaderInst, "displayManager",
                                                      "Lcom/raylib/raymob/DisplayManager;");

        if (displayManager != NULL) {
            jmethodID method = RaymobGetMethod(env, displayManager, "keepScreenOn", "(Z)V");

            if (method != NULL) {
                (*env)->CallVoidMethod(env, displayManager, method, (jboolean)keepOn);
                RaymobExceptionCheck(env, "keepScreenOn");
            }
        }

        DetachCurrentThread();
    }
}

Orientation GetScreenOrientation()
{
    Orientation result = 0;
    jobject nativeLoaderInst = GetNativeLoaderInstance();

    if (nativeLoaderInst != NULL) {
        JNIEnv* env = AttachCurrentThread();

        jobject displayManager = RaymobGetObjectField(env, nativeLoaderInst, "displayManager",
                                                      "Lcom/raylib/raymob/DisplayManager;");

        if (displayManager != NULL) {
            jmethodID screenOrientationMethod = RaymobGetMethod(env, displayManager, "getOrientation", "()I");

            if (screenOrientationMethod != NULL) {
                jint screenOrientation = (*env)->CallIntMethod(env, displayManager, screenOrientationMethod);

                // [rmp patch] upstream tested `result`, which is still 0 here,
                // so `0 >= 0 && 0 < 4` was always true and the bound check
                // never looked at the value it was written to guard.
                // DisplayManager.getOrientation() returns -1 when there is no
                // Display, and that is exactly what must not reach the game.
                if (!RaymobExceptionCheck(env, "getOrientation") &&
                    screenOrientation >= 0 && screenOrientation < 4) {
                    result = (Orientation)screenOrientation;
                }
            }
        }

        DetachCurrentThread();
    }

    return result;
}

#endif
