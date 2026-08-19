#pragma once
// ---------------------------------------------------------------------------
// Platform bindings. Pulled in by <raylib_multiplatform.h>; you never include
// this file yourself.
//
//   raymob.h      the Android side of raylib — vibration, soft keyboard, the
//                 activity's JNI handles. Android only: it needs the NDK's
//                 <jni.h>, which no other target has.
//
//   admob.h       interstitial and rewarded ads. Real calls on Android, inline
//                 no-op stubs everywhere else, so ad code compiles on all
//                 fourteen targets without a single #ifdef. The chain behind
//                 it: thirdparty/raymob/admob.c (JNI) -> NativeLoader.java
//                 (Google Mobile Ads SDK). The ad unit ids come from
//                 [android.admob] in raylib_multiplatform.toml.
//
//   smoke_test.h  the CI gate (tests/). Outside CI, RAY_TEST_MAX_FRAMES is
//                 unset and every one of its functions is a no-op.
// ---------------------------------------------------------------------------

#ifdef __ANDROID__
#include <raymob.h>
#endif // __ANDROID__

#include <admob.h>
#include <smoke_test.h>
