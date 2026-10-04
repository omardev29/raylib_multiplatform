# Examples

Small, focused examples of what **this framework** adds on top of plain raylib,
grouped by the namespace they belong to. Every one of them is a project of its
own — `src/main.cpp` with the entry point, `include/` for its headers when it
has any, `src/scenes/` or `src/objects/` when it is big enough to want them,
`resources/` when it brings its own art — laid out exactly like a game written
with this framework. Copy a folder and you have a starting point.

```
rmp example                 what there is
rmp example 01_pong         build one and run it
rmp example games/01_pong   the same, by path
rmp test examples           build every one and boot it headless, with a screenshot each
```

CI does the last one on every push, in the pinned build image, under raylib's
software renderer: every example is built for real, booted for thirty frames,
checked for pixels, and photographed. The screenshots are an artefact of the
`examples` job. A file that parses is not a game that plays, and the syntax
check this folder used to get let four of the six games sit broken for a phase.

For raylib itself — drawing, cameras, models, shaders — see the official
[raylib examples](https://www.raylib.com/examples.html). This folder only covers
what is ours.

## [`scenes/`](scenes) — `rmp::Scene`

| | |
|---|---|
| [01_stack](scenes/01_stack/src/main.cpp) | The whole model, one scene per file under `src/scenes/`: a menu that changes to a game, and a game that pushes a pause overlay onto itself. The pause scene contains **no policy** — freezing the world is what the defaults already do — and nothing in the game knows it exists. Plus `rmp::global<T>()` for the score that survives the transition. **Start here.** |

## [`input/`](input) — `rmp::input`

| | |
|---|---|
| [01_actions](input/01_actions/src/main.cpp) | Named actions with as many bindings as you like, eight-direction movement in one call, and the routing that stops a finger on a UI button from also firing the gun — which costs no code at all. |

## [`ui/`](ui) — `rmp::ui`

| | |
|---|---|
| [01_menu](ui/01_menu/src/main.cpp) | A main menu in three lines, then an options screen, a confirm dialog and a HUD — one screen per file under `src/screens/`. Variants, disabled controls, explicit ids, placement, the Theme. **Start here.** |
| [02_layout](ui/02_layout/src/main.cpp) | `row`, `column`, `panel`, `center`, `stack`/`layer`, `spacer`, plus `image` and `progress`. The Sizing model — fit, grow, fixed — and why design units are not pixels. |
| [03_clay_direct](ui/03_clay_direct/src/main.cpp) | **The escape hatch.** Clay's own macros in the same frame as `rmp::ui`, for anything the small API does not expose yet. |
| [04_settings](ui/04_settings/src/main.cpp) | Checkbox, slider, dropdown, text input — and the focus/keyboard/gamepad navigation you get without writing any. The state model, which is "a pointer to your variable" and nothing else; the variable lives in `include/settings.h`. The two volume sliders are the `rmp::audio` buses, and the settings are kept with `rmp::save` -- read with every field defaulting to the struct, written on Apply, and a damaged file said as such. |
| [05_inventory](ui/05_inventory/src/main.cpp) | `grid` with a column count worked out from the space available, `scroll` with clipping, and `wants_pointer()` keeping the game's hands off the UI's clicks. One panel per file under `src/panels/`. |
| [06_style](ui/06_style/src/main.cpp) | The two themes, the five variants, the three sizes and the transition. Nothing in it names a colour — that is the point. Plus copy-modify-set for a Theme of your own. |
| [07_responsive](ui/07_responsive/src/main.cpp) | `scale()` versus `current_breakpoint()`, and which to reach for. A sidebar that becomes a top strip when the window is taller than it is wide. Resize it. |

## [`ads/`](ads) — `rmp::ads`

| | |
|---|---|
| [01_interstitial](ads/01_interstitial/src/main.cpp) | Full-screen ads between levels: request, check, show, request the next one. |
| [02_rewarded](ads/02_rewarded/src/main.cpp) | Watch an ad, get a reward — and only if the player actually finished it. The flow where a mistake costs money or trust. |

## [`assets/`](assets) — `rmp::assets`

| | |
|---|---|
| [01_loading](assets/01_loading/src/main.cpp) | Loading by name — the same code whether it comes from loose files or a packed, AES-encrypted `resources.rres`. The counted handles (`rmp::Texture`, `rmp::Font`, `rmp::Sound`) and why there is no `Unload*` in the file, nor an `InitAudioDevice()`: the sound device opens on the first sound. Brings its own [`resources/`](assets/01_loading/resources), which is all an example has to do to read its own. |

## [`games/`](games) — seven whole games

Not decoration. They are the judges of the one rule that outranks the others —
*the API has to be as simple as possible for the user* — and they are what turns
"the API is simple" into something you can check by reading. What is worth
counting in each one is not the lines but what the lines **say**: nearly every
one of them is a rule of that game. And each one has to be playable from start
to finish — win, lose, play again — or it does not belong here; CI boots them
all, and the screenshot says whether they look like games.

| | |
|---|---|
| [01_pong](games/01_pong/src/main.cpp) | `rmp::behavior::Ball` and `Edge::CLAMP`, two players, first to seven. No bounce arithmetic, no bounds check, no frame loop. The serve is the game's, because no behavior chooses a direction for you — and so is the one field that makes a point possible: `bounds` WIDER than the court, since a ball that bounces off all four sides can never go out. With sound: `rmp::audio::play("hit")` on the paddle and nothing else -- no device to open, feed or close. **Start here.** |
| [02_breakout](games/02_breakout/src/main.cpp) | A wall of bricks, and "a brick breaks" as a one-line `on_collision` — which is exactly why `destroy_on_hit` is not in the catalogue. Three lives, and an open floor for the same reason Pong has open sides. A lost ball shakes the camera, which gameplay never sees. |
| [03_space_invaders](games/03_space_invaders/src/main.cpp) | Collision layers earning their place, `Timer` + a callback instead of a `shooter` behavior, `hurt_by` deciding what may hurt the ship, and the formation written out — because the formation **is** Space Invaders. A handle per alien and one number is the whole of it. |
| [04_top_down](games/04_top_down/src/main.cpp) | `TopDown` with eight normalised directions, `Follow` for the enemies, an HP bar with `rmp::ui::progress`, and what you can only say with layers: the player's shot misses the player, enemies pass through each other but not walls, and the doorway out sees the player and nothing else. The bits are named once, in `include/layers.h`. |
| [05_endless_runner](games/05_endless_runner/src/main.cpp) | `Parallax`, `Runner`, a `Spawner` **by distance** and a camera that follows, with a little smoothing. No background loop, no distance counter, no camera arithmetic. Read it for what is not there. Its art is in its own `resources/`, drawn by [`tools/make_example_art.py`](../tools/make_example_art.py). |
| [06_tetris](games/06_tetris/src/main.cpp) | The honest one: a Tetris is a 10×20 array and the rules of Tetris, and the framework does not pretend otherwise. What it does contribute is named at the top of the file, and so is what it does not — plus a 7-bag out of `rmp::random`, which is what makes the same seed the same game, and the best score kept between sessions with `rmp::save`. |
| [07_platformer](games/07_platformer/src/main.cpp) | **A level designed in [LDtk](https://ldtk.io).** Three levels side by side in one world — a meadow, a desert, a snowfield — with Kenney's Pixel Platformer art. The player stands on the IntGrid and runs along it without catching on a seam, every coin, enemy, sign, door and platform is placed in the editor and arrives through `on_object()` with its fields (a patrol is an `Array<Point>`, a platform's destination a `Point`, the key names its door with an `EntityRef`), and walking off the edge of a level is `neighbour_at()` and a scene change. Music, effects, a shaking camera held to each level's edges, the run in an `rmp::global`, the best one in `rmp::save`. The art, the sounds and the first version of `world.ldtk` come out of [`tools/make_platformer_assets.py`](../tools/make_platformer_assets.py); the level is LDtk's from there on. One file per scene under `src/scenes/`, the things in the level under `src/objects/`. |

## [`platform/`](platform)

| | |
|---|---|
| [02_mobile_raymob](platform/02_mobile_raymob/src/main.cpp) | The raymob mobile API: vibration, soft keyboard, sensors, orientation, app storage. Android only. |
| [03_minimal_includes](platform/03_minimal_includes/src/hud.cpp) | **The proof that `rmp/ui.h` stands alone.** One translation unit that includes `<rmp/ui.h>` and nothing else of ours, and no `main()` on purpose: it is compiled, never linked, and if it stops compiling the header has grown a dependency. |

## [`plain_c/`](plain_c)

| | |
|---|---|
| [main.c](plain_c/src/main.c) | **The opt-out.** A C entry point that includes only `<raylib.h>`: none of our headers, none of `rmp::`, your own `main()`. You keep fifteen of the seventeen build targets and lose the runtime layer — iOS and Web are the two you give up, because on those the loop is not yours to write, and the file says so. Built by CI as C99, and not booted: it has no frame budget. |

## Notes

- The game is C++20 and so are these, except `plain_c/src/main.c`, which is C99
  on purpose and is built as such.
- `rmp test examples` and the CI job run [`tools/examples_build.sh`](../tools/examples_build.sh):
  one script, so the two cannot drift. `rmp test` deliberately does **not**
  include it, so your machine is not compiling a growing folder every time you
  check your own change.
- **There is no umbrella header.** Each example includes the headers it uses and
  only those: `rmp/app.h` for the entry point, then `rmp/scene.h`, `rmp/object.h`,
  `rmp/behavior.h`, `rmp/input.h`, `rmp/ui.h`, `rmp/assets.h`, `rmp/tilemap.h`,
  `rmp/random.h`, `rmp/ads.h`, `rmp/math.h` and `rmp/config.h` as needed. What you
  never include is anything under `src/`. The one deliberate exception to all of
  this is `<clay.h>` in `ui/03_clay_direct`, which is the point of that example.
- Namespaces: **`rmp::app`** (the entry point, closing the app, `rmp::global`),
  **`rmp::Scene`** and **`rmp::Object`** with **`rmp::behavior`**, **`rmp::input`**,
  **`rmp::ui`**, **`rmp::assets`**, **`rmp::random`**, **`rmp::ads`**. Everything
  under `rmp::` is ours; everything else is raylib's, unchanged in its API. The
  full reference is in [TECHNICAL.md](../TECHNICAL.md).
- `rmp::ads` is safe to call everywhere — no-op off Android, no `#ifdef` needed.
  `<raymob.h>` only declares its functions on Android, so guard **those** with
  `#ifdef __ANDROID__`.
- Replace the AdMob **test** ids in `[android.admob]` before publishing, or set
  `enabled = false` there and the whole SDK leaves the build.
