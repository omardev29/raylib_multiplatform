# This is a MODIFIED copy of raymob

Upstream: <https://github.com/Bigfoot71/raymob>, the `app/src/main/cpp/raymob/`
directory. Licence: MIT plus raylib's zlib section, reproduced unaltered in
`LICENSE`. MIT has no mark-your-changes clause; the changes are listed anyway
so that a reader does not have to know which licence family imposes what.

| File | Change | Why |
|---|---|---|
| `admob.c` | the AdMob calls check for a live JNI env and clear pending Java exceptions; the whole module is switchable (`[android.admob] enabled`) | a JNI exception left pending crashed the next native call with a message that named neither AdMob nor JNI; a game without ads should not carry the SDK |
| `helper.c`, `display.c`, `admob.c`, `NativeLoader.java` (in `raymob/`) | the fixes recorded in `thirdparty/FROZEN_VERSIONS.md` under "raymob -- patch" (heap overflow in `LoadCacheFile`, `ExceptionCheck` after JNI calls, `DetachCurrentThread` only on threads this code attached, `GetScreenOrientation`'s range check, `onKeyUp`) | each one is a real bug found by reading the JNI contract against the code; see `FROZEN_VERSIONS.md` for the per-site detail |

The Java half lives in `raymob/app/src/main/java/com/raylib/raymob/` with an
identical `LICENSE` at `raymob/LICENSE`.
