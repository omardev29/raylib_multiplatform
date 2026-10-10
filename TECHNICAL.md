# Technical reference

Notes for whoever maintains the framework: how the build, the resource pack, the platforms and CI work inside, and why. How to **use** the framework is the documentation site, <https://omardev29.github.io/rmp-docs/> -- the tutorial, the manual, the examples and the reference, every code block of it compiled against this repository. For the quick start see [README.md](README.md).

## Table of contents

- [Directory layout](#directory-layout)
- [Build system](#build-system)
- [Editor / clangd (LSP)](#editor--clangd-lsp)
- [Compile-time definitions](#compile-time-definitions)
- [Platform detection macros](#platform-detection-macros)
- [Resources: `RMP_RESOURCES_PATH`, `rmp::assets` and rres](#resources-rmp_resources_path-rmpassets-and-rres)
- [Scenes, objects and the frame](#scenes-objects-and-the-frame)
- [`rmp::ui` — notes for maintainers](#rmpui--notes-for-maintainers)
- [`rmp::app`](#rmpapp)
- [`rmp::audio`](#rmpaudio)
- [`rmp::Camera`](#rmpcamera)
- [`rmp::Tilemap`](#rmptilemap)
- [`rmp::save` — what is tested](#rmpsave--what-is-tested)
- [AdMob (Android)](#admob-android)
- [Web export](#web-export)
- [Android (raymob)](#android-raymob)
- [iOS](#ios)
- [BSD & RISC-V targets](#bsd--risc-v-targets)
- [CI/CD pipeline](#cicd-pipeline)
- [Maintenance & platform longevity](#maintenance--platform-longevity)
- [Adding source files & libraries](#adding-source-files--libraries)
- [FAQ & troubleshooting](#faq--troubleshooting)

---

## Directory layout

```
.
├── raylib_multiplatform.toml # THE config. Name, ids, targets, icon, modules. Yours.
├── CMakeLists.txt            # Root build: the `rmp` library, rmp_add_game(), presets, rres
├── CMakePresets.json         # debug / release / web / memory profiles
├── rmp, rmp.ps1, rmp.cmd     # the command, for sh/bash/zsh/ksh, PowerShell and cmd -> tools/rmp.py
├── src/                      # YOUR code. Every .cpp/.c here is auto-compiled (GLOB_RECURSE).
│   ├── main.cpp              # RMP_GAME(MainMenuScene); and nothing else
│   ├── scenes/               # your scenes, one per file
│   └── rmp/                  # THE framework's implementation, compiled once into `rmp` -- not yours
│       ├── internal.h        #   private seams: report_once, the resources root, the pack
│       ├── report.cpp        #   RMP_REPORT_ONCE: one line per mistake, [dev] strict
│       ├── app.cpp           #   rmp::app -- the frame, quit(), compiled into EACH executable
│       ├── global.cpp        #   rmp::global<T>() registry and its destruction order
│       ├── scene.cpp         #   the scene stack, transitions, the frame order
│       ├── scene_internal.h
│       ├── camera.cpp        #   rmp::Camera -- follow, smoothing, limits, shake, world<->screen
│       ├── object.cpp        #   rmp::Object storage, handles, integration, edges, drawing
│       ├── object_internal.h
│       ├── collision.cpp     #   the broad-phase grid, sweeps, MTV, raycast, the pointer pass
│       ├── behavior.cpp      #   the behavior ENGINE: slots, dispatch, the erased hooks
│       ├── behaviors.cpp     #   the CATALOGUE: TopDown, Platformer, Runner, Ball, Spawner...
│       ├── behavior_internal.h
│       ├── input.cpp         #   rmp::input -- actions, the device seam, routing against the UI
│       ├── random.cpp        #   rmp::random -- xoshiro128++, seeded once at start
│       ├── ads.cpp           #   rmp::ads -- the one file that includes <admob.h>
│       ├── assets.cpp        #   rmp::assets -- the public surface, with the loose-file fallback
│       ├── resource.cpp      #   the counted resource slots behind rmp::Texture and friends
│       ├── pack.cpp          #   open/close resources.rres, read one entry
│       ├── loader_hook.cpp   #   routes raylib's own LoadFileData/Text through the pack
│       ├── rres_impl.cpp     #   compiles rres once (container + AES + Argon2i + QOI)
│       ├── animation.cpp     #   rmp::Sprite playback from an Aseprite sheet
│       ├── animation_internal.h
│       ├── aseprite_impl.cpp #   compiles cute_aseprite once
│       ├── tilemap.cpp       #   rmp::Tilemap -- drawing, solids, object factories; the Tiled reader
│       ├── tilemap_internal.h #  the parsed map both readers fill
│       ├── ldtk.cpp          #   the LDtk reader: levels, worlds, IntGrid, entities and their fields
│       ├── tiled_impl.cpp    #   compiles cute_tiled once
│       ├── audio.cpp         #   rmp::audio -- lazy device, voices, the music stream
│       ├── audio_internal.h
│       ├── value.cpp         #   rmp::Value -- the tree a save holds, and its Ref
│       ├── save.cpp          #   rmp::save -- the file format, sealing, where saves go
│       ├── save_internal.h
│       ├── cjson_impl.c      #   compiles cJSON once, as C
│       ├── json_internal.h   #   cJSON owned, and read in the "C" locale (saves and LDtk)
│       └── ui/               #   rmp::ui
│           ├── clay_impl.cpp #     compiles Clay once
│           ├── internal.h    #     the only place Clay is allowed to exist
│           ├── context.cpp   #     lazy start, scale, font, text arena, element ids, passes
│           ├── widgets.cpp   #     begin / end / button / text
│           ├── containers.cpp #    row / column / panel / stack / grid / scroll / image
│           ├── controls.cpp  #     checkbox / slider / dropdown / text_input
│           ├── focus.cpp     #     keyboard and gamepad navigation, the capture flags
│           ├── style.cpp     #     variants, sizes, transitions
│           ├── render.cpp    #     draw commands -> raylib calls
│           └── theme.cpp     #     the dark and light themes
├── include/                  # YOUR headers (already on the include path)
│   └── rmp/                  # THE framework's headers -- one per module, include what you use
│       ├── app.h             #   RMP_GAME, RMP_ENTRY_POINT, quit(), global<T>()
│       ├── scene.h           #   rmp::Scene -- the stack, spawn<T>(), raycast, the `map` field
│       ├── object.h          #   rmp::Object, Handle<T>, Sprite, shapes, edges, Callback
│       ├── behavior.h        #   rmp::behavior -- the catalogue, added with object.add<B>()
│       ├── input.h           #   rmp::input -- named actions, axes, the pointer
│       ├── ui.h              #   rmp::ui -- the public API and the Theme
│       ├── assets.h          #   rmp::assets -- load_*() by name, the counted handles
│       ├── tilemap.h         #   rmp::Tilemap -- a level designed in LDtk (or Tiled)
│       ├── audio.h           #   rmp::audio -- effects and music by name, three buses
│       ├── save.h            #   rmp::Value and rmp::save -- saving the game
│       ├── random.h          #   rmp::random -- seeded, reproducible
│       ├── ads.h             #   rmp::ads -- interstitial and rewarded ads; no-ops off Android
│       ├── math.h            #   vectors, rectangles, colours (raymath, raylib-cpp)
│       ├── config.h          #   the RMP_* values from the .toml
│       └── generated/        #   GENERATED config.h, git-ignored
├── examples/                 # one mini-project each, by area: scenes/ input/ ui/ ads/ assets/ games/ platform/
│   ├── games/                # seven whole games, playable start to finish; CI boots them all
│   └── plain_c/src/main.c    # the opt-out: plain C, <raylib.h> only, your own main()
├── tests/
│   ├── smoke_test.h          # CI boot + render hook (RAY_TEST_MAX_FRAMES, RAY_TEST_SCREENSHOT), header-only
│   ├── *_test.cpp            # doctest, no window: objects, collision, behaviors, scenes, input, assets, tilemap...
│   ├── ui_layout_test.cpp    # layout and hit-testing with no window (-DBUILD_UI_TESTS=ON)
│   ├── platformer_play.cpp   # a bot that plays examples/games/07_platformer to the end (examples job)
│   ├── configure_test.py     # every rejection of configure.py, and the repository's gates
│   ├── rmp_test.py           # rmp itself: every command, the launchers, rmp new, CI's game mode
│   └── fixtures/             # LDtk projects (four saved by LDtk itself), Tiled maps, an Aseprite sheet, an empty rres pack, licence trees
├── resources/                # Your assets (flat -- the pack does not recurse)
├── branding/icon.png         # the source for every app icon; rename it in [icon] source
├── tools/
│   ├── rmp.py                # `rmp`: every command, its help, the test stages, rmp new's manifest
│   ├── configure.py          # the config -> every build system. Run by CMake.
│   ├── license_db.py         # the licence guard, and LICENSES.txt (--check is the gate)
│   ├── android_release_check.py # the release APK's compile commands: optimised, RMP_PRODUCTION_BUILD=1
│   ├── *_check.sh            # the gates: versions, seam, portable, naming, headers, repo, workflows, licences
│   ├── lint.sh               # clang-tidy over src/, tests/ and examples/ (rmp lint, and CI)
│   ├── examples_build.sh     # builds and boots every example headless, with a screenshot each
│   ├── make_example_art.py   # the generated art the runner example brings
│   ├── dev_shell.sh          # run a command inside the pinned CI image
│   └── rres_pack.c           # open rres packer (AES-256) -- no paid tooling needed
├── cmake/
│   ├── configure_hook.cmake  # runs the generator before project()
│   ├── find_python.cmake     # the first Python on PATH that is 3.11+, asked, not assumed
│   ├── generated/            # GENERATED, git-ignored (LICENSES.txt lives here, per family)
│   ├── web/rmp_web.js        # --pre-js of every web build: IndexedDB for saves, key-press audio unlock
│   └── toolchain-riscv64-linux.cmake
├── raymob/                   # Android app shell (Gradle). See "Android (raymob)".
│   ├── generated.properties  # GENERATED, git-ignored
│   └── app/
│       ├── AndroidManifest.template.xml  # the manifest, with #if blocks
│       ├── generated/        # GENERATED manifest, git-ignored -- what Gradle reads
│       ├── proguard-rules.pro            # keeps every class the JNI code names
│       └── src/{admob,noadmob}/java/     # AdmobBridge: the real one, and the no-op
├── ios/                      # iOS scaffold. project.yml is GENERATED.
├── THIRD_PARTY_LICENSES.md   # every vendored component, its licence, our elections, what we altered
├── .github/
│   ├── workflows/ci.yml      # orchestrator: triggers, pins, job graph
│   ├── workflows/canary.yml  # weekly build against floating versions
│   ├── workflows/autofix.yml # agent dispatched onto the runner that broke
│   ├── workflows/_*.yml      # one reusable workflow per platform group
│   ├── known-breakage.md     # canary failures we have seen and chosen not to chase
│   └── scripts/              # web boot test, canary triage, upstream report
└── thirdparty/
    ├── raylib/               # raylib 6.0 -- MODIFIED, six patches + three upstream backports, see its PATCHES.md
    ├── raylib-ios/           # raylib-iOS fork (submodule) -- iOS only
    ├── raymob/               # raymob C sources (Android native bridge + admob) -- patched, see PATCHES.md
    ├── clay/                 # Clay -- the layout engine behind rmp::ui (zlib) -- one line patched
    ├── cute_aseprite/        # reads .aseprite directly (zlib OR public domain; we take the latter)
    ├── cute_tiled/           # reads Tiled JSON (same licence) -- patched, see PATCHES.md
    ├── raylib-cpp/           # the math subset only, behind rmp/math.h
    ├── rres/                 # rres.h + rres-raylib.h + externals (AES, Monocypher, QOI, LZ4)
    ├── cJSON/                # cJSON 1.7.19 (MIT), unmodified -- behind rmp::save and the LDtk reader, in no public header
    ├── doctest/              # the unit-test framework; never shipped
    └── FROZEN_VERSIONS.md    # every pin, machine-readable and CI-enforced
```

**Generated, and never committed** — `cmake/generated/`,
`include/rmp/generated/config.h`,
`raymob/generated.properties`, `raymob/app/generated/AndroidManifest.xml`, `ios/project.yml`,
`ios/Assets.xcassets/` and the Android `mipmap-*` icons. They are rebuilt from `raylib_multiplatform.toml` on every configure, which is
why they cannot drift out of sync with it. `cmake --preset debug` produces all of them; Gradle and
XcodeGen never invoke CMake, so those two jobs run `python3 tools/configure.py` explicitly.

`examples/` holds reference code for the framework's own features, grouped by namespace —
`ui/`, `ads/`, `assets/`, `platform/`, and `plain_c/` for the opt-out. They are **not** compiled
into your game; read them and copy what you need into `src/`. CI does syntax-check every one of
them with GCC and MSVC on each push, so they cannot quietly stop working.

`rmp` is the one command: `rmp run`, `rmp test`, `rmp build release`, `rmp web`, `rmp android`,
`rmp deploy`, `rmp new` and a few more. `rmp help` lists them on one screen, and
`rmp help <command>` explains one with examples. It is `tools/rmp.py`, one stdlib Python file,
behind three launchers -- `rmp` (POSIX sh: bash, zsh, ksh, dash), `rmp.ps1` and `rmp.cmd` -- that
only find a Python 3.11+ and hand it the arguments. In the framework it also has the maintainer's
commands (`rmp lint`, `rmp example`) and every gate as a stage of `rmp test`.

---

## Build system

raylib is linked **statically** (`raylib_static`), so you ship a single self-contained
binary. On Windows the compiler's own runtime goes in too: MinGW links with `-static`
(libstdc++, libgcc and winpthread inside the `.exe`) and MSVC with the static CRT (no VC++
Redistributable). What the `.exe` still imports is Windows itself -- `KERNEL32`, `USER32`,
`GDI32`, `SHELL32`, `WINMM` and the Universal C Runtime that is part of Windows 10 -- and the
Windows jobs fail on anything else: `tools/binary_check.py windows` reads the import table of
every `.exe` they ship, and the x64 boot runs with MinGW off `PATH`.

The macOS binary is universal and opens on **macOS 10.15** on Intel and **11.0** on Apple
silicon (the first macOS that ran there): `CMAKE_OSX_DEPLOYMENT_TARGET` in `CMakeLists.txt`,
which the build would otherwise take from the SDK -- macOS 26 on the runner. 10.15 is the floor
`std::filesystem` puts on Apple's libc++. The macos job reads every slice's minimum back out of
the binary (`tools/binary_check.py macos`) and fails on one above that.

**Presets** (`CMakePresets.json`), all Ninja-based:

| Preset | `PRODUCTION_BUILD` (CMake) → `RMP_PRODUCTION_BUILD` | `RMP_RESOURCES_PATH` | Notes |
|---|---|---|---|
| `debug` | `0` | absolute source path | run from anywhere |
| `release` | `1` | `"./resources/"` | LTO, ship `resources/` next to the exe |
| `web` | `1` | `"./resources/"` | Emscripten toolchain |

Each preset configures a folder of its own under `build/`: `build/debug`, `build/release`,
`build/web`, `build/memory` and `build/lint` (`BUILD_DIRS` in `tools/rmp.py` lists every folder
anything writes there). `build/` itself is never a build -- `CMakeLists.txt` refuses to configure
it -- so a release never inherits the `[dev] compiler` a debug configure wrote into its cache, and
switching between the two rebuilds nothing. A `CMakeCache.txt` left at the top of `build/` by an
older checkout is removed, once and with a message, by the first `rmp` command that builds.

**LTO** (`CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE`) is enabled for release on GCC/Clang and
MSVC. It yields smaller/faster binaries at the cost of longer link times. On NetBSD LTO is
disabled (its linker can't process LTO bytecode in static archives).

**Assembler view:** `cmake --build --preset debug --target assembler` emits `build/debug/main.s`
for the game sources (GCC/Clang) — handy for inspecting codegen.

---

## Editor / clangd (LSP)

The committed `.clangd` points clangd at the **host** compile database
(`build/debug/compile_commands.json`), so the project resolves with **no Android NDK**. Because no
Android target is forced, `__ANDROID__` is not defined, `<raymob.h>` is never processed, and no
NDK headers are needed. Configure once (`cmake --preset debug`) so the database exists.

Android intellisense is **opt-in** (it defines `__ANDROID__`, so `<raymob.h>` and the NDK headers
get indexed). It requires the NDK:

```bash
./generate_android_commands.sh   # writes raymob/.cxx/compile_commands/compile_commands.json
./update_clangd.sh               # rewrites .clangd to use that Android database
```

`git checkout -- .clangd` restores the host default. (`generate_android_commands.ps1` /
`update_clangd.ps1` are the Windows equivalents.)

---

## Compile-time definitions

| Macro | Value | Meaning |
|---|---|---|
| `RMP_RESOURCES_PATH` | absolute (dev) or `"./resources/"` (prod) | Asset folder path |
| `RMP_PRODUCTION_BUILD` | `0` / `1` | `#if RMP_PRODUCTION_BUILD` to strip debug code. Defined on every platform: CMake from its `PRODUCTION_BUILD` option, Android from the Gradle variant (release is `1`), iOS per Xcode configuration |
| `RMP_RRES_PASSWORD` | from `[resources]` | rres decryption password (see below) |
| `RMP_PROJECT_NAME`, `RMP_WINDOW_TITLE`, `RMP_WINDOW_WIDTH/HEIGHT` | from `[project]` / `[window]` | Your identity and design resolution |
| `RMP_WINDOW_VSYNC`, `RMP_WINDOW_FPS` | from `[window]` | Wait for the screen's refresh; cap the frame rate (0 = none, ignored on the web and iOS) |
| `RMP_UI_FONT`, `RMP_UI_FONT_SIZE`, `RMP_UI_SCALE`, `RMP_UI_MAX_ELEMENTS` | from `[ui]` | What `rmp::ui` starts with |
| `RMP_INPUT_DEADZONE` | from `[input]` | The stick travel that reads as zero |
| `RMP_AUDIO_MASTER`, `RMP_AUDIO_MUSIC`, `RMP_AUDIO_SFX` | from `[audio]` | What the three `rmp::audio` buses start at |
| `RMP_SAVE_PORTABLE`, `RMP_SAVE_ENCRYPT`, `RMP_SAVE_VERSION` | from `[save]` | Where saves go, whether they are sealed, the game's save-format version |
| `RMP_MAX_DELTA` | from `[app]` | The longest step the game logic is handed |
| `RMP_DEV_STRICT` | from `[dev]` | The first framework diagnostic aborts a debug build |

```cpp
Texture2D tex = LoadTexture(RMP_RESOURCES_PATH "player.png");  // raw path form
```

---

## Platform detection macros

Two families of macros are available, and they answer different questions:

- **`PLATFORM_*`** — set by the **build system** (raylib's CMake / the app shell). They tell you
  which **raylib backend** you are on. Prefer these for gameplay/rendering branches.
- **Compiler-predefined** (`__ANDROID__`, `_WIN32`, …) — always defined by the **compiler**, no
  setup needed. They tell you the **OS / CPU**. Use these for OS-specific code, and for the
  platforms raylib lumps together (the BSDs).

### raylib `PLATFORM_*` (set by the build)

| Macro | Defined when | Set by |
|---|---|---|
| `PLATFORM_DESKTOP` | Windows, Linux, macOS **and the BSDs** (desktop GLFW backend) | raylib CMake (propagates `PUBLIC` to the game) |
| `PLATFORM_WEB` | Web / Emscripten | `web` preset (`-DPLATFORM=Web`) → raylib CMake |
| `PLATFORM_ANDROID` | Android | raymob CMake (`-DPLATFORM=Android`) |
| `PLATFORM_IOS` | iOS | `ios/project.yml` (`GCC_PREPROCESSOR_DEFINITIONS`) |

Example — this is exactly how the entry-point macro picks the runner, and how
`include/rmp/app.h` pulls in `<raymob.h>`:

```cpp
#if defined(PLATFORM_IOS)
  // iOS: OS-driven frame callbacks (ios_ready/ios_update/ios_destroy)
#else
  // Desktop / BSD / Android / Web: blocking main() loop
#endif
```

> **Note:** there is **no per-BSD `PLATFORM_*`** — FreeBSD/OpenBSD/NetBSD all build as
> `PLATFORM_DESKTOP`. To single out a BSD (or Linux vs Windows vs macOS) use the compiler macros
> below.

### Compiler-predefined: OS

| Macro | Platform |
|---|---|
| `_WIN32` | Windows (32- and 64-bit; add `_WIN64` to require 64-bit) |
| `__APPLE__` | macOS **or** iOS (see note) |
| `__linux__` | Linux (also defined on Android — check `__ANDROID__` first) |
| `__ANDROID__` | Android (NDK). This is what `<raymob.h>`/`<admob.h>` test. |
| `__EMSCRIPTEN__` | Web / Emscripten |
| `__FreeBSD__` / `__OpenBSD__` / `__NetBSD__` | the three BSDs |

> **Apple note:** `__APPLE__` is set for both macOS and iOS. To split them, combine with the
> raylib macro: iOS defines `PLATFORM_IOS`, macOS builds are `PLATFORM_DESKTOP && __APPLE__`.
> (You can also include `<TargetConditionals.h>` and test `TARGET_OS_IPHONE`.)

### Compiler-predefined: CPU architecture

| Macro | Architecture |
|---|---|
| `__x86_64__` (GCC/Clang) / `_M_X64` (MSVC) | x86-64 |
| `__aarch64__` (GCC/Clang) / `_M_ARM64` (MSVC) | ARM64 |
| `__riscv` (with `__riscv_xlen == 64`) | RISC-V 64 |

### Which one do I use?

- **"Am I on mobile / web / desktop?"** → `PLATFORM_ANDROID` / `PLATFORM_WEB` / `PLATFORM_IOS` /
  `PLATFORM_DESKTOP`.
- **"Am I on Windows / Linux / a BSD?"** → compiler macros (`_WIN32`, `__linux__`, `__FreeBSD__`, …).
- **"Is this ARM64 vs x86-64?"** → architecture macros.
- For anything our own headers gate (raymob/admob), match them with `__ANDROID__`.

---

## Resources: `RMP_RESOURCES_PATH`, `rmp::assets` and rres

### The problem this solves

raylib loads assets from **paths**: `LoadTexture("resources/player.png")`. That is fine until you
ship. Then you have a folder of loose files next to the executable that anyone can open, replace or
copy; on Web every one of them is a separate HTTP request; and the path that worked from your IDE
(CWD = project root) does not work when the user double-clicks the binary (CWD = anywhere).

Two mechanisms handle that here, and they are independent.

### 1. `RMP_RESOURCES_PATH` — where "resources/" is

A compile-time string, defined in `CMakeLists.txt`:

| Build | Value |
|---|---|
| Development | absolute path to `<repo>/resources/` — run the binary from any CWD |
| `PRODUCTION_BUILD=ON` | `"./resources/"` — relative to the executable, which is how the packages are laid out |

Always build your paths from it (`rmp::assets::` does this for you). A bare `"resources/foo.png"`
works in dev and silently fails in a release.

`"./resources/"` is relative, and what makes it mean "next to the executable" is the entry point: a
desktop release (Windows, macOS, Linux and DRM, the BSDs) moves into the executable's folder before
it opens anything, when there is a `resources/` there. So a game started from a file manager, a
shortcut or another terminal folder finds its files, and so does a plain raylib
`LoadTexture(RMP_RESOURCES_PATH "x.png")`, pack or no pack. A release run from the source tree --
executable in `build/release/`, `resources/` at the root -- finds none next to itself and keeps the working
directory, as it always did. iOS moves into its bundle the same way; the web reads a virtual file
system and Android its APK, so neither moves. NetBSD and OpenBSD cannot say where an executable is
(raylib's `GetApplicationDirectory()` has no answer there): a release reads `resources/` from the
working directory and logs that it does. `tools/shipped_check.sh` starts a release from another
folder, and the Linux, Windows, macOS and BSD jobs all do.

### 2. rres — one file instead of a folder

[rres](https://github.com/raysan5/rres) is raysan's resource-container format: a header, N data
chunks, and a central directory mapping names to chunk ids. This framework ships its own packer,
`tools/rres_pack.c`, built as a CMake target:

```bash
rmp pack      # cmake --build --preset debug --target pack_resources: resources/ -> resources.rres
rmp unpack    # delete the pack, go back to loose files
```

**You do not need [rrespacker](https://raylibtech.itch.io/rrespacker).** That is raysan's paid GUI
for authoring containers by hand — picking per-resource compression, previewing, editing an existing
`.rres`. The packer here does the one thing a build needs, non-interactively, in CI, with no
licence: walk `resources/`, and write every file into the container.

What it writes, per file:

| Field | Value |
|---|---|
| chunk type | `RAWD` (raw data) — *every* file, whatever it is |
| id | `CRC32(relative filename)` — this is why lookup is by name |
| props | `[size, ext[0..3], ext[4..7], 0]` — the original extension travels **inside** the chunk |
| compression | `RRES_COMP_NONE` |
| cipher | AES-256-CTR, key = Argon2i(password, salt), + MD5 of the plaintext |

Then a `CDIR` chunk holds `(id, filename)` for every entry, unencrypted, so `rmp::assets::` can resolve
a name to an id without knowing the password up front.

The packer emits standard rres containers, so the official rrespacker can open them — but nothing
in this framework requires it.

### Why `rmp::assets::init()` exists

`rmp::assets::init()` (called for you by `RMP_ENTRY_POINT`, before `on_ready()`) does
four things that have to happen exactly once:

1. **Decides which mode you are in.** It looks for `resources/resources.rres`. Found → pack mode.
   Not found → loose-file mode. This is why packing needs no code change: the same
   `rmp::assets::load_texture("player.png")` call works both ways, and you can iterate all day on loose
   files and pack only when you build a release.
2. **Loads the central directory into memory**, once. Without it there is no name→id mapping, and
   the ids are CRC32 hashes — not something you can compute by looking at the container. Reloading
   the CDIR per asset would mean re-reading and re-parsing the file header on every load.
3. **Installs the cipher password** (`rresSetCipherPassword`), which rres keeps as a global.
4. **Routes raylib's own file loading through the pack**, via `SetLoadFileDataCallback` and
   `SetLoadFileTextCallback` — but only in pack mode, so a development build is untouched.

Point 4 is what makes plain raylib work:

```cpp
Texture2D t = LoadTexture(RMP_RESOURCES_PATH "player.png");   // reads the pack
Model     m = LoadModel(RMP_RESOURCES_PATH "ship.obj");       // and so does this
```

It matters more than it looks. Without the hook, `LoadTexture(RMP_RESOURCES_PATH "x.png")` works
perfectly in development and comes back `0x0` in a release — because a release ships
`resources.rres` and not the loose files. Nothing warns you; the texture is just blank. Anyone who
had not read this page would write that line, and it would be the last thing they suspected.

Three details make the hook safe rather than clever:

- **raylib returns the callback's result verbatim.** `rcore.c` reads
  `if (loadFileData) return loadFileData(fileName, dataSize);` — there is no fallback to the
  filesystem behind it. So a miss has to delegate by hand, and the exact way to delegate is to
  *unhook, call raylib, hook back*. That runs raylib's own reader, including the Android one, where
  `fopen` is redirected into the APK's asset manager and stdio of our own would find nothing.
- **It only answers for files under `RMP_RESOURCES_PATH`.** The pack is keyed by bare file name, so
  matching on the name alone would let a save file called `level1.json` anywhere on disk be
  answered with the packed `level1.json`.
- **Models come along for free.** `rmodels.c` loads `.obj` with `LoadFileText` and
  `.gltf`/`.glb`/`.iqm`/`.vox`/`.m3d` with `LoadFileData`, and it resolves the sibling files an
  `.obj` or `.gltf` refers to — the `.mtl`, the `.bin` buffers, the textures named inside the
  material — through the same two functions. Hooking those two hooks the whole chain.

`rmp::assets::shutdown()` unhooks and frees the directory. Calling `rmp::assets::init()` twice is a
no-op; the entry point calls it for you.

### What you actually get

```cpp
rmp::Texture     rmp::assets::load_texture(std::string_view name);
rmp::Image       rmp::assets::load_image(std::string_view name);
rmp::Sound       rmp::assets::load_sound(std::string_view name);
rmp::Font        rmp::assets::load_font(std::string_view name, int font_size);
rmp::SpriteSheet rmp::assets::load_sheet(std::string_view name);
rmp::Tilemap     rmp::assets::load_map(std::string_view name);   // and (name, level)
std::vector<unsigned char> rmp::assets::load_data(std::string_view name);   // frees itself
bool             rmp::assets::using_pack();
```

The resources are counted and release themselves (see `include/rmp/assets.h`): nothing to unload,
and the same name twice is the same resource.

`name` is the **bare file name** — `rmp::assets::load_sound("jump.wav")`. Not a path. `tools/rres_pack.c`
stores every entry under its basename and the CMake glob does not recurse, so `resources/sfx/` is
not packed at all and there is nothing for a nested name to resolve to. See the subfolder note
below.

Every one of them has the same shape: if in pack mode, look the name up, decrypt, decode from
memory; if anything fails, **log a warning and fall back to the loose file**. A missing entry
degrades instead of crashing, and the warning names the asset. What comes back empty -- nothing in
the pack or on disk, or a file that is there and does not decode -- counts in
`rmp::assets::failed_loads()`, which every CI boot requires to be 0.

### What happens to each kind of file in `resources/`

Everything directly in the folder is packed — the packer does not filter by type:

| You put in `resources/` | Load it with | Notes |
|---|---|---|
| `.png .jpg .bmp .tga .gif .qoi .dds .ktx .hdr` | `rmp::assets::load_texture` / `LoadImage`, or plain `LoadTexture` | the extension in props tells raylib which decoder to use |
| `.wav .ogg .mp3 .qoa` | `rmp::audio::play("name")`, or `rmp::assets::load_sound` | short sounds; fully decoded into RAM |
| `.ttf .otf` | `rmp::assets::load_font(name, size)` | size is baked at load time, as always in raylib |
| `.obj .mtl .gltf .glb .bin .iqm .vox .m3d` | plain `LoadModel(RMP_RESOURCES_PATH "…")` | works through the loader hook, siblings included — keep them all directly in `resources/` |
| `.vs .fs .glsl` | plain `LoadShader(RMP_RESOURCES_PATH "…")` | also hooked; `LoadShaderFromMemory` if you prefer |
| `.json .txt .csv` and anything else | `rmp::assets::load_data` | you get the bytes |
| **music** (`.ogg .mp3 .wav .qoa`, and `.xm .mod` by full name) | `rmp::audio::music("name")` | streamed, from the pack too; plain `LoadMusicStream` is the exception, see below |

### The two things that stay outside the pack

**Music through raylib directly.** `LoadMusicStream` is path-only by design, and unlike the model
loaders it does not go through `LoadFileData`: `raudio.c` hands the file name straight to
`drwav_init_file`, `drmp3_init_file` or `jar_xm_create_context_from_file`, which open them
themselves, so there is nothing for the hook to intercept and a packed song is invisible to it.
`rmp::audio::music()` does not have that problem: it reads the song through `load_data`, which
serves the pack and the loose files alike, and streams it with `LoadMusicStreamFromMemory`. The
price is that the *encoded* file stays in memory while it plays — a three-minute OGG is about 3 MB
— and the decoding is still streamed, so the 30 MB of PCM it would decode to never exists at once.
Only code that calls `LoadMusicStream` itself needs the song as a loose file.

**Subfolders.** `file(GLOB …)` in `CMakeLists.txt` does not recurse and resource names are flat, so
nothing inside `resources/art/` is packed.

For both: the files have to actually ship, and **the release packages are not uniform**:

| Target | Packs? | What ships in the package |
|---|---|---|
| Linux x64/arm64 (glibc, and DRM), Windows x64, macOS, the BSDs | yes | **`resources.rres` only** — nothing else is copied in |
| Linux musl and riscv64, Windows arm64 | no | the whole `resources/` folder, and a `PACK_SKIPPED.txt` saying why: the packer is built for the target and cannot run on the machine that cross-compiles it |
| Web | no | the whole folder, preloaded into the Emscripten virtual FS |
| Android | no | the whole folder, copied into `assets/` (minus any `.rres`) |
| iOS | no | the whole folder, as a folder reference inside the `.app` |

Only the first row runs `pack_resources` -- the BSDs inside their VM, where the packer is a native
binary -- which has a consequence worth saying out loud: **the encryption applies to the first row
and no other.** On musl, riscv64, Windows arm64, Web, Android and iOS your assets ship as ordinary
files that anyone can open. If that matters, pack them yourself in the job that builds those
targets — but see the Android note below before you try it there.

So an un-packable file works in development, works on Web and Android — and is missing only in the
packaged desktop builds of the first row. That is the worst possible failure mode, so
`CMakeLists.txt` emits a `WARNING` at configure time when `PRODUCTION_BUILD=ON` and it finds a
subfolder in `resources/`. If you need one anyway, extend the `package/` step in
`.github/workflows/_linux.yml`, `_windows.yml`, `_apple.yml` and `_bsd.yml` to copy it alongside the
pack.

> The pack is also never used on **Android**, whatever you do. rres opens the container with plain
> `fopen`, and inside an APK there is no such file — raylib reaches its assets through
> `AAssetManager`, which only its own internal `fopen` redirect knows about. `FileExists` returns
> false, `rmp::assets::init()` reports loose-file mode, and the loose files are there because the
> Android job copies the whole folder in. Nothing breaks; it just means `[resources]
> rres_password` buys you nothing on that one platform.

### Encryption

Password: `[resources] rres_password` in `raylib_multiplatform.toml`, which reaches both sides from
one place — `RRES_PACK_PASSWORD` for the packer and `RMP_RRES_PASSWORD` in
`include/rmp/generated/config.h` for the game. (They used to be two hardcoded literals in
`CMakeLists.txt` and the asset layer; desynchronising them broke loading at runtime only.)

> **This is obfuscation, not security.** The password is a string inside a binary you hand to the
> player; anyone determined will find it. What it does buy is that assets are not casually
> extractable by dragging the folder open. Cost is *load-time* only — Argon2i (16 MiB, 3 passes)
> per resource, nothing per frame.

---

## Scenes, objects and the frame

Documented on the site: [Scenes](https://omardev29.github.io/rmp-docs/manual/core/scenes.html), [the app](https://omardev29.github.io/rmp-docs/manual/core/app.html) and [one frame](https://omardev29.github.io/rmp-docs/framework/the-frame.html); why the web has no `while` loop: [Web](https://omardev29.github.io/rmp-docs/manual/platforms/web.html).

## `rmp::ui` — notes for maintainers

How to use it is on the site: [Interface](https://omardev29.github.io/rmp-docs/manual/modules/ui.html), [layout](https://omardev29.github.io/rmp-docs/manual/modules/ui-layout.html), [controls](https://omardev29.github.io/rmp-docs/manual/modules/ui-controls.html), [style](https://omardev29.github.io/rmp-docs/manual/modules/ui-style.html) and [Clay directly](https://omardev29.github.io/rmp-docs/manual/raylib/clay.html). What stays here is why it is built the way it is.

### Strings are copied

`std::string_view` accepts a literal, a `std::string`, or a temporary — and the text is copied into
a per-frame arena the moment you pass it. So this is correct:

```cpp
rmp::ui::text(std::to_string(score));
```

It matters because the layout engine underneath keeps pointers to text and reads them later, during
`end()`. Without the copy that temporary would be long gone, and the bug would be the kind that
only shows up in a release build. There is nothing to remember here; it is written down because
the absence of a rule is worth knowing about.

### Why one frame behind

When `button("Play")` has to return true or false, this frame's layout does not exist yet — it is
computed in `end()`. So the hit test uses the rectangle that button had **last** frame.

Two consequences, both acceptable: the first frame a button appears it cannot be clicked (16 ms at
60 fps), and while the interface is moving the sensitive area trails what you see by one frame.

The engine documents a fix — run the layout twice per frame — and it is **deliberately not used**.
It would mean running your code twice, so a `score++` inside the UI block would count double. One
frame of lag is far cheaper than that class of surprise.

### What is underneath, and how to use it directly

The layout engine is [Clay](https://github.com/nicbarker/clay), vendored at `thirdparty/clay/`
under the zlib licence. It is held to one rule: **no Clay type, macro or enum appears anywhere
under `include/`.** The public API takes `std::string_view` and raylib's own types, so code written
against `rmp::ui` does not depend on Clay and the engine can be replaced without that code
changing.

**That rule is about what you are handed by default, not about what you are allowed to do.** Clay
is on the include path, and using it directly is supported:

```cpp
#include <clay.h>

rmp::ui::begin();
rmp::ui::text("Inventory");

CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_FIT(0) },
                           .childGap = 10,
                           .layoutDirection = CLAY_LEFT_TO_RIGHT },
               .backgroundColor = { 30, 30, 38, 255 },
               .cornerRadius = CLAY_CORNER_RADIUS(10) }) {
    CLAY(CLAY_IDI("slot", 0), { .layout = { .sizing = { .width = CLAY_SIZING_FIXED(64) } } }) {}
}

if (rmp::ui::button("Close")) rmp::app::quit();
rmp::ui::end();
```

Your elements join the same tree, are laid out in the same pass and drawn by the same renderer. It
is the same bargain as everywhere else in this framework: `rmp::assets` does not stop you calling
`LoadTexture`, and `rmp::ui` does not stop you calling Clay — or rlgl, or raw OpenGL.

The full worked version, including images and hover, is
[`examples/ui/03_clay_direct/src/main.cpp`](examples/ui/03_clay_direct/src/main.cpp). CI compiles
every example with GCC **and MSVC** on every run, so that claim cannot quietly stop being true.

Three things to know before you do it:

- **`begin()` has already opened two elements** — a root that fills the screen and a column that
  centres its contents. Yours become children of that column. Clay's floating elements are the way
  out of it.
- **Clay does not copy strings.** `CLAY_STRING()` on a literal is fine forever; anything built at
  runtime has to stay alive until `end()` returns. This is the one footgun `rmp::ui::text()`
  removes by copying.
- **It pins you to Clay 0.14.** Clay is pre-1.0 and its API has moved between minor versions. That
  is a fair trade for a feature you need today and a bad one for a button, which is most of why
  `rmp::ui` exists at all.

Our renderer handles `RECTANGLE` and `BORDER` (each corner with its own radius, each side of a
border with its own width), `TEXT`, `IMAGE` (point `imageData` at a `Texture2D` you own; a tint
is a `Color *` in `userData`, and a `backgroundColor` is not one — Clay draws it as a rectangle
after the image, over it) and the `SCISSOR` pair, so clipping and scroll containers work.
`CUSTOM` is not handled — `src/rmp/ui/render.cpp` is one short file, and adding a case is the
intended way to extend it.

**Why `rmp::ui` does not use the macros internally**, since it is a fair question: `CLAY(...)` is a
`for`-loop block, and this API is a `begin()`/`end()` pair. An element opened inside a block macro
cannot stay open across a function boundary. So `widgets.cpp` calls `Clay__OpenElement`,
`Clay__ConfigureOpenElement` and `Clay__CloseElement` — all public — and fills the structs field by
field, which is also what you want when the values are computed from the Theme and the scale rather
than written as literals. Nothing about the macros is being avoided; they simply do not fit the
shape of this particular API.

The implementation is ten files under `src/rmp/ui/`: `clay_impl.cpp` (compiles the engine once,
same idea as `rres_impl.cpp`), `internal.h` (the private header, which is where Clay is
included), `context.cpp` (start-up, scale, font, the text arena, element identity, hit testing, the
test seams), `widgets.cpp` (`begin`, `end`, `button`, `text`), `containers.cpp` (the containers,
`image`, `progress`, `grid`, `scroll`), `controls.cpp` (`checkbox`, `slider`, `dropdown`,
`text_input`), `focus.cpp` (focus, keyboard and gamepad navigation), `style.cpp` (sizes, state
colours, transitions), `render.cpp` (draw commands into raylib calls) and `theme.cpp`.

Two details from that boundary that are worth knowing:

- **Element identity.** Elements are identified by their label, which would make two "Back" buttons
  in two different screens the same element — hover one, both light up. Ids carry an occurrence
  counter, so identical labels in one frame are told apart automatically. When the UI is
  conditional and elements come and go, pass an explicit `.id`.
- **Start-up and shutdown.** The UI starts itself on the first `begin()`, because
  `rmp::assets::init()` runs before `InitWindow()` and a font is a GPU texture. It shuts down
  *before* `on_exit()`, because `on_exit()` is where you call `CloseWindow()` and releasing a font
  after that is touching a context that no longer exists. Neither is yours to call.

### Testing layout without a window

`tests/ui_layout_test.cpp` runs the real `begin/button/text/end` with the text measurement, the
pointer and the viewport injected. No window, no GL context, no display:

```bash
cmake --preset debug -DBUILD_UI_TESTS=ON
cmake --build --preset debug --target ui_layout_test && ./build/debug/ui_layout_test
```

It checks the menu is centred at four resolutions, that buttons do not overlap and share a width,
that none is shorter than `min_touch_size`, that nothing leaves the screen, and that the scale
clamps at both ends. Those are the things a person verifies once by hand and then never again.

## `rmp::app`

Documented on the site: [The app](https://omardev29.github.io/rmp-docs/manual/core/app.html), [why quit() returns](https://omardev29.github.io/rmp-docs/framework/why/no-exceptions.html) and [iOS](https://omardev29.github.io/rmp-docs/manual/platforms/ios.html).

## `rmp::audio`

Documented on the site: [Audio](https://omardev29.github.io/rmp-docs/manual/modules/audio.html).

## `rmp::Camera`

Documented on the site: [Camera](https://omardev29.github.io/rmp-docs/manual/core/camera.html).

## `rmp::Tilemap`

Documented on the site: [Tilemaps](https://omardev29.github.io/rmp-docs/manual/modules/tilemaps.html).

## `rmp::save` — what is tested

How to use it is on the site: [Saving](https://omardev29.github.io/rmp-docs/manual/modules/saving.html).

**What is tested, and what is not.** `tests/save_test.cpp` runs on Linux in `rmp test` and in the
CI `lint` job: the Value and its Ref, the file format attacked byte by byte, sealing, the portable
fallback across two sessions, the XDG folders, and numbers under a comma and a two-byte decimal
point (locales built from `tests/fixtures/locale/` by `tools/test_locales.sh`, required in CI).
What only another platform can run is covered by the boot instead: under the CI smoke test every
game saves a small file to its platform's real folder, reads it back and removes it before it may
print `RAY_TEST_BOOT_OK` (`after_ready()` in `src/rmp/app.cpp`). Every target CI actually boots
does it: Windows (with `%APPDATA%` set to a folder called "José Müller"), macOS, Linux on glibc,
musl and DRM, the three BSDs, every example, and the web, whose boot test opens the page with
`?ray_test_save=1` and requires the save to have gone through IndexedDB. **Two families are not
covered at run time:** iOS, because the hosted simulator does not boot and the job only builds;
and Android, because the Firebase job is a Robo crawl in Test Lab that proves the app starts
and does not crash, with no log to read a marker from. Their folder code (Application Support,
internal storage) is only reviewed, not run. Nor does any job check that a web save survives a
real page reload.

## AdMob (Android)

Interstitial + rewarded ads. The API is `rmp::ads`, and it arrives with
`<rmp/ads.h>` — **real on Android**, a **no-op on every other platform**, so no
`#ifdef`s in your game code.

| Function | Purpose |
|---|---|
| `rmp::ads::request_interstitial()` / `is_interstitial_loaded()` / `show_interstitial()` | interstitial |
| `rmp::ads::request_rewarded()` / `is_rewarded_loaded()` / `show_rewarded()` | rewarded |
| `rmp::ads::take_reward_earned()` (true once, then clears) + `reward_amount()` | poll the reward |

They are defined out of line, in `src/rmp/ads.cpp`, over the C functions in `<admob.h>` -- the one
file that includes it, so a game that shows no ad never compiles the JNI bridge's header and
`rmp/ads.h` includes nothing of raymob's. `<admob.h>` stays where it is: it is the real JNI boundary,
and the pure-C entry point has no namespaces to call into.

Configuration is in `raylib_multiplatform.toml`:

```toml
[android.admob]
enabled         = true                   # off by default; a game made with rmp new has false
app_id          = "ca-app-pub-...~..."   # AdMob application id (goes into the manifest)
interstitial_id = "ca-app-pub-.../..."   # interstitial ad unit
rewarded_id     = "ca-app-pub-.../..."   # rewarded ad unit
```

The ids default to **Google's official test ids**; replace them before publishing. The app id is
injected into the manifest, the ad units are exposed to Java via `BuildConfig`. Ads are
**Android-only**; iOS ads are not implemented.

### Where it actually lives

Five steps, and each one exists because the one above it cannot do its job:

| Layer | File | What it does |
|---|---|---|
| API | `include/rmp/ads.h`, `src/rmp/ads.cpp` | the eight functions of `rmp::ads`, each one call into `admob.h` |
| C | `thirdparty/raymob/admob.h` | the eight C functions. Real on Android, inline no-ops elsewhere |
| JNI | `thirdparty/raymob/admob.c` | calls the methods **by name** on the Activity instance |
| Java | `raymob/.../NativeLoader.java` | the eight public methods, kept by `proguard-rules.pro` |
| SDK | `raymob/app/src/{admob,noadmob}/java/.../AdmobBridge.java` | Google Mobile Ads, or nothing |

`NativeLoader` only forwards. The work is in `AdmobBridge`, which exists **twice** — the real one
under `src/admob/java`, a no-op twin under `src/noadmob/java` — and Gradle puts exactly one of them
on the source path. That split is what makes the switch real rather than cosmetic.

### Switching it off

`enabled = false` removes AdMob from the build: no `play-services-ads` dependency, no `AD_ID`
permission, no `APPLICATION_ID` meta-data, no `MobileAds.initialize()` at startup. Your game code
does not change — `rmp::ads` keeps compiling and the calls do nothing, exactly as they already do
on desktop.

It also **turns itself off when `android` is not in `[targets]`**, however you removed it: by name,
or by dropping a group like `mobile`. Ads in a build that does not exist would still have cost you
the SDK, the permission and a Play declaration.

That permission is the reason this is worth a switch. `com.google.android.gms.permission.AD_ID`
obliges you to declare advertising-id collection in Play's **Data safety** form, and a game that
shows no ads should not have to answer for it.

> **Not done yet: consent (UMP).** Serving ads to users in the EEA or the UK requires a
> Google-certified CMP — in practice the [User Messaging Platform
> SDK](https://developers.google.com/admob/android/privacy) — since January 2024. This framework
> does **not** ship one. With ads on, expect EEA/UK traffic to be served badly or not at all until
> you add it: `com.google.android.gms:play-services-ads` already contains UMP, so it is a
> `ConsentInformation.requestConsentInfoUpdate()` call in `AdmobBridge.initialize()` plus a form,
> not a new dependency.

---

## Web export

Emscripten. Prereq: install & activate the [emsdk](https://emscripten.org/), and make sure the
`EMSDK` env var / `Emscripten.cmake` toolchain is reachable (the `web` preset references it).

```bash
cmake --preset web
cmake --build --preset web      # -> build/web/{ray_test.html,.js,.wasm,.data}
```

Test locally with a local HTTP server (opening the HTML directly won't work):

```bash
python -m http.server 8000 --directory build/web   # then open http://localhost:8000/<name>.html
```

Memory: the web build preloads `resources/` and allocates `TOTAL_MEMORY` (default 67 MB). Raise
`-s TOTAL_MEMORY=` in `CMakeLists.txt` if your game needs more (larger = longer load).

---

## Android (raymob)

The Android app shell lives in `raymob/` (Gradle + raymob). To build locally:

1. Open `raymob/` in Android Studio (it installs missing deps).
2. Install CMake 3.30.3 inside Android Studio and Java 11+.
3. `cd raymob && ./gradlew assembleDebug` (APK) or `./gradlew bundleRelease` (AAB).

`raymob.h` gives native Android access (vibrate, soft keyboard, sensors). It expands to nothing
off-Android, so you can `#include <raymob.h>` unconditionally:

```cpp
#include <raymob.h>
...
#ifdef __ANDROID__
  Vibrate(2);
#endif
```

App identity comes from `raylib_multiplatform.toml` (`[project] name`, `[window] title`,
`[android] application_id`). `tools/configure.py` writes it to `raymob/generated.properties`,
`settings.gradle` loads that and injects it as Gradle `ext`, and the build renames the Java package
and native library accordingly.

`raymob/gradle.properties` still exists but holds only Gradle/AGP infrastructure — `jvmargs`,
`useAndroidX`. Those have to be in that file because Gradle reads it during initialisation, before
`settings.gradle` is even evaluated and before the daemon JVM starts, so nothing generated exists
early enough. Everything about your game moved out.

### Publishing to Google Play (signed AAB)

Google Play requires a **signed Android App Bundle (AAB)** — not an APK, and not the debug
build. The framework produces it via `./gradlew bundleRelease` plus an env-driven signing
config, so **no secret ever lives in the repo**.

1. **Create an upload keystore** (once; keep it safe. Enable *Play App Signing* in the Play
   Console so Google can reset it if you ever lose it):

   ```bash
   keytool -genkey -v -keystore upload-keystore.jks -keyalg RSA -keysize 2048 \
           -validity 10000 -alias upload
   ```

2. **Build the signed AAB.** Export these variables, then run the bundle task:

   | Env var | Value |
   |---|---|
   | `ANDROID_KEYSTORE_FILE` | path to `upload-keystore.jks` |
   | `ANDROID_KEYSTORE_PASSWORD` | keystore password |
   | `ANDROID_KEY_ALIAS` | key alias (`upload` above) |
   | `ANDROID_KEY_PASSWORD` | key password |

   ```bash
   cd raymob && ./gradlew bundleRelease
   # -> raymob/app/build/outputs/bundle/release/*.aab
   ```

   Without `ANDROID_KEYSTORE_FILE` the release build is unsigned (fine for a local check,
   rejected by Play).

3. **CI.** The AAB is built, signed and verified on **every** run, secrets or not — the
   bundle is the one artifact you cannot afford to first test on release day. What changes is
   *which key* signs it:

   | | Key | Artifact | Attached to the Release? |
   |---|---|---|---|
   | No secrets set (the default) | throwaway key generated in the job | `android-unsigned-aab` → `*-NOT-FOR-PLAY.aab` | **No** |
   | Secrets set | your upload key | `android-release-aab` | Yes |

   To switch to the second row, add four **repository secrets** with those exact names
   (*Settings → Secrets and variables → Actions*), where `ANDROID_KEYSTORE_BASE64` is
   `base64 -w0 upload-keystore.jks`.

   Verification is `jarsigner -verify -strict` plus an assertion on the signer's CN, checked
   both ways: a run configured with a real key fails if the bundle turns out to carry the
   throwaway signature, and vice versa. (`unzip -l | grep META-INF/.*\.SF`, which the build
   used to do, only proves that *a* signature exists — not whose, and not that it verifies.)

> **Never commit the keystore or its passwords.** And before publishing, replace the AdMob
> test ids and set your real `application_id` in `raylib_multiplatform.toml`. CI refuses to build
> a tag while the id is still `com.example.*`, because a Google Play application id can never be
> changed once published.

---

## iOS

iOS uses the **`raylib-iOS` fork** (submodule `thirdparty/raylib-ios`, frozen at tag `6.0.3-iOS`)
because upstream raylib has no iOS backend. The app scaffold is in `ios/` (XcodeGen `project.yml`).

- The fork builds `raylib.xcframework` (device + simulator) via
  `thirdparty/raylib-ios/projects/scripts/build-ios-xcframework.sh`.
- Graphics go through **ANGLE** (OpenGL ES → Metal), bundled in the fork.
- The game's code is the same: `RMP_GAME` expands to `RMP_IOS_FUNCS` there, which defines the
  `ios_ready/ios_update/ios_destroy` callbacks the fork's UIKit backend calls.
- Building requires **macOS + Xcode**. CI's `ios` job in `_apple.yml` builds the xcframework and
  the app for the simulator, unsigned; `ios/README.md` has the steps by hand. iOS ads are not
  implemented.

---

## BSD & RISC-V targets

- **Linux RISC-V**: cross-compiled inside the CI build image using
  `cmake/toolchain-riscv64-linux.cmake` + the image's `riscv64-linux-gnu` toolchain and riscv64
  X11/GL multiarch libraries.
- **FreeBSD / OpenBSD / NetBSD**: built inside QEMU VMs via
  [`cross-platform-actions/action`](https://github.com/cross-platform-actions/action)
  (pinned by SHA). The action syncs the workspace into the VM, builds there into
  `build/release/`, and syncs `build/` back so the artifact can be uploaded.
  - NetBSD uses `pkg_add` with `PKG_PATH` pointed at the pkgsrc binary packages and builds with
    GNU make (`Unix Makefiles`), because pkgsrc's `ninja` package is an IRC client, not the build
    tool. NetBSD **arm64** is excluded (unresolvable pkgsrc/base version conflicts).
  - **FreeBSD riscv64** and **OpenBSD/NetBSD riscv64** are not built: no riscv64 packages/disk
    images exist for them. RISC-V is covered by Linux RISC-V.

---

## CI/CD pipeline

### Layout

One orchestrator plus one reusable workflow per platform group. `ci.yml` is the only file you
normally read or edit.

| File | What it does |
|---|---|
| `ci.yml` | Triggers, the fast-lane/full decision, the build-image pin, and the job graph |
| `_linux.yml` | x64, ARM64, RISC-V (in the frozen image) |
| `_web.yml` | Emscripten build + headless-Chromium boot/render test |
| `_windows.yml` | x64 (native, real render test) and ARM64 (MSVC cross) |
| `_apple.yml` | macOS universal binary + iOS xcframework, app and simulator test |
| `_android.yml` | Debug APK + release AAB |
| `_bsd.yml` | FreeBSD / OpenBSD / NetBSD in QEMU VMs |
| `_release.yml` | `SHA256SUMS` + GitHub Release |
| `_itch.yml` | itch.io upload with butler |
| `_firebase.yml` | Firebase Test Lab robo test on a real Android device |

Three things bite everyone who splits workflows this way, so they are called out in comments
in `ci.yml` too: workflow-level `env` does **not** cross the `workflow_call` boundary (hence
`project_name` as an input); called workflows receive no secrets without `secrets: inherit`;
and `concurrency` must live only in the orchestrator or parent and children cancel each other.

### The framework, and every game made from it

A game made with `rmp new` carries the same `ci.yml`, byte for byte, and the same reusable
workflows. The `config` job asks `python3 tools/rmp.py --mode` which one it is running in --
the framework has `tests/configure_test.py`, a game does not -- and stops the run on any answer
but `framework` or `game`. What only the framework has is skipped by that answer, never cut out
of a copy: the framework's own steps in `lint` (its tests and its gates over its own sources),
the `examples` job, the MSVC pass over `examples/` in `_windows.yml`, and `rmp_new`. A game's
`lint` job is actionlint, its configuration, the pinned versions, the licences and a frame drawn
in software. `release` accepts those jobs as skipped only in a game.

`rmp_new` is the framework's check on what a game gets: it makes a game from the commit under
test, runs actionlint and `tools/workflow_check.sh` on the game's workflows, and the game's own
`rmp test config`, `rmp test render` (no golden frame: the boot, the pixels, a clean exit) and
`rmp test smoke`. The launchers run where Linux cannot: `rmp.ps1` and `rmp.cmd` in a Windows job
of their own (which also makes a game there), `zsh` and `/bin/bash` 3.2 in the macOS job, and
OpenBSD's `ksh` in its BSD job.

### When it runs

| Trigger | What builds |
|---|---|
| push to `main`, pull request | **Fast lane** — Linux x64, Web, Android, Windows x64, plus the `lint`, `examples` and `rmp_new` jobs (~10 min) |
| tag `v*` | All 17 targets, then Release, then itch.io |
| `workflow_dispatch` | Fast lane, or everything with the `full` input |

The full matrix costs about an hour of BSD QEMU and macOS runner time. Paying that per commit
buys either a slow loop or, worse, a team that has learned to ignore CI.

`workflow_dispatch` also takes `image_digest`, so you can smoke-test a candidate build image
without committing the pin.

### What is pinned, and how

- **Linux/Web/Android jobs run inside a frozen Docker image**, referenced by **digest**
  (`ghcr.io/omardev29/raylib-build@sha256:…`), never by `:latest`. A tag can be moved; a digest
  cannot. The image is built from its [own repo](https://github.com/omardev29/raylib-build-image),
  where the base image is digest-pinned, every apt source points at an **Ubuntu snapshot
  timestamp**, every download is sha256-verified, and emsdk is fetched by commit rather than
  tag. Every container job prints `/etc/raylib-build-image.json` as its first step, so a
  failing log always states which toolchain produced it.
- **Runner labels are pinned** (`ubuntu-24.04`, `windows-2025`, `macos-26`) — never `-latest`,
  which moves. On macOS the Xcode version is also selected explicitly, with a guard that lists
  what is installed and fails loudly if the pin is gone.
- **No `brew install` at job time.** Ninja and XcodeGen are pinned release binaries with
  checksums; brew formulae are unversioned and raise their macOS requirements on Homebrew's
  schedule, not yours.
- **Every action is pinned by full commit SHA**, with the tag it resolves to in a comment.
- `tools/versions_check.sh` runs in CI and fails if any of this has drifted apart. See
  [`thirdparty/FROZEN_VERSIONS.md`](thirdparty/FROZEN_VERSIONS.md).

The honest exception is **BSD**. The QEMU disk images and the `pkg`/`pkgsrc` mirrors are both
rolling, and neither project runs a snapshot service, so those three jobs genuinely install
whatever the mirror is serving that day. If a BSD job starts failing for no reason you changed,
that is the first thing to suspect. The one lever available is switching FreeBSD's repository
from `latest` to `quarterly` (edit `/usr/local/etc/pkg/repos/` inside the VM step), which moves
four times a year instead of continuously — it is not pinning, just a slower drift, so it is
left off by default rather than pretending otherwise.

### The canary, and the thing that fixes itself

Everything above is pinned, which stops the world from breaking your build — and also stops you
from finding out that the world moved. A green pipeline in six months means nothing if it is green
against Xcode 26.6 while everyone else is on 28.

> **One repository setting has to be on, or the agent cannot finish.**
> *Settings → Actions → General → Workflow permissions → "Allow GitHub Actions to create and
> approve pull requests."* It is off by default, and while it is off `GITHUB_TOKEN` is refused the
> `createPullRequest` GraphQL mutation no matter what `permissions:` the workflow asks for. The
> push succeeds and the PR does not, so the agent does all the work and leaves a branch nobody is
> looking at. Measured with a probe job holding exactly the permissions `autofix.yml` grants:
> `pull request create failed: GraphQL: GitHub Actions is not permitted to create or approve pull
> requests`. The *Did the agent produce anything?* step now names this specifically when it sees a
> branch with no PR behind it.

> This is **upstream infrastructure, not something a game built on this runs.** Every job in
> `canary.yml` and `autofix.yml` is guarded by
> `if: github.repository == 'omardev29/raylib_multiplatform'`. A repository created from this
> template is a real copy, not a fork, so its cron *would* fire — the guard makes every job skip
> immediately. Nobody's private game repo ends up with a weekly build it did not ask for, or an
> agent opening PRs against it. If you fork this to maintain your own, change that string
> in both files; otherwise leave it alone.

**`canary.yml`** runs `0 4 * * 1` (Mondays) and on dispatch. It calls **the same reusable
workflows** as `ci.yml` — not copies, which would drift — with the pins deliberately unset:

| Family | CI uses | The canary uses |
|---|---|---|
| Linux / Web / Android | image digest | `ghcr.io/omardev29/raylib-build:latest` |
| Apple | `macos-26`, Xcode 26.6 | `macos-latest`, newest Xcode on the image |
| Windows | `windows-2025`, Mesa pinned + sha256 | `windows-latest`, Mesa `latest`, **no** checksum |
| BSD | nothing (mirrors are rolling) | nothing — it is already a canary |

The empty `mesa_sha256` is intentional: with a checksum the job would die at verification before
testing the thing that matters. Each family prints `CANARY_OBSERVED <key>=<value>` lines, and that
**version delta is the product** — more than the log. `workflow_dispatch` takes a `families` input
(`apple`, or `apple,windows`) so a single family can be exercised without paying for all six, and
`xcode_version`/`image_tag` overrides so a failure can be forced deliberately.

There is no `continue-on-error`. It is its own workflow, it gates nothing, and a red canary should
look red.

**`.github/scripts/canary_triage.py`** runs when any family fails, under
`if: always() && (contains(needs.*.result, 'failure') || contains(needs.*.result, 'cancelled'))` —
`failure()` alone misses the cancelled case, and BSD hits its 60-minute timeout often enough to
matter. It writes `.github/canary-report.json`:

```json
{ "family": "apple", "runner": "macos-26", "job": "apple / ios",
  "step": "Build raylib.xcframework", "known": false,
  "version_delta": [{"key": "xcode", "frozen": "26.6", "observed": "27.0", "source": "measured"}],
  "error_lines": ["..."], "key": "apple/xcode-99.9/build-raylib-xcframework" }
```

Three things about it are non-obvious and were learned the hard way:

- Logs come from `GET /repos/{o}/{r}/actions/jobs/{id}/logs` per job. `gh run view --log` refuses
  while a run is in progress; the per-job endpoint works for any job that has finished.
- That endpoint 302s to Azure blob storage, which **rejects the request if the `Authorization`
  header is replayed**. urllib follows redirects with headers intact, so the redirect is
  intercepted and the second fetch made unauthenticated.
- Actions echoes each script line before running it, so a log contains both
  `xcode: $(xcodebuild -version…)` and the real output. The `CANARY_OBSERVED` regex is anchored to
  line start and end, or the report reads back the source of its own probe.

The delta is filtered to the family's own keys and ordered by likely cause (Xcode before the runner
label, image tag before everything) — otherwise an alphabetical sort puts `macos_runner` first and
an Apple report leads with a Windows Mesa version. Findings are keyed and recorded in
`.github/known-breakage.md`; a key already in the ledger with an unchanged signature comes back
`known: true` and does not raise the alarm twice.

**`autofix.yml`** triggers on `workflow_run: [canary] completed` with `conclusion == 'failure'`,
plus `workflow_dispatch` with a `run_id` (a `workflow_run` workflow always runs from the default
branch, so without the manual input there is no way to test a change before merging it). It reads
the triage artifact and builds a matrix of one job per broken family, deduplicated — Apple failing
on both macOS and iOS is one problem, not two.

The important part: `runs-on: ${{ matrix.runner }}`. **The agent runs on the platform that broke.**
A job has exactly one `runs-on`, so a single job could not repair an Apple and a Linux regression in
the same run; the matrix gives each family its own runner. On Apple that means a real `macos-26`
session (macOS runners have no Docker); on Linux/Web/Android it is `ubuntu-24.04` driving the pinned
image through `docker run` (the action cannot run *inside* the image — it has no node); BSD is
semi-blind, so the agent proposes and the PR validates.

It uses `anthropics/claude-code-action` pinned by SHA, and **skips cleanly when no
`CLAUDE_CODE_OAUTH_TOKEN`/`ANTHROPIC_API_KEY` is configured** — the `secrets` context is not
available in a job-level `if:`, so the check is a step-level `HAS_KEY` env comparison.

Guardrails, in order of how much they matter:

1. `GITHUB_TOKEN` **cannot write to `.github/workflows/`**. This started as the obstacle that made
   the whole idea look unworkable (every floating pin lived in workflow YAML, so the agent's PR
   would have been empty) and became the design: the pins moved into
   `thirdparty/FROZEN_VERSIONS.md`, and the agent now *cannot* rewrite the CI even if it decides it
   should.
2. `tools/versions_check.sh` runs in the same PR. Bump the NDK in one place and not the other and
   the agent's own PR goes red.
3. `permissions: {contents: write, pull-requests: write, actions: read}`, branch `autofix/<key>`,
   **draft** PR, deduplicated against an open PR with the same key.
4. The prompt is scoped to one family, its reusable, and `FROZEN_VERSIONS.md` — explicitly not
   `ci.yml`, the release path, or anything to do with signing.

`source` on a delta entry is `measured` when the runner really reported that version, and
`requested` when the job died before it could — a bad Xcode pin fails at *Select Xcode*, before
anything echoes what it resolved to, and a report with no delta at all would be useless. The
distinction used to be a comment in `canary_triage.py` and nothing more, so a value nobody observed
arrived looking like a measurement. The autofix agent worked that out for itself by reading the
script, which is turns it should not have needed and a trap a less careful reader would fall into.

### Tests

**Static** — every target: the binary exists, is executable, and has the right format and
architecture (ELF `e_machine`, PE machine type, Mach-O slices, web artifacts, APK/AAB contents
and signature).

**Runtime** — the game is actually started and the frame it draws is inspected:

| Target | How |
|---|---|
| Linux x64 / ARM64 | headless under `xvfb` with `RAY_TEST_MAX_FRAMES` |
| Windows x64 | on the real runner, with Mesa's software rasteriser staged next to the `.exe` |
| Web | headless Chromium; the composited canvas is screenshotted and measured |
| iOS | *(disabled)* — written, but the `macos-26` simulator will not boot under `simctl`. Set `vars.IOS_SIMULATOR_TEST=true` to try it. |

Booting is not the interesting part. A broken shader, a lost texture binding or a draw call
that silently no-ops still boots, still exits 0, and used to pass. So each of these asserts
`RAY_TEST_RENDER_OK`: the frame is read back and the fraction of pixels differing from the
**most common** colour must fall in a sane range. Blank frame → no marker → red.

Windows deserves a note, because it used to be the worst case. Hosted runners have no GPU, so
GLFW could not create a GL context and the render loop faulted right after boot — and the old
test explicitly tolerated that crash, which meant no Windows render regression could ever fail
CI. Mesa gives the shipped binary a genuine WGL context on real Windows; the DLLs are deleted
before packaging and the package step asserts they are not in the zip, because shipping them
would force software rendering on every player.

The gate is the **ratio** on every target, because llvmpipe, ANGLE-on-Metal, SwiftShader and
mobile GPUs disagree about text antialiasing and texture filtering. The hash is asserted **too**,
but only where it is deterministic: the targets that render through raylib's own software
rasteriser (`PLATFORM=Memory` locally, the BSDs and macOS in CI) all produce `e2ffc15d`, and
`tools/render_check.sh` fails on anything else. DRM renders through OpenGL ES on llvmpipe and
gives a different hash with the same pixel count and ratio; that one is compared against its
own golden and must not be "fixed" by copying one over the other.

**Played, not just booted** — `tests/platformer_play.cpp` drives `examples/games/07_platformer`
through its real loop with the keys fed through the input and UI seams: to the flag at 60 Hz, Play
again, three falls, game over, Menu; a jump from the moving platform at 60 and at 240 Hz; and to
the flag again at 240 Hz. The examples job runs it after booting every example, and it prints
`PLAY PASS` or what went wrong and where. It was written to answer "is the game playable from start
to finish?", and it found three bugs a boot never would: a lift that turned round the frame it
arrived (nobody could step off), a rider that kept a pixel of overlap (the ledge's corner), and a
platform that undid every jump from it above ~127 FPS.

**Sanitized** — every Debug build of the framework runs under AddressSanitizer (with
LeakSanitizer on Linux) and UndefinedBehaviorSanitizer, every report fatal: `[dev] sanitize` in
the `.toml`, applied by `cmake/sanitize.cmake` where a probe can run a sanitized program, so the
unit tests, the layout test, the smoke and render checks and every example are all instrumented
(a game turns it on; `rmp new` writes `[]`). `tools/sanitize_check.sh` (`rmp test sanitize`, a step
of the `lint` job) builds the unit tests a second time with gcc. A test asserts what it can see; an
int overflow that wraps somewhere harmless or a float cast that lands in range is what it cannot.
The first run found the LDtk reader's casts and three tests reading objects already freed.
Third-party code it must not report is listed, with the reason, in `cmake/sanitize_ignore.txt`.

> **What the tests still do NOT cover.** BSD, RISC-V and Windows ARM64 are compiled and
> format-checked but never run — there is no runner or emulator for them here. Android is built
> and can optionally be smoke-run in Firebase Test Lab (below). iOS is built
> and statically verified (the app bundle must contain a readable `CFBundleIdentifier` and its
> `resources/` folder) but not executed, because the hosted simulator does not boot reliably.
> **Test your actual game on each platform you ship.**

### Releasing

Tag pushes run everything, then `_release.yml` collects the artifacts, generates `SHA256SUMS`
and publishes a Release with the games, the iOS xcframework and `.app`, the APK, the signed AAB
(when real signing secrets exist) and `THIRD_PARTY_LICENSES.md`.

### Publishing to itch.io

Tag pushes publish automatically once these are set:

| Key | Where |
|---|---|
| `user` | `[deploy.itch]` in `raylib_multiplatform.toml` — your itch.io username |
| `game` | `[deploy.itch]` — the game's URL slug |
| `BUTLER_API_KEY` | Repository **secret** — from <https://itch.io/user/settings/api-keys> |

The first two are not secrets, so they live in the config file with everything else. Repository
variables `ITCH_USER` / `ITCH_GAME` still work and **take precedence** over the config, which is
there for setups that predate the config file — if a value looks ignored, check whether a variable
is shadowing it.

Without them the job logs a warning and skips; cloning this repository must not give you a red
pipeline for a service you have not signed up to.

Two details that are easy to get wrong and are handled for you:

- **HTML5 must be uploaded as a directory containing `index.html`.** Pushing a `.zip` gives you
  a downloadable file that nobody can play in the browser. The job unpacks the web build and
  copies `ray_test.html` to `index.html`.
- **itch infers the platform from the channel name**, and only recognises a fixed set. The
  `linux-x64-glibc`, `linux-arm64-glibc`, `windows-x64`, `windows-arm64`, `osx`, `android` and `html5`
  channels get tagged correctly; `linux-riscv64-glibc` is uploaded as a plain download because itch
  has no concept of it. The BSD builds are deliberately **not** pushed to itch at all — they
  would be untagged downloads on a storefront with no BSD audience. They are still attached to
  the GitHub Release.

### Firebase Test Lab

An emulator inside an unaccelerated runner is slow and fails for reasons unrelated to your
game. Test Lab runs the actual APK on one of Google's devices -- by default MediumPhone.arm, an
Arm virtual device with GPU acceleration; a physical model in `[deploy.firebase] device` puts it on
a phone -- which is the one thing CI cannot otherwise check.

| Key | Where |
|---|---|
| `project_id` | `[deploy.firebase]` in `raylib_multiplatform.toml` — the GCP project id |
| `device` | `[deploy.firebase]`, optional — Google retires device models every few years |
| `GCP_SA_KEY` | Repository **secret** — the service-account JSON, whole file |
| `FIREBASE_RESULTS_BUCKET` | Repository variable, optional — a GCS bucket to pull results back from |

As with itch, `vars.FIREBASE_PROJECT` / `vars.FIREBASE_DEVICE` still work and take precedence over
the config file.

Setup, once:

1. Create (or pick) a GCP project and **enable Blaze billing**. Test Lab's free Spark-tier
   quota no longer exists.
2. Enable the APIs: `gcloud services enable testing.googleapis.com toolresults.googleapis.com`
3. Create a service account and grant it
   `roles/cloudtesting.testAdmin` and `roles/cloudtoolresults.testAdmin`.
   If you set `FIREBASE_RESULTS_BUCKET`, also grant `roles/storage.objectAdmin` on that bucket.
4. Create a JSON key for it and paste the whole file into the `GCP_SA_KEY` secret.
5. Check the device model still exists: `gcloud firebase test android models list`.

The job only ever submits the **debug** APK, and refuses anything else. Robo crawls the UI and
will happily click an ad banner; the debug build uses Google's official test ad unit ids, so
those clicks are harmless, whereas a release build would generate invalid traffic against your
real ad units.

**iOS is not possible here**, and that is not an oversight: `gcloud firebase test ios run`
requires a signed `.ipa`, which requires an Apple developer account.

---

## Maintenance & platform longevity

Everything is pinned (frozen image + pinned actions + frozen raylib), so the pipeline keeps
**building** unchanged for a long time. The thing that forces updates is not the build — it's the
**app-store SDK requirements**, which move on their own schedule.

| Platform | Builds keep working… | Publishing constraint |
|---|---|---|
| Windows / macOS / Linux | Years. Frozen toolchain in the image. | None (no store gate). |
| Web (Emscripten) | Years. emsdk is pinned in the image; newer emsdk only needed for new features. | None. |
| **Android** | Builds fine as-is. | **Play Store demands a recent `targetSdk`** (roughly yearly). You'll bump `compileSdk`/`targetSdk` + NDK + AGP + Gradle + Java about once a year to keep publishing. |
| **iOS** | Builds via the fork. | **App Store requires a recent Xcode/SDK** (roughly yearly). Requires updating the `raylib-iOS` fork / ANGLE. |
| BSD | Stable. `cross-platform-actions` and its disk images update occasionally. | None (no store). |

**Bottom line:** *building* stays green for years without touching anything. *Publishing* to the
Play Store / App Store is what forces SDK bumps — roughly **once a year for Android and iOS**.
Desktop/BSD/Web have no such gate.

Specifics to expect when the stores move:

- **Android:** Google Play requires new apps and updates to target an API level within about a
  year of the latest — **API 36 from 2026-08-31**, which is what we target today. To
  keep publishing you bump, *together*, `compileSdk`/`targetSdk`/`buildToolsVersion`/`ndkVersion`
  in `raymob/app/build.gradle`, AGP in `raymob/build.gradle`, Gradle in `gradle-wrapper.properties`
  (with its `distributionSha256Sum`), the matching packages in the build image, and the
  ```versions block in `thirdparty/FROZEN_VERSIONS.md`. These are a compatibility **set**, not
  independent knobs: AGP 8.13 requires Gradle ≥ 8.13 and JDK 17 and caps at API 36.1.
  `tools/versions_check.sh` fails CI if you miss one. The native raylib/raymob code usually needs
  no change.

  One trap when you get there: **do not move to AGP 9 without rewriting `raymob/app/build.gradle`
  first.** Gradle 9 removes `gradle.buildFinished` and AGP 9 removes `applicationVariants`, and
  that file uses both. AGP 8.13.2 is the last line that tolerates it.
- **iOS:** Apple requires submission with a recent Xcode. The `raylib-iOS` fork (and its bundled
  ANGLE) must be updated to build against the new SDK/Xcode. Watch the fork for releases.
- **Note:** the frozen image pins today's SDKs, so an Android SDK bump means rebuilding the image
  too. The order matters: bump the image repo first, take the digest from its run summary, then
  update `PINNED_IMAGE` in `ci.yml` and the ```versions block in the same commit as the Gradle
  changes. Bump only one side and `tools/versions_check.sh` will tell you — which is the point of
  it. A container job also refuses to build if Gradle asks for an SDK platform the image does not
  carry, because AGP would otherwise silently download it at job time and quietly void the whole
  "nothing is downloaded at job time" claim.

---

## Adding source files & libraries

**Source files:** drop any `.cpp` into `src/` (or a subfolder). The `GLOB_RECURSE ... CONFIGURE_DEPENDS`
picks it up on the next build. Headers go in `include/`.

**Libraries:** put the library in `thirdparty/`, `add_subdirectory(thirdparty/yourlib)` it, and add
its target to `target_link_libraries` -- and before any of that, its row in the components block of
`THIRD_PARTY_LICENSES.md`, because `tools/license_db.py --check` fails on a directory under `thirdparty/`
it does not know. An unmodified library is also pinned by content: the failure message prints the
exact `sha256_<name>` line to add to the versions block of `thirdparty/FROZEN_VERSIONS.md` (for a
directory, a hash over every source file in it, subdirectories included). A modified one gets a
`PATCHES.md` that says MODIFIED instead. And a library with headers goes on the four include lists
the framework keeps -- `tests/configure_test.py` names them when one is missing.

```cmake
target_link_libraries("${CMAKE_PROJECT_NAME}" PRIVATE
    raylib_static raymoblib rres yourlib_target
)
```

The target name is whatever the library's own `CMakeLists.txt` defines.

---

## FAQ & troubleshooting

**Q: I changed `PRODUCTION_BUILD` and the build is wrong.**
A: Use the preset that says it -- `debug` or `release`, each in its own folder -- rather than
flipping the option in one: CMake caches the value and VS doesn't always notice the change. `rmp
clean` deletes every build when one goes strange.

**Q: Do I need to ship DLLs?**
A: No. raylib and the C and C++ runtimes are linked into the `.exe` (MinGW `-static`, MSVC
`/MT`), and CI reads the import table to prove it: `python tools/binary_check.py windows
build/release/<name>.exe` lists every DLL it needs and fails on one Windows 10 does not have.

**Q: `file` / arch checks fail in CI?**
A: The static tests use `od` to read the ELF magic + `e_machine` (no `file` dependency in the
build image), so they're portable.

**Q: Why is there no `resources.rres` in my dev build?**
A: It's only produced when you run `rmp pack` (`cmake --build --preset debug --target
pack_resources`). In dev the game loads loose files by default.

**Q: The game shows no texture / "Failed to open file".**
A: Check `RMP_RESOURCES_PATH` — in dev it's an absolute path; in production the `resources/` folder
(or `resources.rres`) must sit next to the executable. The log says which folder a release read
(`ASSETS: reading ./resources/ next to the executable, in ...`).
