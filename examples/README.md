# Examples

Small, focused examples of the things **this template** gives you on top of
plain raylib. They are **reference code** — read them and copy the relevant
bits into `src/main.cpp`. They are **not** compiled by the build, so they can
lag slightly; treat them as documentation with code.

For general raylib usage (drawing, cameras, models, shaders…) see the official
[raylib examples](https://www.raylib.com/examples.html) — this folder only
covers the template's own features.

| Example | Shows |
|---|---|
| [lifecycle_ready_process_exit.cpp](lifecycle_ready_process_exit.cpp) | The core `_ready()` / `_process()` / `_exit()` (Godot-style) pattern every game in this template uses. |
| [admob_interstitial_rewarded.cpp](admob_interstitial_rewarded.cpp) | AdMob interstitial + rewarded ads via `<admob.h>` — cross-platform, no-op outside Android. |
| [raymob_mobile_features.cpp](raymob_mobile_features.cpp) | The raymob mobile API via `<raymob.h>` — vibration, soft keyboard, sensors, orientation, app storage (Android-only). |
| [assets_rres_loading.cpp](assets_rres_loading.cpp) | Loading assets through the dual-mode `Assets::` layer — loose files or a packed/AES-encrypted `resources.rres`. |

## Notes

- The game is C++20; these examples are `.cpp` and follow the same
  `_ready/_process/_exit` structure as `src/main.cpp`.
- `<admob.h>` is safe everywhere (no-op off Android, no `#ifdef` needed).
  `<raymob.h>` only declares its functions on Android, so guard those calls
  with `#ifdef __ANDROID__`.
- Replace the AdMob **test** ids in `raymob/gradle.properties` with your own
  before publishing.
