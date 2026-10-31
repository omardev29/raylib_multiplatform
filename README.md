# raylib_multiplatform

**📖 Documentation: <https://omardev29.github.io/rmp-docs/>** — a tutorial that builds a game
from nothing to a release, the manual, the examples (most of them playable in the browser),
publishing, and the reference. Every C and C++ block there is compiled against this repository.

> [!WARNING]
> **This is experimental only, and it is not for production.** The pipeline is green and the render gates
> are real, but the features are very experimental and not intended to production

A **C++20** framework for shipping [raylib](https://www.raylib.com/) games to 17 targets from one
codebase — Windows, Linux, macOS, Web, Android, iOS and the three BSDs — with a CI pipeline that
builds them, **boots them, and checks they actually put pixels on screen**.

<p align="center">
  <img src=".github/img/platformer.png" width="49%" alt="The platformer example: a level designed in LDtk">
  <img src=".github/img/skinned.png" width="49%" alt="A shop skinned with pictures, nine-slices and a custom font">
</p>
<p align="center">
  <img src=".github/img/settings.png" width="49%" alt="A settings screen: checkboxes, sliders, dropdowns, a text field">
  <img src=".github/img/style.png" width="49%" alt="The light theme, variants and sizes">
</p>
<p align="center">
  <img src=".github/img/inventory.png" width="32%" alt="An inventory grid with tooltips and a scrolling list">
  <img src=".github/img/overlay.png" width="32%" alt="A dialog over the game, dimmed behind it">
  <img src=".github/img/tetris.png" width="32%" alt="A Tetris, one of the example games">
</p>

Every picture on this page is a frame the CI draws and compares pixel for pixel on every commit:
the games and menus are played from scripts, and a shot that moves fails the build.

---

## A main menu is three functions

```cpp
rmp::ui::begin();
if (rmp::ui::button("Play")) play();
if (rmp::ui::button("Quit")) quit();
rmp::ui::end();
```

<p align="center">
  <img src=".github/img/menu.png" width="70%" alt="The menu those calls draw">
</p>

Centred, sized for the screen it is on, and playable with a mouse, a finger, a keyboard or a
gamepad without a line about any of them. That is the benchmark for every API here: anything that
would make it longer is redesigned until it does not.

When a screen is *designed* rather than written, the same controls are widgets you keep as members
of your scene — each with its own look, sounds and callbacks:

```cpp
rmp::ui::Button buy{
    .text = "Buy a star",
    .icon = rmp::ui::slice(skin, "coin"),          // a slice of an Aseprite file
    .style = { .skin = rmp::ui::nine_slice(skin, "button"),
               .shadow = rmp::ui::Shadow{},
               .sounds = { .click = "coin" } },
    .on_click = [this] { if (!spend(3)) buy.shake(); },
};
```

Themes, styles that children inherit, nine-slices, per-state skins, soft shadows, text outlines,
motion (hover, press, appear, shake), any font with any letter baked on demand, overlays that
blur the game behind them, tooltips and notices.

## Scenes, objects and behaviors

<p align="center">
  <img src=".github/img/top_down.png" width="49%" alt="The top-down example: a player, enemies with health bars, bullets">
  <img src=".github/img/breakout.png" width="49%" alt="The breakout example">
</p>

A game is **scenes** on a stack, and a scene is three hooks: `_ready()` builds it, `_update()`
runs its logic and `_draw()` its interface, while the scene itself moves, collides and draws its
**objects**. `src/main.cpp` names the first one and is done:

```cpp
RMP_GAME(MenuScene);   // opens the window and runs the scene, on every target
```

```cpp
class ArenaScene : public rmp::Scene {
    void _ready() override {
        auto &player = spawn({ .position = { 200, 225 }, .shape = rmp::rect({ 26, 26 }) });
        player.add<rmp::behavior::TopDown>({ .speed = 240 });          // eight directions
        player.add<rmp::behavior::Health>({
            .hp = 3,
            .on_death = [](rmp::Object &) { rmp::Scene::push<OverScene>("You died"); },
        });
        camera.follow = player.handle();

        auto &spawner = spawn({ .position = { 760, 225 } });
        spawner.add<rmp::behavior::Spawner>({
            .every_seconds = 2,
            .max_alive = 5,
            .on_spawn = [target = player.handle()](rmp::Scene &s, Vector2 at) {
                auto &enemy = s.spawn({ .position = at, .shape = rmp::circle(14) });
                enemy.add<rmp::behavior::Follow>({ .target = target, .speed = 90 });
            },
        });
    }

    void _update(float) override {
        if (rmp::input::just_pressed("pause")) push<PauseScene>();  // pop() comes back
    }
};
```

An **object** is a position, a velocity, a shape or an animated sprite, collision layers and
masks, and hooks of its own (`_update`, `_draw`, `_collision`) or callbacks (`on_click`,
`on_collision`) — the sweeps, the separation, the raycasts and the drawing order are the scene's.
A **behavior** is how it moves or what it does, one `add<>()` each: `TopDown`, `Platformer`,
`Runner`, `Ball`, `Projectile`, `Follow`, `Tween`, `GridSnap`, `Parallax`, `Spawner`, `Timer`,
`Lifespan`, `Health`, or one of your own. Keep an object across frames with its `rmp::Handle`,
which goes empty when the object dies instead of dangling.

## A level from LDtk, and the objects in it

<p align="center">
  <img src=".github/img/platformer_title.png" width="49%" alt="The platformer's title over its own level">
  <img src=".github/img/runner.png" width="49%" alt="The endless runner, with parallax layers">
</p>

```cpp
void LevelScene::_ready() {
    map = rmp::assets::load_map("world.ldtk");                // drawn and collided by the scene
    map.on_object("Coin", [](rmp::Scene &s, const rmp::MapObject &o) {
        s.spawn<Coin>({ .position = o.position }).value = o.property_int("value", 1);
    });

    auto &player = spawn<Player>({ .position = { 64, 64 }, .size = { 14, 22 } });
    player.add<rmp::behavior::Platformer>({ .jump = 700, .air_jumps = 1 });
    player.sprite.sheet = rmp::assets::load_sheet("hero.aseprite");
    camera.follow = player.handle();
}
```

Levels are placed, linked and tuned in [LDtk](https://ldtk.io) (or Tiled), and each entity type
is a factory of yours; sprites are Aseprite files with their tags as animations. The seven games in
`examples/games/` (Pong to the platformer) are each playable start to finish.

## Input, sound, saves, tuning

```cpp
rmp::input::action("jump", KEY_SPACE, KEY_W, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
if (rmp::input::just_pressed("jump")) rmp::audio::play("jump");
rmp::audio::music("level1");

rmp::Value best;
rmp::save::read("records", best);                 // every read has a default
best["coins"] = std::max(best["coins"].as_int(0), coins);
rmp::save::write("records", best);                // the right folder on every platform

static rmp::Value tuning = rmp::assets::load_value("tuning.json");
if (tuning.changed()) tuning.reload();            // edited while the game runs: hot reload
```

Actions with any number of bindings, routed away from the game while the interface wants the
pointer; sounds and music by name, whatever the format; saves as real structures, sealed if you
like; JSON and textures that reload when you save them in your editor, in a debug build.

---

## One command, every platform

```bash
rmp new my_game              # a game of your own, with the framework and CI for every target
rmp run                      # build (debug) and play
rmp test                     # the .toml, the pins, the licences, a frame drawn, a boot
rmp build release            # the build players get
rmp web                      # or: rmp android
rmp build windows            # the Windows .exe, from Linux (MinGW-w64, or clang-cl)
rmp run windows              # ...started under Wine
rmp package android          # what a release ships, made here: desktop, windows, web, android
rmp deploy 1.2.3             # tag a release; CI builds all 17 targets and publishes them
```

- **One config file**, `raylib_multiplatform.toml`, generates the CMake, Gradle and Xcode inputs,
  so they cannot drift; an invalid one fails in a second, naming the line.
- **Fast debug builds**: in a new game, the C of raylib and its libraries is compiled by tcc when
  it is installed, many times faster than GCC or Clang.
- **A development container** (`.devcontainer/`) that is the build image CI uses, with a desktop
  in the browser: open the repository in VS Code or a Codespace and `rmp run` works.
- **CI that proves things**: every target built, the desktop, web, Android, iOS-simulator and BSD
  builds booted and a frame read back, the games and menus played from scripts and compared
  exactly, sanitizers, fuzzing, property tests, coverage that only rises, size budgets per
  binary, and a licence check that fails on anything copyleft. What each target gets:
  [What CI covers](https://omardev29.github.io/rmp-docs/publishing/ci.html).
- **Releases** are a git tag, and the build is refused while your application id is still `com.example.*`, so you cannot cut your first release under a placeholder identity you can never change.
  Google Play, the App Store, itch.io and the rest:
  [Publishing](https://omardev29.github.io/rmp-docs/publishing/).

---

## Quick start

```bash
curl -fsSL https://omardev29.github.io/rmp-docs/install.sh | sh     # Linux, macOS
irm https://omardev29.github.io/rmp-docs/install.ps1 | iex          # Windows, in PowerShell
```

Then, in a new terminal, `rmp new my_game`, `cd my_game`, `rmp run`. You need **Python 3.11+**,
**CMake**, **Ninja** and a C++ compiler; on Linux the X11 development libraries too:

```bash
sudo apt install libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev
```

On a Linux console with no desktop, `rmp` builds for DRM/KMS instead — straight to the screen, no
X11 — and needs these in their place:

```bash
sudo apt install libdrm-dev libgbm-dev libegl1-mesa-dev libgles2-mesa-dev
```

The rest — the BSDs, a clone of your own, uninstalling — is in
[Install](https://omardev29.github.io/rmp-docs/getting-started/install.html), and the first game
step by step in the [tutorial](https://omardev29.github.io/rmp-docs/getting-started/).

---

### What we add on top of raylib

All of raylib's API is there: `DrawTexture`, `LoadModel`, `IsKeyPressed`, everything. The copy in
`thirdparty/raylib/` is raylib 6.2 with local patches, all listed in
[`thirdparty/raylib/PATCHES.md`](thirdparty/raylib/PATCHES.md) and marked at each site, none of
them changing a signature. On top of it, a few small namespaces, all under `rmp::`:

| Namespace | What it is for |
| --- | --- |
| **`rmp::app`** | The entry point, and `quit()` that is right on every platform; what outlives a scene; a window that is not the design size, letterboxed or expanded. [Manual](https://omardev29.github.io/rmp-docs/manual/core/app.html) |
| **`rmp::Scene`** | State, update and drawing on a stack: change, push, pop. A pause menu is a scene pushed on top. [Manual](https://omardev29.github.io/rmp-docs/manual/core/scenes.html) |
| **`rmp::Object`** | What lives in a scene: position, velocity, a shape or a sprite, collision layers, `on_click`, and the sweeps, separation and raycasts done for you. [Manual](https://omardev29.github.io/rmp-docs/manual/core/objects.html) |
| **`rmp::behavior`** | `TopDown`, `Platformer`, `Runner`, `Ball`, `Projectile`, `Follow`, `Tween`, `GridSnap`, `Parallax`, `Spawner`, `Timer`, `Lifespan`, `Health`: one `add<B>({...})` each. [Manual](https://omardev29.github.io/rmp-docs/manual/core/behaviors.html) |
| **`rmp::Camera`** | `follow`, `limits`, `smoothing` that is the same at 30 and 144 Hz, and `shake()`. [Manual](https://omardev29.github.io/rmp-docs/manual/core/camera.html) |
| **`rmp::input`** | Named actions, axes and vectors, routed away from the game while the UI wants the pointer. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/input.html) |
| **`rmp::ui`** | Menus, HUDs, settings screens and shops, simple calls or widgets you keep, themes and styles, overlays, tooltips and notices. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/ui.html) |
| **`rmp::assets`** | Loading by name from loose files or a packed, encrypted `.rres`; counted handles that release themselves; hot reload of JSON and textures. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/assets.html) |
| **`rmp::Tilemap`** | A level from LDtk or Tiled, drawn, collided and populated through your factories. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/tilemaps.html) |
| **`rmp::audio`** | `play("coin")` and `music("level1")` by name; three volume buses. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/audio.html) |
| **`rmp::save`** | `rmp::Value` saves with a default on every read, in the right folder everywhere. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/saving.html) |
| **`rmp::random`** | Seeded and reproducible. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/random.html) |
| **`rmp::ads`** | Interstitial and rewarded ads on Android, nothing everywhere else, no `#ifdef`s. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/ads.html) |

There is no umbrella header: you include what you use — `rmp/app.h`, `rmp/scene.h`,
`rmp/object.h`, `rmp/behavior.h`, `rmp/input.h`, `rmp/ui.h`, `rmp/assets.h`, `rmp/tilemap.h`,
`rmp/audio.h`, `rmp/save.h`, `rmp/random.h`, `rmp/ads.h`, `rmp/math.h` — and `rmp/config.h`, which
every one of them already carries for you.

Rather write plain C? [`examples/plain_c/src/main.c`](examples/plain_c/src/main.c) is a whole
game with your own `main()` and no `rmp::` anything; in a game made with `rmp new`,
`rm src/main.cpp && rm -r src/rmp/ src/scenes/ tests/game/` and copy it into `src/`. See
[Plain C](https://omardev29.github.io/rmp-docs/manual/raylib/plain-c.html).

---

## Licence

This framework's own code — `src/`, `include/`, `tools/`, `cmake/`, `ios/` and the build files —
is MIT; see [LICENSE](LICENSE).

It is **built on raylib**, which is zlib/libpng licensed and is not ours. This project is not
affiliated with or endorsed by raylib or Ramon Santamaria. The copy it vendors is **modified**,
and every change is listed in [`thirdparty/raylib/PATCHES.md`](thirdparty/raylib/PATCHES.md), as
the zlib licence's second clause requires of altered source versions. Clay, cute_tiled, raymob
and raylib-cpp are likewise modified, and each lists its changes in a `PATCHES.md` of its own.

Every vendored component — down to the libraries raylib bundles — with its licence and whether we
altered it, is in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). A release ships the full
notices as `LICENSES.txt` next to the binary, inside the APK, and in the iOS bundle;
`tools/license_db.py --check` fails the build on a copyleft or unknown licence and on an
alteration nobody marked.
