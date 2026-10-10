# raylib_multiplatform

A **C++20** framework for shipping [raylib](https://www.raylib.com/) games to 17 targets from one
codebase — Windows, Linux, macOS, Web, Android, iOS and the three BSDs — with a CI pipeline that
builds them, **boots them, and checks they actually put pixels on screen**.

> [!WARNING]
> **This is experimental only, and it is not for production.** The pipeline is green and the render gates
> are real, but the features are very experimental and not intended to production

**The documentation is at <https://omardev29.github.io/rmp-docs/>**: a tutorial that builds a game
from nothing to a release, the manual, the examples (most of them playable in the browser),
publishing, and the reference. Every C and C++ block there is compiled against this repository,
every `.toml` block goes through its `configure.py`, and what a machine can check about the rest is
checked, so it is the place to read. This page is the short version.

---

## Look how little you have to do

| You edit | For |
| --- | --- |
| `src/` | Your game. `src/main.cpp` ends at `RMP_GAME(MainMenuScene);`, naming the first scene; the game is in `src/scenes/` (and `src/objects/` when it has objects of its own). Every `.cpp`/`.c` under `src/` is compiled automatically, subfolders included. |
| `include/` | Your headers. |
| `resources/` | Your assets — images, sounds, fonts, levels, models. |
| `branding/icon.png` | Your app icon. One 1024×1024 PNG. |
| `raylib_multiplatform.toml` | Your name, app ids, which platforms to build, everything else. |

That is the list. You do **not** edit CMakeLists.txt to rename your game, or `gradle.properties`
for Android, or `project.yml` for iOS, or any workflow file to choose platforms. Those are
generated from the config on every build, which is why they cannot drift out of sync with it.
`include/rmp/` and `src/rmp/` are the framework's; everything else under `src/` and `include/` is
yours.

---

## Quick start

`rmp` is the one command: it builds, runs, tests and ships a game, and makes new ones. Get it
once by cloning this repository into your home folder and putting it on your PATH:

```bash
cd ~
git clone https://github.com/omardev29/raylib_multiplatform
```

| Shell | Add to PATH (once) |
| --- | --- |
| bash on Linux | `echo 'export PATH="$HOME/raylib_multiplatform:$PATH"' >> ~/.bashrc` |
| bash on macOS | `echo 'export PATH="$HOME/raylib_multiplatform:$PATH"' >> ~/.bash_profile` |
| zsh | `echo 'export PATH="$HOME/raylib_multiplatform:$PATH"' >> ~/.zshrc` |
| ksh | `echo 'export PATH="$HOME/raylib_multiplatform:$PATH"' >> ~/.profile` |
| PowerShell and cmd | `[Environment]::SetEnvironmentVariable("Path", "$HOME\raylib_multiplatform;" + [Environment]::GetEnvironmentVariable("Path", "User"), "User")` |

On Windows, PowerShell also has to be allowed to run a local script, once:
`Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`. Open a new terminal, then:

```bash
rmp new my_game       # a game of your own, with the framework and CI for every target
cd my_game
rmp run               # build it and play
```

You need **Python 3.11+**, **CMake** and **Ninja** on PATH, and a C++ compiler. On Linux, the
X11 development libraries too:

```bash
sudo apt install libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev
```

Then open `raylib_multiplatform.toml` in the new game, set your name and app ids, and build again.
`rmp new` fills the ids with `com.example.` placeholders, and `rmp deploy` refuses to tag a release
while they are: a Google Play id can never change once it is published.

The commands you will type every day:

```bash
rmp run               # build (debug) and play
rmp test              # the .toml, the pins, the licences, a frame drawn, a boot
rmp build release     # the release build
rmp web               # or: rmp android
rmp help              # every command; rmp help <command> explains one, with examples
```

Inside a game, `rmp` is the game's own copy (`tools/rmp.py`), so a game keeps working the way
it did when the framework on your PATH moves on. Without `rmp`, it is plain CMake:
`cmake --preset debug` configures and generates everything from the `.toml`, and
`cmake --build build` builds it.

---

## A main menu is three functions

```cpp
rmp::ui::begin();
if (rmp::ui::button("Play")) play();
if (rmp::ui::button("Quit")) quit();
rmp::ui::end();
```

No coordinates, no sizes, no fonts, no hitboxes, and it does not change when the window does. It is
the benchmark for every API here: anything that would make it longer is redesigned until it does
not. The game itself is scenes — `src/main.cpp` names the first one with `RMP_GAME`, which opens
the window and writes the entry point for whichever platform you build — and the objects in them.

### What we add on top of raylib

All of raylib's API is there: `DrawTexture`, `LoadModel`, `IsKeyPressed`, everything. The copy in
`thirdparty/raylib/` is raylib 6.0.0 with local patches, all listed in
[`thirdparty/raylib/PATCHES.md`](thirdparty/raylib/PATCHES.md) and marked at each site, none of
them changing a signature. On top of it this framework adds a few small namespaces, all under
`rmp::`, for the things every game needs and raylib deliberately does not decide for you:

| Namespace | What it is for |
| --- | --- |
| **`rmp::app`** | The entry point, and `quit()`: closing the app cleanly from anywhere, on every platform, including the two where ending the process yourself is wrong. `rmp::global<T>()` for what outlives a scene. [Manual](https://omardev29.github.io/rmp-docs/manual/core/app.html) |
| **`rmp::Scene`** | A self-contained context of state, update and drawing on a stack: change, push, pop. A pause menu is a scene pushed on top and writes no policy. [Manual](https://omardev29.github.io/rmp-docs/manual/core/scenes.html) |
| **`rmp::Object`** | What lives in a scene: position, velocity, a shape or a sprite, solid or not, collision layers, `on_click`. `spawn<T>()` creates it, `rmp::Handle<T>` is how you keep it across frames. Sweeps, separation and raycasts are the scene's, not yours. [Manual](https://omardev29.github.io/rmp-docs/manual/core/objects.html) |
| **`rmp::behavior`** | The catalogue: `TopDown`, `Platformer`, `Runner`, `Ball`, `Projectile`, `Follow`, `Tween`, `GridSnap`, `Parallax`, `Spawner`, `Timer`, `Lifespan`, `Health`. `object.add<B>({...})` and the object moves like that game; the games in `examples/games/` are the proof. [Manual](https://omardev29.github.io/rmp-docs/manual/core/behaviors.html) |
| **`rmp::Camera`** | Every scene's `camera`: `follow` an object, keep inside `limits`, `smoothing` that is the same at 30 and 144 Hz, and `shake()` that never touches gameplay. [Manual](https://omardev29.github.io/rmp-docs/manual/core/camera.html) |
| **`rmp::input`** | Named actions with as many bindings as you like, axes and eight-direction vectors, and routing: an action bound to the mouse is silent while the UI wants the pointer. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/input.html) |
| **`rmp::ui`** | Menus, buttons, text, lists, and the controls a settings screen is made of. Responsive by default, and playable with a mouse, a finger and a controller. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/ui.html) |
| **`rmp::assets`** | Loading from `resources/` by name, without caring whether the game is running from loose files or from a packed, encrypted `.rres`. Counted handles (`rmp::Texture`, `rmp::Font`, `rmp::Sound`, `rmp::SpriteSheet`) release what they own. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/assets.html) |
| **`rmp::Tilemap`** | A level designed in [LDtk](https://ldtk.io) (or Tiled): the scene draws it, collides against its solid cells and spawns its entities through the factories you register. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/tilemaps.html) |
| **`rmp::audio`** | `play("coin")` and `music("level1")` by name, whatever the format; three volume buses for a settings screen. A machine without a sound device simply runs silent. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/audio.html) |
| **`rmp::save`** | `rmp::save::write("slot1", v)` and `read("slot1", &v)`, where `rmp::Value` holds a real structure. Every read has a default, so an update never breaks an old save; the right folder on every platform. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/saving.html) |
| **`rmp::random`** | Seeded and reproducible: the number on a bug report reproduces the run. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/random.html) |
| **`rmp::ads`** | Interstitial and rewarded ads. Real on Android, silently nothing everywhere else, so there are no `#ifdef`s in your game. Off until you turn it on. [Manual](https://omardev29.github.io/rmp-docs/manual/modules/ads.html) |

There is no umbrella header: you include what you use, and each one is a module you can name —
`rmp/app.h`, `rmp/scene.h`, `rmp/object.h`, `rmp/behavior.h`, `rmp/input.h`, `rmp/ui.h`,
`rmp/assets.h`, `rmp/tilemap.h`, `rmp/audio.h`, `rmp/save.h`, `rmp/random.h`, `rmp/ads.h`,
`rmp/math.h` — and `rmp/config.h`, which every one of them already carries for you.

### If you would rather write plain C

[`examples/plain_c/src/main.c`](examples/plain_c/src/main.c) is a complete entry point with your
own `main()` and no `rmp::` anything. Its first lines are the recipe -- in a game made with
`rmp new`, `rm src/main.cpp && rm -r src/rmp/ src/scenes/ tests/game/` and copy the file into
`src/` -- and CI follows them in a fresh game on every commit. See
[Plain C](https://omardev29.github.io/rmp-docs/manual/raylib/plain-c.html) for what you keep and
what you give up.

---

## Releases

A release is a git tag, and there is no version anywhere in the repository to bump: `rmp deploy
1.2.3` checks what CI will check, tags `v1.2.3` and pushes the tag. Tagging runs all 17 targets,
then publishes, and the build is refused while your application id is still `com.example.*`, so
you cannot cut your first release under a placeholder identity you can never change. Signing keys
and tokens are GitHub secrets and never go in the repository. Google Play, the App Store, itch.io
and the rest: [Publishing](https://omardev29.github.io/rmp-docs/publishing/).

## What CI covers

Every push runs a fast lane; a tag, or `gh workflow run ci.yml -f full=true`, runs all 17 targets.
Booting proves the window opened and the assets loaded; the CI also reads a frame back and checks
it has pixels on it, because a broken shader still boots and exits 0. What each target gets —
built, booted, rendered, or only checked statically — and what is pinned is in
[What CI covers](https://omardev29.github.io/rmp-docs/publishing/ci.html). Nothing here tests
*your game*: test it on the platforms you ship.

---

## Licence

This framework's own code — `src/`, `include/`, `tools/`, `cmake/`, `ios/` and the build files —
is MIT; see [LICENSE](LICENSE).

It is **built on raylib**, which is zlib/libpng licensed and is not ours. This project is not
affiliated with or endorsed by raylib or Ramon Santamaria. The copy it vendors is **modified**,
and every change is listed in [`thirdparty/raylib/PATCHES.md`](thirdparty/raylib/PATCHES.md), as
the zlib licence's second clause requires of altered source versions. Clay and cute_tiled are
likewise modified and likewise listed.

Every vendored component — down to the libraries raylib bundles — with its licence, the
alternative we take when there is a choice, and whether we altered it, is in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). A release ships the full notices as
`LICENSES.txt` next to the binary, inside the APK, and in the iOS bundle; `tools/license_db.py --check`
fails the build on a copyleft or unknown licence and on an alteration nobody marked.
