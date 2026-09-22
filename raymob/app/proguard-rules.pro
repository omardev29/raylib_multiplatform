# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
#
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# If your project uses WebView with JS, uncomment the following
# and specify the fully qualified class name to the JavaScript interface
# class:
#-keepclassmembers class fqcn.of.javascript.interface.for.webview {
#   public *;
#}

# Uncomment this to preserve the line number information for
# debugging stack traces.
#-keepattributes SourceFile,LineNumberTable

# If you keep the line number information, uncomment this to
# hide the original source file name.
#-renamesourcefileattribute SourceFile

# Keep everything the native side reaches BY NAME.
#
# The release build is minified (minifyEnabled true, build.gradle), and R8
# renames whatever it is not told to keep. A JNI lookup is a string, so a
# renamed member is not a link error at build time -- it is GetFieldID
# returning NULL at runtime with a NoSuchFieldError pending, and ART killing
# the process at the next JNI call with `JNI DETECTED ERROR IN APPLICATION:
# jfieldID was NULL`. Nothing in CI sees it either: the smoke test boots
# assembleDebug, which is not minified.
#
# `public <methods>` on NativeLoader alone, which is what this file used to
# say, kept the eight AdMob methods and nothing else. It did NOT keep the
# three public FIELDS the C looks up -- `initCallback` (callback.c),
# `displayManager` (display.c) and `softKeyboard` (soft_keyboard.c) -- and it
# did not keep DisplayManager or SoftKeyboard at all, nor their
# keepScreenOn/getOrientation/showKeyboard/hideKeyboard/getLastKey*.
#
# The whole package is four classes. Keeping all of their members costs a few
# kilobytes and removes an entire class of release-only crash, so the rule is
# the package rather than a list that has to be kept in step with the C by
# hand. tests/configure_test.py checks that every com/raylib/raymob class the
# JNI code names, and every class in the package, is covered by a keep-all
# rule here -- a new JNI lookup without one fails the test instead of failing
# on a user's device.
#
# NOTE: build.gradle rewrites `com.raylib.raymob` in this file to the real
# application id before a build and back afterwards, so the package has to be
# spelled exactly like this.
-keep class com.raylib.raymob.** { *; }

# The four onApp* callbacks are private native methods registered from C with
# RegisterNatives (thirdparty/raymob/callback.c). proguard-android-optimize.txt
# happens to carry this rule; depending on a default file's contents for
# something that aborts the VM when it is missing is not a plan.
-keepclasseswithmembernames class * {
    native <methods>;
}
