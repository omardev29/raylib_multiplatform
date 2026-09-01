# Examples

Small, focused examples of what **this framework** adds on top of plain raylib,
grouped by the namespace they belong to. They are **reference code**: read them
and copy the parts you need into `src/`. They are not compiled into your game —
but CI does syntax-check every one of them, with GCC and with MSVC, on every
push, so they cannot quietly stop working.

For raylib itself — drawing, cameras, models, shaders — see the official
[raylib examples](https://www.raylib.com/examples.html). This folder only covers
what is ours.

## [`scenes/`](scenes) — `rmp::Scene`

| | |
|---|---|
| [01_stack.cpp](scenes/01_stack.cpp) | The whole model in one file: a menu that changes to a game, and a game that pushes a pause overlay onto itself. The pause scene contains **no policy** — freezing the world is what the defaults already do — and nothing in the game knows it exists. Plus `rmp::global<T>()` for the score that survives the transition. **Start here.** |

## [`input/`](input) — `rmp::input`

| | |
|---|---|
| [01_actions.cpp](input/01_actions.cpp) | Named actions with as many bindings as you like, eight-direction movement in one call, and the routing that stops a finger on a UI button from also firing the gun — which costs no code at all. |

## [`ui/`](ui) — `rmp::ui`

| | |
|---|---|
| [01_menu.cpp](ui/01_menu.cpp) | A main menu in three lines, then an options screen, a confirm dialog and a HUD. Variants, disabled controls, explicit ids, placement, the Theme. **Start here.** |
| [02_layout.cpp](ui/02_layout.cpp) | `row`, `column`, `panel`, `center`, `stack`/`layer`, `spacer`, plus `image` and `progress`. The Sizing model — fit, grow, fixed — and why design units are not pixels. |
| [03_clay_direct.cpp](ui/03_clay_direct.cpp) | **The escape hatch.** Clay's own macros in the same frame as `rmp::ui`, for anything the small API does not expose yet. |
| [04_settings.cpp](ui/04_settings.cpp) | Checkbox, slider, dropdown, text input — and the focus/keyboard/gamepad navigation you get without writing any. The state model, which is "a pointer to your variable" and nothing else. |
| [05_inventory.cpp](ui/05_inventory.cpp) | `grid` with a column count worked out from the space available, `scroll` with clipping, and `wants_pointer()` keeping the game's hands off the UI's clicks. |
| [06_style.cpp](ui/06_style.cpp) | The two themes, the five variants, the three sizes and the transition. Nothing in it names a colour — that is the point. Plus copy-modify-set for a Theme of your own. |
| [07_responsive.cpp](ui/07_responsive.cpp) | `scale()` versus `current_breakpoint()`, and which to reach for. A sidebar that becomes a top strip when the window is taller than it is wide. Resize it. |

## [`ads/`](ads) — `rmp::ads`

| | |
|---|---|
| [01_interstitial.cpp](ads/01_interstitial.cpp) | Full-screen ads between levels: request, check, show, request the next one. |
| [02_rewarded.cpp](ads/02_rewarded.cpp) | Watch an ad, get a reward — and only if the player actually finished it. The flow where a mistake costs money or trust. |

## [`assets/`](assets) — `rmp::assets`

| | |
|---|---|
| [01_rres_and_loose_files.cpp](assets/01_rres_and_loose_files.cpp) | Loading by name — the same code whether it comes from loose files or a packed, AES-encrypted `resources.rres`. |

## [`platform/`](platform)

| | |
|---|---|
| [02_mobile_raymob.cpp](platform/02_mobile_raymob.cpp) | The raymob mobile API: vibration, soft keyboard, sensors, orientation, app storage. Android only. |

## [`plain_c/`](plain_c)

| | |
|---|---|
| [main.c](plain_c/main.c) | **The opt-out.** A C entry point that includes only `<raylib.h>`: none of our headers, none of `rmp::`, your own `main()`. You keep fifteen of the seventeen build targets and lose the runtime layer — iOS and Web are the two you give up, because on those the loop is not yours to write, and the file says so. |

## Notes

- The game is C++20 and so are these, except `plain_c/main.c`, which is C99 on
  purpose and is checked as such.
- These are compiled by CI in a job of their own, on its own runner — not on
  your machine every time you build. `just test examples` runs the same check
  locally when you have changed the API and want to know what you broke.
- **There is no umbrella header.** Each example includes the headers it uses and
  only those: `rmp/app.h` for the entry point, then `rmp/ui.h`, `rmp/assets.h`,
  `rmp/ads.h`, `rmp/math.h`, `rmp/config.h` as needed. `platform/03_minimal_includes.cpp`
  exists to prove `rmp/ui.h` stands on its own, and CI compiles it, so it stays
  true. What you never include is anything under `src/`. The one deliberate
  exception to all of this is `<clay.h>` in `ui/03_clay_direct.cpp`, which is the
  point of that example.
- Four namespaces: **`rmp::ui`** (menus, buttons, layout), **`rmp::assets`**
  (loading from `resources/`), **`rmp::ads`** and **`rmp::app`** (closing the
  app). Everything under `rmp::` is ours; everything else is raylib's,
  unchanged. The full API is in [TECHNICAL.md](../TECHNICAL.md).
- `rmp::ads` is safe to call everywhere — no-op off Android, no `#ifdef` needed.
  `<raymob.h>` only declares its functions on Android, so guard **those** with
  `#ifdef __ANDROID__`.
- Replace the AdMob **test** ids in `[android.admob]` before publishing, or set
  `enabled = false` there and the whole SDK leaves the build.
