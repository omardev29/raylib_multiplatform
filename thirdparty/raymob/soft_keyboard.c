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

#include <string.h>

void ShowSoftKeyboard(void) {
  jobject context = GetNativeLoaderInstance();

  if (context != NULL) {
    JNIEnv *env = AttachCurrentThread();
    jobject softKeyboard = RaymobGetObjectField(
        env, context, "softKeyboard", "Lcom/raylib/raymob/SoftKeyboard;");

    if (softKeyboard != NULL) {
      jmethodID method = RaymobGetMethod(env, softKeyboard, "showKeyboard", "()V");

      if (method != NULL) {
        (*env)->CallVoidMethod(env, softKeyboard, method);
        RaymobExceptionCheck(env, "showKeyboard");
      }
    }

    DetachCurrentThread();
  }
}

void HideSoftKeyboard(void) {
  jobject context = GetNativeLoaderInstance();

  if (context != NULL) {
    JNIEnv *env = AttachCurrentThread();
    jobject softKeyboard = RaymobGetObjectField(
        env, context, "softKeyboard", "Lcom/raylib/raymob/SoftKeyboard;");

    if (softKeyboard != NULL) {
      jmethodID method = RaymobGetMethod(env, softKeyboard, "hideKeyboard", "()V");

      if (method != NULL) {
        (*env)->CallVoidMethod(env, softKeyboard, method);
        RaymobExceptionCheck(env, "hideKeyboard");
      }
    }

    DetachCurrentThread();
  }
}

int GetLastSoftKeyCode(void) {
  jobject context = GetNativeLoaderInstance();

  if (context != NULL) {
    JNIEnv *env = AttachCurrentThread();
    jobject softKeyboard = RaymobGetObjectField(
        env, context, "softKeyboard", "Lcom/raylib/raymob/SoftKeyboard;");

    if (softKeyboard != NULL) {
      jmethodID method = RaymobGetMethod(env, softKeyboard, "getLastKeyCode", "()I");
      int value = 0;

      if (method != NULL) {
        value = (*env)->CallIntMethod(env, softKeyboard, method);
        if (RaymobExceptionCheck(env, "getLastKeyCode")) value = 0;
      }

      DetachCurrentThread();
      return value;
    }

    DetachCurrentThread();
  }

  return 0;
}

unsigned short GetLastSoftKeyLabel(void) {
  jobject context = GetNativeLoaderInstance();

  if (context != NULL) {
    JNIEnv *env = AttachCurrentThread();
    jobject softKeyboard = RaymobGetObjectField(
        env, context, "softKeyboard", "Lcom/raylib/raymob/SoftKeyboard;");

    if (softKeyboard != NULL) {
      jmethodID method = RaymobGetMethod(env, softKeyboard, "getLastKeyLabel", "()C");
      unsigned short value = 0;

      if (method != NULL) {
        value = (*env)->CallCharMethod(env, softKeyboard, method);
        if (RaymobExceptionCheck(env, "getLastKeyLabel")) value = 0;
      }

      DetachCurrentThread();
      return value;
    }

    DetachCurrentThread();  // [rmp patch] the early return skipped this
  }

  return 0;
}

int GetLastSoftKeyUnicode(void) {
  jobject context = GetNativeLoaderInstance();

  if (context != NULL) {
    JNIEnv *env = AttachCurrentThread();
    jobject softKeyboard = RaymobGetObjectField(
        env, context, "softKeyboard", "Lcom/raylib/raymob/SoftKeyboard;");

    if (softKeyboard != NULL) {
      jmethodID method = RaymobGetMethod(env, softKeyboard, "getLastKeyUnicode", "()I");
      int value = 0;

      if (method != NULL) {
        value = (*env)->CallIntMethod(env, softKeyboard, method);
        if (RaymobExceptionCheck(env, "getLastKeyUnicode")) value = 0;
      }

      DetachCurrentThread();
      return value;
    }

    DetachCurrentThread();  // [rmp patch] the early return skipped this
  }

  return 0;
}

char GetLastSoftKeyChar(void) {
#define KEYCODE_ENTER 66
#define KEYCODE_DEL 67

  jobject context = GetNativeLoaderInstance();

  if (context != NULL) {
    char value = '\0';

    JNIEnv *env = AttachCurrentThread();
    jobject softKeyboard = RaymobGetObjectField(
        env, context, "softKeyboard", "Lcom/raylib/raymob/SoftKeyboard;");

    if (softKeyboard != NULL) {
      jmethodID methodKeyCode =
          RaymobGetMethod(env, softKeyboard, "getLastKeyCode", "()I");
      int keyCode = 0;

      if (methodKeyCode != NULL) {
        keyCode = (*env)->CallIntMethod(env, softKeyboard, methodKeyCode);
        if (RaymobExceptionCheck(env, "getLastKeyCode")) keyCode = 0;
      }

      if (keyCode != 0) {
        switch (keyCode) {
        case KEYCODE_ENTER: {
          value = '\n';
        } break;
        case KEYCODE_DEL: {
          value = '\b';
        } break;
        default: {
          jmethodID methodKeyUnicode =
              RaymobGetMethod(env, softKeyboard, "getLastKeyUnicode", "()I");
          int u = 0;

          if (methodKeyUnicode != NULL) {
            u = (*env)->CallIntMethod(env, softKeyboard, methodKeyUnicode);
            if (RaymobExceptionCheck(env, "getLastKeyUnicode")) u = 0;
          }

          if (u > 0xFF)
            value = '?';
          else
            value = (char)u;
        }
        }
      }
    }

    DetachCurrentThread();

    return value;
  }

  return '\0';
}

void ClearLastSoftKey(void) {
  jobject context = GetNativeLoaderInstance();

  if (context != NULL) {
    JNIEnv *env = AttachCurrentThread();
    jobject softKeyboard = RaymobGetObjectField(
        env, context, "softKeyboard", "Lcom/raylib/raymob/SoftKeyboard;");

    if (softKeyboard != NULL) {
      jmethodID method = RaymobGetMethod(env, softKeyboard, "clearLastKeyEvent", "()V");

      if (method != NULL) {
        (*env)->CallVoidMethod(env, softKeyboard, method);
        RaymobExceptionCheck(env, "clearLastKeyEvent");
      }
    }

    DetachCurrentThread();
  }
}

void SoftKeyboardEditText(char *text, unsigned int size) {
  char c = GetLastSoftKeyChar();
  if (c == '\0')
    return;

  unsigned int len = strlen(text);

  if (c == '\b' && len > 0) {
    text[len - 1] = '\0';
  } else if (c != '\b' && len < size) {
    text[len++] = c;
    text[len] = '\0';
  }

  ClearLastSoftKey();
}

#endif
