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
| [assets_rres_loading.cpp](assets_rres_loading.cpp) | Loading assets with `assets::` — the same code whether it is loose files or a packed/AES-encrypted `resources.rres`. |
| [main.c](main.c) | **The opt-out.** A plain C entry point that includes only `<raylib.h>`: no template header, no `assets::`, your own `main()`. You keep the fourteen build targets and lose the runtime layer. |

## Notes

- The game is C++20; these examples are `.cpp` and follow the same
  `_ready/_process/_exit` structure as `src/main.cpp`. `main.c` is the
  deliberate exception — it is what you copy over `src/` if you want none of
  the above.
- Everything the template offers comes from one header,
  `<raylib_multiplatform.h>`. There is nothing else to include — it is an
  umbrella over `include/raylib_multiplatform/`, which you never include from
  directly.
- `<admob.h>` is safe everywhere (no-op off Android, no `#ifdef` needed).
  `<raymob.h>` only declares its functions on Android, so guard those calls
  with `#ifdef __ANDROID__`.
- Replace the AdMob **test** ids in `[android.admob]` in
  `raylib_multiplatform.toml` with your own before publishing — or set
  `enabled = false` there and the whole SDK leaves the build.
