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
| [ui_menu.cpp](ui_menu.cpp) | **`rmp::ui`.** A main menu in three lines, then options, a confirm dialog and a HUD — variants, disabled controls, explicit ids, placement, and the theme. |
| [ui_clay_direct.cpp](ui_clay_direct.cpp) | **The escape hatch.** Using Clay's own macros from your game code, in the same frame as `rmp::ui` — for anything the small API does not expose yet. |
| [admob_interstitial_rewarded.cpp](admob_interstitial_rewarded.cpp) | **`rmp::ads`.** Interstitial + rewarded ads — cross-platform, no-op outside Android. |
| [raymob_mobile_features.cpp](raymob_mobile_features.cpp) | The raymob mobile API via `<raymob.h>` — vibration, soft keyboard, sensors, orientation, app storage (Android-only). |
| [assets_rres_loading.cpp](assets_rres_loading.cpp) | **`rmp::assets`.** Loading by name — the same code whether it is loose files or a packed, AES-encrypted `resources.rres`. |
| [main.c](main.c) | **The opt-out.** A plain C entry point that includes only `<raylib.h>`: no template header, none of `rmp::`, your own `main()`. You keep the fourteen build targets and lose the runtime layer. |

## Notes

- The game is C++20; these examples are `.cpp` and follow the same
  `_ready/_process/_exit` structure as `src/main.cpp`. `main.c` is the
  deliberate exception — it is what you copy over `src/` if you want none of
  the above.
- Everything the template offers comes from one header,
  `<raylib_multiplatform.h>`. There is nothing else to include — it is an
  umbrella over `include/raylib_multiplatform/`, which you never include from
  directly.
- It gives you four namespaces: **`rmp::ui`** (menus, buttons, text),
  **`rmp::assets`** (loading from `resources/`), **`rmp::ads`** and
  **`rmp::utils`** (closing the app, and whatever else earns its place).
  Everything under `rmp::` is the template's; everything else is raylib's,
  unchanged. The full API is in [TECHNICAL.md](../TECHNICAL.md).
- `rmp::ads` is safe everywhere (no-op off Android, no `#ifdef` needed).
  `<raymob.h>` only declares its functions on Android, so guard those calls
  with `#ifdef __ANDROID__`.
- Replace the AdMob **test** ids in `[android.admob]` in
  `raylib_multiplatform.toml` with your own before publishing — or set
  `enabled = false` there and the whole SDK leaves the build.
