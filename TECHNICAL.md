# Technical reference

How this framework works, in depth. For the quick-start see [README.md](README.md).

## Table of contents

- [Directory layout](#directory-layout)
- [Build system](#build-system)
- [Editor / clangd (LSP)](#editor--clangd-lsp)
- [Compile-time definitions](#compile-time-definitions)
- [Platform detection macros](#platform-detection-macros)
- [Resources: `RESOURCES_PATH`, `rmp::assets` and rres](#resources-resources_path-rmpassets-and-rres)
- [Game lifecycle (Godot style)](#game-lifecycle-godot-style)
  - [Why web does not get a `while` loop](#why-web-does-not-get-a-while-loop)
- [`rmp::ui` — the interface layer](#rmpui--the-interface-layer)
- [`rmp::app` — the entry point and closing the app](#rmpapp--the-entry-point-and-closing-the-app)
- [AdMob (Android)](#admob-android)
- [Web export](#web-export)
- [Android (raymob)](#android-raymob)
- [iOS](#ios)
- [BSD & RISC-V targets](#bsd--risc-v-targets)
- [CI/CD pipeline](#cicd-pipeline)
  - [The canary, and the thing that fixes itself](#the-canary-and-the-thing-that-fixes-itself)
- [Maintenance & platform longevity](#maintenance--platform-longevity)
- [Adding source files & libraries](#adding-source-files--libraries)
- [FAQ & troubleshooting](#faq--troubleshooting)

---

## Directory layout

```
.
├── raylib_multiplatform.toml # THE config. Name, ids, targets, icon, modules. Yours.
├── CMakeLists.txt            # Root build: links raylib statically, presets, rres
├── CMakePresets.json         # debug / release / web profiles
├── src/                      # YOUR code. Every .cpp/.c here is auto-compiled (GLOB_RECURSE).
│   ├── main.cpp              # your game
│   └── rmp/                  # THE framework's implementation — not yours
│       ├── internal.h        #   private surface, deliberately not in include/
│       ├── rres_impl.cpp     #   compiles rres once (container + AES + Argon2i + QOI)
│       ├── pack.cpp          #   open/close resources.rres, read one entry
│       ├── loader_hook.cpp   #   routes raylib's own LoadFileData/Text through the pack
│       ├── assets.cpp        #   rmp::assets — the public surface, with the loose-file fallback
│       ├── app.cpp           #   rmp::app — closing the app
│       └── ui/               #   rmp::ui
│           ├── clay_impl.cpp #     compiles Clay once
│           ├── internal.h    #     the only place Clay is allowed to exist
│           ├── context.cpp   #     lazy start, scale, font, text arena, element ids
│           ├── widgets.cpp   #     begin / end / button / text
│           ├── containers.cpp#     row / column / panel / stack / grid / scroll
│           ├── controls.cpp  #     checkbox / slider / dropdown / text_input
│           ├── focus.cpp     #     keyboard and gamepad navigation
│           ├── style.cpp     #     variants, sizes, transitions
│           ├── render.cpp    #     draw commands -> raylib calls
│           └── Theme.cpp     #     the dark and light themes
├── include/                  # YOUR headers (already on the include path)
│   └── rmp/                  # THE framework's headers — include what you use
│       ├── app.h             #   RMP_ENTRY_POINT + rmp::app::quit()
│       ├── ui.h              #   rmp::ui — the public API and the Theme
│       ├── assets.h          #   the rmp::assets declarations
│       ├── ads.h             #   rmp::ads — inline wrappers over <admob.h>
│       ├── math.h            #   vectors, rectangles, colours (raymath)
│       ├── config.h          #   the APP_* values from the .toml
│       └── generated/        #   GENERATED config.h, git-ignored
├── examples/                 # reference code, by namespace: ui/ ads/ assets/ platform/
│   └── plain_c/main.c        # the opt-out: plain C, <raylib.h> only, your own main()
├── tests/
│   ├── smoke_test.h          # CI boot + render hook (RAY_TEST_MAX_FRAMES), header-only
│   └── ui_layout_test.cpp    # layout checks with no window (-DBUILD_UI_TESTS=ON)
├── resources/                # Your assets (flat — the pack does not recurse)
├── branding/icon.png         # the source for every app icon; rename it in [icon] source
├── tools/
│   ├── configure.py          # the config -> every build system. Run by CMake.
│   ├── versions_check.sh     # fails CI when the pins drift apart
│   ├── dev_shell.sh          # run a command inside the pinned CI image
│   └── rres_pack.c           # open rres packer (AES-256) — no paid tooling needed
├── cmake/
│   ├── configure_hook.cmake  # runs the generator before project()
│   ├── generated/            # GENERATED, git-ignored
│   └── toolchain-riscv64-linux.cmake
├── raymob/                   # Android app shell (Gradle). See "Android (raymob)".
│   ├── generated.properties  # GENERATED, git-ignored
│   └── app/
│       ├── AndroidManifest.template.xml  # the manifest, with #if blocks
│       ├── generated/        # GENERATED manifest, git-ignored — what Gradle reads
│       └── src/{admob,noadmob}/java/     # AdmobBridge: the real one, and the no-op
├── ios/                      # iOS scaffold. project.yml is GENERATED.
├── .github/
│   ├── workflows/ci.yml      # orchestrator: triggers, pins, job graph
│   ├── workflows/canary.yml  # weekly build against floating versions
│   ├── workflows/autofix.yml # agent dispatched onto the runner that broke
│   ├── workflows/_*.yml      # one reusable workflow per platform group
│   ├── known-breakage.md     # canary failures we have seen and chosen not to chase
│   └── scripts/              # web boot test, canary triage, upstream report
└── thirdparty/
    ├── raylib/               # raylib 6.0 (frozen, slightly patched for this build)
    ├── raylib-ios/           # raylib-iOS fork (submodule) — iOS only
    ├── raymob/               # raymob C sources (Android native bridge + admob)
    ├── clay/                 # Clay — the layout engine behind rmp::ui (zlib)
    ├── rres/                 # rres.h + rres-raylib.h + externals
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

`Justfile` holds the handful of commands worth having a shortcut for — `just run`, `just test`,
`just rel`, `just web`, `just android` — and deliberately nothing else, so `just --list` stays
something you can read rather than a menu to search.

---

## Build system

raylib is linked **statically** (`raylib_static`), so you ship a single self-contained
binary — no DLLs, and on MSVC even the CRT is static (no VC++ Redistributable).

**Presets** (`CMakePresets.json`), all Ninja-based:

| Preset | `PRODUCTION_BUILD` | `RESOURCES_PATH` | Notes |
|---|---|---|---|
| `debug` | `0` | absolute source path | run from anywhere |
| `release` | `1` | `"./resources/"` | LTO, ship `resources/` next to the exe |
| `web` | `1` | `"./resources/"` | Emscripten toolchain |

**LTO** (`CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE`) is enabled for release on GCC/Clang and
MSVC. It yields smaller/faster binaries at the cost of longer link times. On NetBSD LTO is
disabled (its linker can't process LTO bytecode in static archives).

**Assembler view:** `cmake --build build --target assembler` emits `build/main.s` for the
game sources (GCC/Clang) — handy for inspecting codegen.

---

## Editor / clangd (LSP)

The committed `.clangd` points clangd at the **host** compile database
(`build/compile_commands.json`), so the project resolves with **no Android NDK**. Because no
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
| `RESOURCES_PATH` | absolute (dev) or `"./resources/"` (prod) | Asset folder path |
| `PRODUCTION_BUILD` | `0` / `1` | `#if PRODUCTION_BUILD` to strip debug code |
| `RRES_PASSWORD` | string | rres decryption password (see below) |
| `APP_NAME`, `APP_WINDOW_TITLE`, `APP_WINDOW_WIDTH/HEIGHT` | from `[project]` / `[window]` | Your identity and design resolution |
| `APP_UI_FONT`, `APP_UI_FONT_SIZE`, `APP_UI_SCALE`, `APP_UI_MAX_ELEMENTS` | from `[ui]` | What `rmp::ui` starts with |

```cpp
Texture2D tex = LoadTexture(RESOURCES_PATH "player.png");  // raw path form
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

## Resources: `RESOURCES_PATH`, `rmp::assets` and rres

### The problem this solves

raylib loads assets from **paths**: `LoadTexture("resources/player.png")`. That is fine until you
ship. Then you have a folder of loose files next to the executable that anyone can open, replace or
copy; on Web every one of them is a separate HTTP request; and the path that worked from your IDE
(CWD = project root) does not work when the user double-clicks the binary (CWD = anywhere).

Two mechanisms handle that here, and they are independent.

### 1. `RESOURCES_PATH` — where "resources/" is

A compile-time string, defined in `CMakeLists.txt`:

| Build | Value |
|---|---|
| Development | absolute path to `<repo>/resources/` — run the binary from any CWD |
| `PRODUCTION_BUILD=ON` | `"./resources/"` — relative to the executable, which is how the packages are laid out |

Always build your paths from it (`rmp::assets::` does this for you). A bare `"resources/foo.png"`
works in dev and silently fails in a release.

### 2. rres — one file instead of a folder

[rres](https://github.com/raysan5/rres) is raysan's resource-container format: a header, N data
chunks, and a central directory mapping names to chunk ids. This framework ships its own packer,
`tools/rres_pack.c`, built as a CMake target:

```bash
cmake --build build --target pack_resources    # resources/ -> resources/resources.rres
cmake --build build --target unpack_resources  # delete the pack, go back to loose files
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
Texture2D t = LoadTexture(RESOURCES_PATH "player.png");   // reads the pack
Model     m = LoadModel(RESOURCES_PATH "ship.obj");       // and so does this
```

It matters more than it looks. Without the hook, `LoadTexture(RESOURCES_PATH "x.png")` works
perfectly in development and comes back `0x0` in a release — because a release ships
`resources.rres` and not the loose files. Nothing warns you; the texture is just blank. Anyone who
had not read this page would write that line, and it would be the last thing they suspected.

Three details make the hook safe rather than clever:

- **raylib returns the callback's result verbatim.** `rcore.c` reads
  `if (loadFileData) return loadFileData(fileName, dataSize);` — there is no fallback to the
  filesystem behind it. So a miss has to delegate by hand, and the exact way to delegate is to
  *unhook, call raylib, hook back*. That runs raylib's own reader, including the Android one, where
  `fopen` is redirected into the APK's asset manager and stdio of our own would find nothing.
- **It only answers for files under `RESOURCES_PATH`.** The pack is keyed by bare file name, so
  matching on the name alone would let a save file called `level1.json` anywhere on disk be
  answered with the packed `level1.json`.
- **Models come along for free.** `rmodels.c` loads `.obj` with `LoadFileText` and
  `.gltf`/`.glb`/`.iqm`/`.vox`/`.m3d` with `LoadFileData`, and it resolves the sibling files an
  `.obj` or `.gltf` refers to — the `.mtl`, the `.bin` buffers, the textures named inside the
  material — through the same two functions. Hooking those two hooks the whole chain.

`rmp::assets::shutdown()` unhooks and frees the directory. Calling `Init()` twice is a no-op.

### What you actually get

```cpp
Texture2D      rmp::assets::load_texture(const char *name);
Image          rmp::assets::load_image  (const char *name);
Sound          rmp::assets::load_sound  (const char *name);
Font           rmp::assets::load_font   (const char *name, int fontSize);
unsigned char *rmp::assets::load_data   (const char *name, int *size);   // free with UnloadFileData
bool           rmp::assets::using_pack();
```

`name` is the **bare file name** — `rmp::assets::load_sound("jump.wav")`. Not a path. `tools/rres_pack.c`
stores every entry under its basename and the CMake glob does not recurse, so `resources/sfx/` is
not packed at all and there is nothing for a nested name to resolve to. See the subfolder note
below.

Every one of them has the same shape: if in pack mode, look the name up, decrypt, decode from
memory; if anything fails, **log a warning and fall back to the loose file**. A missing entry
degrades instead of crashing, and the warning names the asset.

### What happens to each kind of file in `resources/`

Everything directly in the folder is packed — the packer does not filter by type:

| You put in `resources/` | Load it with | Notes |
|---|---|---|
| `.png .jpg .bmp .tga .gif .qoi .dds .ktx .hdr` | `rmp::assets::load_texture` / `LoadImage`, or plain `LoadTexture` | the extension in props tells raylib which decoder to use |
| `.wav .ogg .mp3 .flac .qoa .xm .mod` | `rmp::assets::load_sound` | short sounds; fully decoded into RAM |
| `.ttf .otf` | `rmp::assets::load_font(name, size)` | size is baked at load time, as always in raylib |
| `.obj .mtl .gltf .glb .bin .iqm .vox .m3d` | plain `LoadModel(RESOURCES_PATH "…")` | works through the loader hook, siblings included — keep them all directly in `resources/` |
| `.vs .fs .glsl` | plain `LoadShader(RESOURCES_PATH "…")` | also hooked; `LoadShaderFromMemory` if you prefer |
| `.json .txt .csv` and anything else | `rmp::assets::load_data` | you get the bytes |
| **long music** (`.ogg/.mp3` streamed) | see below | the one real exception |

### The two things that stay outside the pack

**Streamed music.** `LoadMusicStream` is path-only by design, and unlike the model loaders it does
not go through `LoadFileData`: `raudio.c` hands the file name straight to `drwav_init_file`,
`drmp3_init_file` or `jar_xm_create_context_from_file`, which open it themselves. The point of a
music stream is that it is *not* fully in memory, so there is nothing for the hook to intercept.
(`LoadMusicStreamFromMemory` exists in recent raylib, but it requires you to keep the whole encoded
buffer alive for the lifetime of the stream, which defeats the purpose. Short sound effects have no
such problem — use `rmp::assets::load_sound`.)

**Subfolders.** `file(GLOB …)` in `CMakeLists.txt` does not recurse and resource names are flat, so
nothing inside `resources/art/` is packed.

For both: the files have to actually ship, and **the release packages are not uniform**:

| Target | Packs? | What ships in the package |
|---|---|---|
| Linux x64/arm64, Windows x64, macOS | yes | **`resources.rres` only** — nothing else is copied in |
| Linux riscv64, Windows arm64, the three BSDs | no | the whole `resources/` folder |
| Web | no | the whole folder, preloaded into the Emscripten virtual FS |
| Android | no | the whole folder, copied into `assets/` (minus any `.rres`) |
| iOS | no | the whole folder, as a folder reference inside the `.app` |

Only the first row runs `pack_resources`, which has a consequence worth saying out loud: **the
encryption applies to four targets and no others.** On Web, Android, iOS, BSD and riscv64 your
assets ship as ordinary files that anyone can open. If that matters, pack them yourself in the job
that builds those targets — but see the Android note below before you try it there.

So an un-packable file works in development, works on Web and Android, works on BSD — and is
missing only in the four packaged desktop builds. That is the worst possible failure mode, so
`CMakeLists.txt` emits a `WARNING` at configure time when `PRODUCTION_BUILD=ON` and it finds a
subfolder in `resources/`. If you need one anyway, extend the `package/` step in
`.github/workflows/_linux.yml`, `_windows.yml` and `_apple.yml` to copy it alongside the pack.

> The pack is also never used on **Android**, whatever you do. rres opens the container with plain
> `fopen`, and inside an APK there is no such file — raylib reaches its assets through
> `AAssetManager`, which only its own internal `fopen` redirect knows about. `FileExists` returns
> false, `rmp::assets::init()` reports loose-file mode, and the loose files are there because the
> Android job copies the whole folder in. Nothing breaks; it just means `[resources]
> rres_password` buys you nothing on that one platform.

### Encryption

Password: `[resources] rres_password` in `raylib_multiplatform.toml`, which reaches both sides from
one place — `RRES_PACK_PASSWORD` for the packer and `APP_RRES_PASSWORD` in
`include/rmp/generated/config.h` for the game. (They used to be two hardcoded literals in
`CMakeLists.txt` and the asset layer; desynchronising them broke loading at runtime only.)

> **This is obfuscation, not security.** The password is a string inside a binary you hand to the
> player; anyone determined will find it. What it does buy is that assets are not casually
> extractable by dragging the folder open. Cost is *load-time* only — Argon2i (16 MiB, 3 passes)
> per resource, nothing per frame.

---

## Game lifecycle (Godot style)

Your game lives in three functions in `src/main.cpp`:

| Hook | When |
|---|---|
| `on_ready()` | once at startup — window, assets, preload |
| `on_frame()` | every frame |
| `on_exit()` | once at shutdown — unload |

A small platform runner drives them, and there are three of them. The difference between them is
**who owns the frame loop**:

| Platform | Runner | Who owns the loop |
|---|---|---|
| Desktop / BSD / Android | `main()` with `while (!WindowShouldClose())` | We do |
| **Web** | `emscripten_set_main_loop(frame, 0, 1)` | The browser does |
| iOS | `ios_ready` / `ios_update` / `ios_destroy` | UIKit does, via `CADisplayLink` |

This is what lets the **same game code** run on every platform, including iOS and the browser.

#### Why web does not get a `while` loop

Because a `while` loop in a browser is not free, and the price is invisible. JavaScript is
single-threaded and cooperative: a function that never returns never gives the event loop back, so
nothing renders. The only way to run a synchronous loop there is `-s ASYNCIFY`, which rewrites the
program so its stack can be unwound at a suspension point and restored afterwards. That
instrumentation is **not billed to the loop** — it is billed to every function that might be on the
stack when a suspension happens, which is most of them, in code size and in speed, whether or not
anything ever suspends.

raylib says so itself, in the comment above `WindowShouldClose()` in `rcore_web.c`:

> `WindowShouldClose()` is not called on a web-ready raylib application if using
> `emscripten_set_main_loop()` […] allowing the browser to manage execution asynchronously

That call is the **only** `emscripten_sleep()` in raylib. Not calling it is what lets ASYNCIFY go
away entirely, which is why `CMakeLists.txt` does not pass it. Measured on this project, same
commit otherwise, from the CI artefact check:

| | With ASYNCIFY | Without | |
|---|---|---|---|
| `.wasm` | 349,720 B | 257,505 B | **−26 %** |
| `.js` | 194,721 B | 189,379 B | −3 % |
| Total download | 544,441 B | 446,884 B | **−18 %** |

That is 95 KB off a build that draws a menu and a rabbit. It does not shrink as the game grows —
it is a percentage of the whole binary, because the instrumentation is per function. One frame per callback is also what
`requestAnimationFrame` wants: the browser schedules us with the display instead of us blocking it
and asking for control back every 12 ms.

Two rules follow, and they are in the header next to the code:

- **Do not call `SetTargetFPS()` on web.** `fps = 0` means `requestAnimationFrame`, which is
  already the right cadence. A target FPS would make raylib sleep against the browser's scheduler.
- **If you put a `while` loop back, you have to put `-s ASYNCIFY` back with it.**

`rmp::app::quit()` works the same as everywhere else: the frame that asked to quit finishes, then
`emscripten_cancel_main_loop()` runs and the shutdown order is identical to desktop.

`RMP_ENTRY_POINT` in `include/rmp/app.h` is that
runner. It also
brackets your three hooks with `rmp::assets::init()` before `on_ready()` and `rmp::assets::shutdown()` after
`on_exit()`, so opening the resource pack is not something `src/main.cpp` has to remember — and so
that rewriting `main.cpp` from scratch cannot accidentally drop it.

A CI smoke-test hook is built in: set the env var `RAY_TEST_MAX_FRAMES=N` and the game renders
N frames then exits with code 0, printing `RAY_TEST_BOOT_OK` and `RAY_TEST_DONE_FRAMES`. The hook
itself lives in **`tests/smoke_test.h`** (header-only, so it compiles on every target without
wiring extra sources). `SmokeTest_Begin()`, `SmokeTest_ReportBoot()` and `SmokeTest_Tick()` are all
called by the runner. The only one left in `src/main.cpp` is `SmokeTest_CaptureFrame()`, and it has
to be: only your code knows where the last draw call is.

**The boot marker reports asset failures, not successes** — `RAY_TEST_BOOT_OK assets_failed=0
assets_requested=1`, and CI fails unless `assets_failed=0`. It used to carry the dimensions of a
texture your `on_ready()` passed in, with CI insisting they were non-zero, which quietly made "ship
at least one image" a rule of the framework: a game drawing nothing but shapes could not pass, and
deleting the call failed the build. Counting failures keeps the check that mattered — iOS once
shipped a bundle with no `resources/` in it and every texture came back 0x0 — while a game that
requests nothing fails nothing and passes.

Only `rmp::assets::` calls are counted. The loader hook sees raylib's internal probing as well, such as
an `.obj` looking for a `.mtl` that legitimately is not there, and counting those would turn a
working build red.

Call them from wherever your drawing actually lives, including another file. The two counters are
`inline` variables in C++ precisely so that works: as file-scope statics each translation unit got
its own pair, so `SmokeTest_CaptureFrame()` in a second `.cpp` read a budget nothing had set,
returned immediately, and the render gate went silent — leaving CI passing a blank screen with no
sign anything was wrong.

---

## `rmp::ui` — the interface layer

Immediate mode: you describe the interface during the frame that shows it, and there is no widget
tree to keep, no objects to create and destroy, no state of ours to synchronise with state of
yours. What persists is your data; what is rebuilt every frame is the picture.

```cpp
rmp::ui::begin();
if (rmp::ui::button("Play"))    play();
if (rmp::ui::button("Options")) options();
if (rmp::ui::button("Quit"))    quit();
rmp::ui::end();
```

That is a centred main menu that holds up from 800×600 to 4K. The rest of this section is what to
reach for when the default is not what you want.

### The three rules

1. **`end()` draws.** The pair goes inside `BeginDrawing()`/`EndDrawing()`, and before
   `SmokeTest_CaptureFrame()` if you keep the CI hook.
2. **One UI frame per game frame.** A second `begin()` without an `end()` warns and is ignored.
3. **Interaction uses the previous frame's geometry.** A button cannot be clicked on the first
   frame it appears — see [Why one frame behind](#why-one-frame-behind).

### The frame

```cpp
struct FrameOptions {
    align placement = Align::CENTER;   // where the root's content sits
    float gap       = -1;              // between children; -1 = the Theme's
    float padding   = -1;              // inside the root;  -1 = the Theme's
};

void begin();
void begin(const FrameOptions &o);
void end();
```

`align` has the nine you would expect: `top_left`, `top_center`, `top_right`, `center_left`,
`center`, `center_right`, `bottom_left`, `bottom_center`, `bottom_right`.

The default is `center` and that is deliberate. A plain top-to-bottom layout would put a menu in
the top-left corner, and then "a menu is three functions" would be a lie.

### Widgets

```cpp
bool button(std::string_view label);
bool button(std::string_view label, const ButtonOptions &o);

void text(std::string_view s);
void text(std::string_view s, const TextOptions &o);
```

```cpp
struct ButtonOptions {
    Variant       style = Variant::NORMAL;  // normal|primary|danger|outline|ghost
    detail::Sizing size{};                  // Size::SMALL|medium|large, or a number
    bool          enabled = true;
    const char   *id      = nullptr;
};

struct TextOptions {
    ColorRole     color = ColorRole::TEXT; // text | muted | primary | danger
    detail::Sizing size{};                   // a step, a number, or the Theme's
    bool           wrap  = true;
};
```

You never name `detail::Sizing`. It is one field that accepts two spellings —
`{ .size = rmp::ui::Size::LARGE }` and `{ .size = 34 }` both land in it — rather than two fields
that could contradict each other. See [Variants and sizes](#variants-and-sizes).

`button` returns `true` on the frame the pointer is **released over it**, having been pressed on
it. Drag off and let go and nothing happens, which is what every interface worth using does.

`Variant` is semantic on purpose. `Variant::DANGER` says what the button *is*; the Theme decides
what red means. Restyling a game then never involves revisiting call sites.

```cpp
rmp::ui::button("Delete save", { .style = rmp::ui::Variant::DANGER });
rmp::ui::text("Paused", { .color = rmp::ui::ColorRole::MUTED, .size = 32 });
```

Field order in these structs is not cosmetic: **C++20 requires designated initialisers in
declaration order**, so they are ordered the way they are most likely to be written — Sizing, then
appearance, then identity. `{ .style = ..., .size = ... }` compiles; the other way round does not.

### Containers

Everything above arranges itself in a centred column. These are how you get structure, and they
compose:

```cpp
rmp::ui::panel([&]{
    rmp::ui::text("Really quit?");
    rmp::ui::row({ .grow_x = true }, [&]{
        if (rmp::ui::button("Yes")) quit();
        rmp::ui::spacer();
        rmp::ui::button("No");
    });
});
```

| | |
|---|---|
| `row(body)` | Left to right |
| `column(body)` | Top to bottom |
| `panel(body)` | A column with a background and padding — a dialog, a card, a tooltip |
| `center(body)` | Fills what it was given, puts the contents in the middle |
| `stack(body)` + `layer(body)` | Layers in the same box, later ones on top |
| `spacer()` / `spacer(n)` | Eats the leftover space, or a fixed gap |

The contents are a **lambda**, and that is the whole reason there is no matching `end()` to forget:
the compiler closes the container. Capture with `[&]` when the body needs your variables.

Every container takes `BoxOptions`:

```cpp
struct BoxOptions {
    float gap     = -1;              // between children; -1 = the Theme's
    float padding = -1;              // inside this container
    align items   = Align::CENTER;   // where children sit in the leftover space
    bool  grow_x  = false;           // fill the parent instead of fitting content
    bool  grow_y  = false;
    float width   = 0;               // > 0 = fixed, overriding fit/grow
    float height  = 0;
    const char *id = nullptr;        // only if you want to ask about it later
};
```

`PanelOptions` wraps that in `{ .box = {...}, .background, .radius, .border, .border_width }`.

**Sizing is three cases and no more.** Default is *fit*: as big as the contents need. `grow_x` /
`grow_y` is *grow*: fill what the parent offers. `width` / `height` is *fixed*, in design units. A
container that fits its contents, holding children that grow into it, is what makes the buttons in
a menu come out the same width without anyone measuring anything.

**`stack` needs an explicit `layer` per child**, and that is deliberate rather than clumsy: the
layout has to be told which things are meant to overlap, and there is no way to guess that from the
inside of a lambda.

### image and progress

```cpp
void image(const Texture2D &texture);            // its own size, scaled with the UI
void image(const Texture2D &texture, const ImageOptions &o);
void progress(float fraction);                   // 0..1, clamped
void progress(float fraction, const ProgressOptions &o);
```

The texture has to stay alive until `end()` returns — Clay keeps the pointer and reads it at draw
time.

> **A field-order rule that will bite you once.** C++20 requires designated initialisers in
> **declaration order**: `{ .width = 8, .tint = RED }` compiles, `{ .tint = RED, .width = 8 }` does
> not. The structs here are ordered the way they are most likely to be written — Sizing, then
> appearance, then identity — but when the compiler complains about "designator order", that is
> what it means.

### Controls that own a value

```cpp
bool checkbox(std::string_view label, bool *value);
bool slider(std::string_view label, float *value, float min, float max);
bool dropdown(std::string_view label, int *selected, const char *const *items, int count);
bool text_input(std::string_view label, char *buffer, int capacity);
```

Each takes a pointer to **your** variable and writes to it. That is the whole state model: nothing
of ours holds a copy, nothing needs synchronising, and what is on screen is what is in your struct
because it was read this frame. They return `true` on the frame the value changed:

```cpp
if (rmp::ui::checkbox("Fullscreen", &cfg.fullscreen)) apply(cfg);
```

`slider` takes `.step` to snap to multiples, `.show_value` to hide the percentage. `dropdown` keeps
its own open/closed flag — that is UI state, not yours. `text_input` writes into your buffer,
NUL-terminated, never past `capacity - 1`.

Options structs carry `.enabled` and `.id`. A control that is unavailable right now should be
disabled rather than missing: a menu whose items appear and disappear is a menu nobody can learn.

### Grid and scroll

```cpp
rmp::ui::grid(4, [&]{
    for (auto &item : inventory)
        rmp::ui::cell([&]{ rmp::ui::image(item.icon); });
});

rmp::ui::scroll([&]{ /* a long list */ });
```

`grid(0, …)` works out its own column count from the width it has and works it out again when that
changes — an inventory that reflows on a phone instead of staying at the number someone typed on a
desktop. Give it an `.id` so it can measure itself; without one it falls back to four.

**Every child of a grid has to be a `cell()`**, the same bargain as `stack()`/`layer()`. The layout
engine wraps text, not elements, so the rows are built as real rows — and counting the items is
what tells the grid when to start one.

`scroll()` clips its contents and moves them with the wheel or with a dragging finger, which is the
same gesture on a phone and needs no branch. It **grows by default**, because a scroll area with no
height clips nothing: if the parent also fits its contents, give one of them a size.

### Focus, keyboard and gamepad

Every interactive control is focusable, in declaration order, and nothing in your code asks for it:

| | |
|---|---|
| Tab / Down / d-pad down / left stick | next control |
| Shift+Tab / Up / d-pad up | previous |
| Enter / Space / gamepad bottom face button | activate |
| Left / Right / d-pad / stick | move a slider |

```cpp
void rmp::ui::focus(std::string_view id);   // when a menu opens
std::string_view rmp::ui::focused();
void rmp::ui::set_navigation_enabled(bool); // if your game drives focus itself
```

Put the focus somewhere when a screen opens. A controller arriving at a screen with nothing
selected presses a button and nothing happens, which reads as "the menu is broken".

**No widget implements navigation.** A widget registers itself as focusable and asks whether it is
the focused one; moving between them, key repeat, and what "activate" means on three input devices
all happen once, in `focus.cpp`. That is why it could be added after the widgets were written
without touching their logic, and why the focus ring is a Theme colour rather than something each
widget decides — a controller build where one widget forgot to draw it is a controller build that
gets stuck.

Navigation resolves against the **previous** frame's list of focusables, for the same reason hit
testing does: this frame's order does not exist until `end()`.

### Who gets the input

```cpp
if (!rmp::ui::wants_pointer()  && IsMouseButtonPressed(0)) shoot();
if (!rmp::ui::wants_keyboard() && IsKeyDown(KEY_W))        walk();
```

The UI reads the pointer and the keyboard itself, so these two are how the game finds out to keep
its hands off. Without the first, the click that presses Pause also fires your weapon. Without the
second, typing a save name walks the player across the level — `wants_keyboard()` is true only
while a text field has the focus.

`wants_pointer()` is true when the pointer is over a control or while a slider is being dragged. It
answers for the previous frame's layout, like everything else here.

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

### Scale — how "responsive" is actually implemented

Layout happens in pixels, and pixels are not a unit you can design in: a 40 px button is 6.7 % of a
600 px screen and 1.9 % of a 2160 px one. So every metric in the Theme is in **design units**, and
everything is multiplied by one number before it is drawn:

```
scale = clamp( min( width / APP_WINDOW_WIDTH, height / APP_WINDOW_HEIGHT ), 0.5, 4.0 )
```

`APP_WINDOW_WIDTH/HEIGHT` come from `[window]` in `raylib_multiplatform.toml`, which gives that
block a second and more useful meaning: **it is the resolution you are designing for**. Declare
800×450 and the UI is drawn as if for 800×450, whatever the window turns out to be.

`min()` and not `max()`: what does not fit is worse than what is left over, so the tighter axis
wins and the interface stays on screen. On a 3840×480 window the height decides.

The clamps stop the two absurd ends — a window too small to read, and a monitor big enough to turn
a button into a billboard.

```cpp
float rmp::ui::scale();          // what it is right now
void  rmp::ui::set_scale(float); // pin it; 0 goes back to automatic
```

Pinning it is how you would build an "interface size" option in a settings menu.

**The built-in font is a bitmap**, so its scale — and only its scale — is rounded to a whole
number. It steps 1×, 2×, 3× instead of sliding, and stays sharp instead of going blurry. A `.ttf`
set in `[ui] font` rasterises at any size, so it keeps the continuous scale and is re-baked when
the size it is asked for changes.

### Breakpoints — the one thing scale cannot do

Scale makes everything bigger or smaller together. It cannot change the **shape** of a layout, and
no combination of `grow`, `fit` and a fixed size turns a row into a column. On a phone held upright
a sidebar-and-content row has to become a column, or it is unusable.

```cpp
if (rmp::ui::compact()) rmp::ui::column([&]{ sidebar(); content(); });
else                    rmp::ui::row   ([&]{ sidebar(); content(); });

switch (rmp::ui::current_breakpoint()) { /* compact, medium, expanded */ }
```

The classification is by **aspect ratio**, not by pixels, and that is deliberate. A pixel threshold
is a lie on a phone — a 1080-pixel-wide screen four inches across is not a desktop — and `scale()`
has already normalised how big everything is. What is left, and the only thing that decides whether
a row still fits, is how wide the viewport is next to how tall it is.

| Breakpoint | Aspect | What it is in practice |
|---|---|---|
| `compact` | < 1:1 | A phone held upright, a narrow window |
| `medium` | < 1.6:1 | A tablet on its side, a small desktop window, 4:3 |
| `expanded` | ≥ 1.6:1 | An ordinary desktop, a TV, a phone on its side |

**Reach for it only when the layout has to become a different layout.** Using a Breakpoint to pick
a *size* is undoing the work `scale()` already did, and it is how a UI ends up looking right on
exactly one machine.

There is no `wrap` flag on `row()` and there is not going to be one: a row that wraps is
`grid({ .columns = 0 })`, which already exists and already recomputes itself as the window changes.

### Theme

Plain data, no logic, no inheritance, no cascade:

```cpp
rmp::ui::Theme t = rmp::ui::current_theme();
t.primary       = GOLD;
t.corner_radius = 0;
rmp::ui::set_theme(t);
```

Colours use raylib's `Color`, because you already have `RED` and `CLITERAL` and a second colour
type would only add conversions. Every metric — `font_size`, `padding_x`, `padding_y`, `gap`,
`panel_padding`, `corner_radius`, `border_width`, `min_touch_size` — is in design units.

**Two themes come with the framework**, and `[ui] Theme` in the `.toml` picks which one the app
starts with. After that it is a runtime call, so an in-game appearance setting is one line:

```cpp
rmp::ui::Theme rmp::ui::theme_dark();    // the default
rmp::ui::Theme rmp::ui::theme_light();

if (rmp::ui::checkbox("Light Theme", &light))
    rmp::ui::set_theme(light ? rmp::ui::theme_light() : rmp::ui::theme_dark());
```

The light Theme is not the dark one with the numbers flipped. It sets `border_width = 1`, and that
single field is why it works: a dark interface separates its surfaces with its own shadows, and a
light one has none, so a pale button on a pale page needs an outline to still be a button. Its
accents are darker than the dark Theme's for the same reason — the blue that reads as bright on
near-black is washed out on near-white, and white label text on it stops being legible.

`min_touch_size` (44 by default) is the floor on a control's height. It is Apple's touch-target
guidance, close to Material's 48 dp, and it is the difference between a menu you can use with a
thumb and one you cannot. Four of the fourteen targets are touch screens.

States are handled for you: `normal`, `hovered`, `pressed`, `focused`, `disabled`. You never ask
where the mouse is.

### Variants and sizes

The two axes you actually style along. Neither of them names a colour, which is the point: the call
site says what the control *means* and how important it is, and the Theme decides what that looks
like. Change the Theme and no call site is revisited.

```cpp
rmp::ui::button("Start game", { .style = rmp::ui::Variant::PRIMARY });
rmp::ui::button("Load");                                              // Variant::NORMAL
rmp::ui::button("Settings",   { .style = rmp::ui::Variant::OUTLINE });
rmp::ui::button("Back",       { .style = rmp::ui::Variant::GHOST   });
rmp::ui::button("Delete",     { .style = rmp::ui::Variant::DANGER  });
rmp::ui::button("Continue",   { .style = rmp::ui::Variant::PRIMARY, .enabled = false });
```

| Variant | What it is for |
|---|---|
| `normal` | Most buttons: a filled surface |
| `primary` | The one thing you want pressed on this screen |
| `danger` | Destructive, and it should look like it |
| `outline` | An outline and a label, no fill until you point at it: a secondary action |
| `ghost` | Just the label. Toolbars, "back" links |

`enabled` is a flag and not a sixth Variant, because being disabled can happen to any of them.

Sizes are three steps in the Theme's type scale, and the same field takes an exact number of design
units when you genuinely need one — a title, not a button:

```cpp
rmp::ui::button("Play",        { .size = rmp::ui::Size::LARGE });
rmp::ui::text  ("v1.2.3",      { .color = rmp::ui::ColorRole::MUTED, .size = rmp::ui::Size::SMALL });
rmp::ui::text  ("CHAPTER ONE", { .size = 44 });
```

One field takes both spellings rather than two fields that could contradict each other. A step
moves the type size, the padding and the minimum touch height **together**, so a large button is
large all over instead of a normal one with bigger letters in it — with one exception: on a touch
screen a small button never drops below `min_touch_size`, because a small button there is still a
button you hit with a thumb.

### Transitions

Controls fade between their states rather than snapping. It is automatic, there is nothing to opt
into, and there is exactly one knob:

```cpp
rmp::ui::Theme t = rmp::ui::current_theme();
t.transition = 0.0f;          // "reduce motion". 0.12 s is the default
rmp::ui::set_theme(t);
```

**Colour only. Nothing about the layout moves.** That is a deliberate limit, not an unfinished
feature: a control that slid into place could be somewhere other than where you aimed, and an
interface that makes you miss what you clicked on is worse than one that does not animate. It also
means transitions cost nothing in the layout pass and are free to skip entirely — with
`transition = 0` there is no state to keep and no table to look in, which is how the headless test
runs deterministically.

### Touch

raylib maps touch to the mouse, so a menu works on Android and iOS with no extra code. Two
adjustments happen underneath:

- **Hover is suppressed when nothing is touching the screen.** A touch device has no pointer at
  rest, so the last place tapped would otherwise stay lit up forever.
- **The safe area is reserved** at the root. `[android.display] into_cutout = true` draws the game
  behind the notch — right for a background, wrong for a menu. The current inset is a conservative
  approximation; real per-device insets need platform code and are a later job.

### Configuration

```toml
[ui]
Theme        = "dark"  # or "light" — only which one the app STARTS with
font         = ""      # "" = raylib's built-in font, or a .ttf in resources/
font_size    = 20      # design units, i.e. at the [window] resolution
scale        = 0       # 0 = automatic
max_elements = 512     # ceiling on the UI tree; it sizes the layout arena
```

`Theme` is validated against the themes that exist, so a typo is a configure error and not a
silent fall back to dark at runtime.

The font goes through `rmp::assets::load_font`, so one packed into the `.rres` works exactly like
a loose one. If it is missing, the UI says so once and falls back to the built-in font — a missing
font must not switch off the interface.

`max_elements` is what sizes the arena, and 512 is generous for menus and HUDs. The engine's own
default is 8192, which would reserve megabytes for three buttons.

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

CLAY_AUTO_ID({ .layout = { .Sizing = { .width = CLAY_SIZING_FIT(0) },
                           .childGap = 10,
                           .layoutDirection = CLAY_LEFT_TO_RIGHT },
               .backgroundColor = { 30, 30, 38, 255 },
               .cornerRadius = CLAY_CORNER_RADIUS(10) }) {
    CLAY(CLAY_IDI("slot", 0), { .layout = { .Sizing = { .width = CLAY_SIZING_FIXED(64) } } }) {}
}

if (rmp::ui::button("Close")) rmp::app::quit();
rmp::ui::end();
```

Your elements join the same tree, are laid out in the same pass and drawn by the same renderer. It
is the same bargain as everywhere else in this framework: `rmp::assets` does not stop you calling
`LoadTexture`, and `rmp::ui` does not stop you calling Clay — or rlgl, or raw OpenGL.

The full worked version, including images and hover, is
[`examples/ui/03_clay_direct.cpp`](examples/ui/03_clay_direct.cpp). CI compiles every example with GCC
**and MSVC** on every run, so that claim cannot quietly stop being true.

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

Our renderer handles `RECTANGLE`, `BORDER`, `TEXT`, `IMAGE` (point `imageData` at a `Texture2D` you
own; `backgroundColor` is the tint) and the `SCISSOR` pair, so clipping and scroll containers work.
`CUSTOM` is not handled — `src/rmp/ui/render.cpp` is ~150 readable lines and
adding a case is the intended way to extend it.

**Why `rmp::ui` does not use the macros internally**, since it is a fair question: `CLAY(...)` is a
`for`-loop block, and this API is a `begin()`/`end()` pair. An element opened inside a block macro
cannot stay open across a function boundary. So `widgets.cpp` calls `Clay__OpenElement`,
`Clay__ConfigureOpenElement` and `Clay__CloseElement` — all public — and fills the structs field by
field, which is also what you want when the values are computed from the Theme and the scale rather
than written as literals. Nothing about the macros is being avoided; they simply do not fit the
shape of this particular API.

The implementation is five files under `src/rmp/ui/`: `clay_impl.cpp` (compiles
the engine once, same idea as `rres_impl.cpp`), `context.cpp` (start-up, scale, font, the text
arena, element identity), `widgets.cpp`, `render.cpp` (draw commands into raylib calls) and
`Theme.cpp`.

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
cmake --build build --target ui_layout_test && ./build/ui_layout_test
```

It checks the menu is centred at four resolutions, that buttons do not overlap and share a width,
that none is shorter than `min_touch_size`, that nothing leaves the screen, and that the scale
clamps at both ends. Those are the things a person verifies once by hand and then never again.

### What is not here yet

Tabs, tooltips, modals, context menus, tree views, drag and drop, and text input beyond the
basics — no selection, no clipboard, no IME, and on mobile no soft keyboard (raymob has one; wiring
it to `text_input` is a job of its own).

Motion beyond colour is deliberately absent rather than pending: see
[Transitions](#transitions) for why a control that moves is a control you can miss.

[Dropping to Clay](#what-is-underneath-and-how-to-use-it-directly) covers a good deal of the
layout half of that list already: floating elements, aspect ratios and per-corner radii all exist
in the engine and our renderer draws what they produce.

---

## `rmp::app` — the entry point and closing the app

`RMP_ENTRY_POINT` and `quit()`. They are in the same namespace because they are
the same subject: who owns the frame loop, and how it ends. Quitting gets most
of the space below because it is one of those problems that looks trivial until
it is on fourteen platforms.

```cpp
void rmp::app::quit();          // ask the app to close
bool rmp::app::quit_requested(); // has it been asked?
```

Call it from anywhere. A menu callback, a game-over screen, ten frames deep in your own code — it
does not need to know where `main()` is, and there is no value to thread back up your call stack.

```cpp
if (rmp::ui::button("Quit")) rmp::app::quit();
```

### What it actually does

It **returns**. All it does is raise a flag, which the entry point checks at the top of the next
iteration. The frame you are in finishes normally, the loop ends, and then `on_exit()` runs,
`CloseWindow()` runs and the asset pack is released — the same shutdown you get by closing the
window with the X.

That is exactly why not to call `std::exit()` from a button handler. `std::exit` ends the process
where it stands: `on_exit()` never runs, `CloseWindow()` never runs, and on Android the Activity is
left behind while its process vanishes underneath it.

### Why not an exception

Throwing out of a button handler is the obvious alternative and it is worse on three counts:
exceptions are switched off in plenty of game builds, unwinding through raylib's C frames is
undefined behaviour, and an exception raised mid-frame leaves `BeginDrawing()` unbalanced and a UI
frame open. A flag costs one branch per frame and cannot leave anything half done.

### Per platform

| Platform | What happens |
|---|---|
| Desktop, BSD, Web | The frame loop ends and `main()` returns |
| Android | The same, **plus `ANativeActivity_finish()`** — without it the process stops but the Activity is left in the recents list pointing at nothing |
| iOS | **Nothing.** It logs a warning and returns — see below |

### iOS does not get to quit

Apple's [QA1561](https://developer.apple.com/library/archive/qa/qa1561/_index.html) is unambiguous:
there is no API for gracefully terminating an iOS app, an app that calls `exit()` "will appear to
the user to have crashed", and App Review rejects anything that crashes or appears to. Worse,
`applicationWillTerminate:` never runs, so unsaved data is lost. A Quit control also fails the
Human Interface Guidelines on its own account.

So `rmp::app::quit()` is inert on iOS by design. The same source ships to all fourteen targets
with its Quit button intact; on iPhone the button does nothing, which is exactly the behaviour
Apple asks for. If a dead control bothers you, hide it:

```cpp
#if !defined(PLATFORM_IOS)
if (rmp::ui::button("Quit")) rmp::app::quit();
#endif
```

The CI smoke test still terminates the simulator, because a bounded test run has to end — but that
path is behind `RAY_TEST_MAX_FRAMES`, an environment variable no shipped app ever sets.

---

## AdMob (Android)

Interstitial + rewarded ads. The API is `rmp::ads`, and it arrives with
`<rmp/ads.h>` — **real on Android**, a **no-op on every other platform**, so no
`#ifdef`s in your game code.

| Function | Purpose |
|---|---|
| `rmp::ads::request_interstitial()` / `is_interstitial_loaded()` / `show_interstitial()` | interstitial |
| `rmp::ads::request_rewarded()` / `is_rewarded_loaded()` / `show_rewarded()` | rewarded |
| `rmp::ads::take_reward_earned()` (true once, then clears) + `reward_amount()` | poll the reward |

They are `inline` wrappers over the C functions in `<admob.h>`, which stay where they are: that
header is the real JNI boundary, and the pure-C entry point has no namespaces to call into.

Configuration is in `raylib_multiplatform.toml`:

```toml
[android.admob]
enabled         = true                   # false removes AdMob from the build entirely
app_id          = "ca-app-pub-...~..."   # AdMob application id (goes into the manifest)
interstitial_id = "ca-app-pub-.../..."   # interstitial ad unit
rewarded_id     = "ca-app-pub-.../..."   # rewarded ad unit
```

The ids default to **Google's official test ids**; replace them before publishing. The app id is
injected into the manifest, the ad units are exposed to Java via `BuildConfig`. Ads are
**Android-only**; iOS ads are not implemented.

### Where it actually lives

Four steps, and each one exists because the one above it cannot do its job:

| Layer | File | What it does |
|---|---|---|
| API | `thirdparty/raymob/admob.h` | the eight functions. Real on Android, inline no-ops elsewhere |
| JNI | `thirdparty/raymob/admob.c` | calls the methods **by name** on the Activity instance |
| Java | `raymob/.../NativeLoader.java` | the eight public methods, kept by `proguard-rules.pro` |
| SDK | `raymob/app/src/{admob,noadmob}/java/.../AdmobBridge.java` | Google Mobile Ads, or nothing |

`NativeLoader` only forwards. The work is in `AdmobBridge`, which exists **twice** — the real one
under `src/admob/java`, a no-op twin under `src/noadmob/java` — and Gradle puts exactly one of them
on the source path. That split is what makes the switch real rather than cosmetic.

### Switching it off

`enabled = false` removes AdMob from the build: no `play-services-ads` dependency, no `AD_ID`
permission, no `APPLICATION_ID` meta-data, no `MobileAds.initialize()` at startup. Your game code
does not change — `<admob.h>` keeps compiling and the calls do nothing, exactly as they already do
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
- The game uses the same `on_ready/on_frame/on_exit` lifecycle; the runner maps it to
  `ios_ready/ios_update/ios_destroy`.
- Building requires **macOS + Xcode** (see the `build-ios` CI job). iOS ads are not implemented.

---

## BSD & RISC-V targets

- **Linux RISC-V**: cross-compiled inside the CI build image using
  `cmake/toolchain-riscv64-linux.cmake` + the image's `riscv64-linux-gnu` toolchain and riscv64
  X11/GL multiarch libraries.
- **FreeBSD / OpenBSD / NetBSD**: built inside QEMU VMs via
  [`cross-platform-actions/action`](https://github.com/cross-platform-actions/action)
  (pinned by SHA). The action syncs the workspace into the VM, builds there, and syncs `build/`
  back so the artifact can be uploaded.
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

### When it runs

| Trigger | What builds |
|---|---|
| push to `main`, pull request | **Fast lane** — Linux x64, Web, Android, Windows x64 (~10 min) |
| tag `v*` | All 14 targets, then Release, then itch.io |
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

The gate is a **ratio, never a pixel hash**: llvmpipe, ANGLE-on-Metal, SwiftShader and mobile
GPUs disagree about text antialiasing and texture filtering, so a hash would be red on half the
matrix for no reason. The hash is logged as a diagnostic only.

> **What the tests still do NOT cover.** BSD, RISC-V and Windows ARM64 are compiled and
> format-checked but never run — there is no runner or emulator for them here. Android is built
> and can optionally be smoke-run on real hardware via Firebase Test Lab (below). iOS is built
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
  `linux-x64`, `linux-arm64`, `windows-x64`, `windows-arm64`, `osx`, `android` and `html5`
  channels get tagged correctly; `linux-riscv64` is uploaded as a plain download because itch
  has no concept of it. The BSD builds are deliberately **not** pushed to itch at all — they
  would be untagged downloads on a storefront with no BSD audience. They are still attached to
  the GitHub Release.

### Firebase Test Lab (real Android hardware)

An emulator inside an unaccelerated runner is slow and fails for reasons unrelated to your
game. Test Lab runs the actual APK on actual hardware, which is the one thing CI cannot
otherwise check.

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
its target to `target_link_libraries`:

```cmake
target_link_libraries("${CMAKE_PROJECT_NAME}" PRIVATE
    raylib_static raymoblib rres yourlib_target
)
```

The target name is whatever the library's own `CMakeLists.txt` defines.

---

## FAQ & troubleshooting

**Q: I changed `PRODUCTION_BUILD` and the build is wrong.**
A: Delete `build/` and reconfigure — CMake caches the value and VS doesn't always notice the change.

**Q: Do I need to ship DLLs?**
A: No. Everything links statically; on MSVC even the CRT is static.

**Q: `file` / arch checks fail in CI?**
A: The static tests use `od` to read the ELF magic + `e_machine` (no `file` dependency in the
build image), so they're portable.

**Q: Why is there no `resources.rres` in my dev build?**
A: It's only produced when you run `cmake --build build --target pack_resources`. In dev the game
loads loose files by default.

**Q: The game shows no texture / "Failed to open file".**
A: Check `RESOURCES_PATH` — in dev it's an absolute path; in production the `resources/` folder
(or `resources.rres`) must sit next to the executable.
