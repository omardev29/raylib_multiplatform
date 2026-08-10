# Technical reference

How this template works, in depth. For the quick-start see [README.md](README.md).

## Table of contents

- [Directory layout](#directory-layout)
- [Build system](#build-system)
- [Editor / clangd (LSP)](#editor--clangd-lsp)
- [Compile-time definitions](#compile-time-definitions)
- [Resources: `RESOURCES_PATH` and rres](#resources-resources_path-and-rres)
- [Game lifecycle (Godot style)](#game-lifecycle-godot-style)
- [AdMob (Android)](#admob-android)
- [Web export](#web-export)
- [Android (raymob)](#android-raymob)
- [iOS](#ios)
- [BSD & RISC-V targets](#bsd--risc-v-targets)
- [CI/CD pipeline](#cicd-pipeline)
- [Adding source files & libraries](#adding-source-files--libraries)
- [FAQ & troubleshooting](#faq--troubleshooting)

---

## Directory layout

```
.
├── CMakeLists.txt            # Root build: links raylib statically, sets presets, tests, rres
├── CMakePresets.json         # debug / release / web profiles
├── .clangd                   # clangd config (LSP) — host by default; update_clangd.* = Android opt-in
├── src/                      # YOUR code. Every .cpp here is auto-compiled (GLOB_RECURSE).
│   ├── main.cpp              # lifecycle runner + your game
│   ├── assets.cpp            # rres-backed asset layer (Assets::*)
│   ├── assets_rres.c         # rres implementation TU (C)
│   └── md5.c                 # MD5 for rres AES integrity
├── include/                  # YOUR headers (already on the include path)
│   ├── assets.h              # asset-layer API
│   └── test.h                # sample asset struct (replace with your game)
├── tests/
│   └── smoke_test.h          # CI smoke-test hook (RAY_TEST_MAX_FRAMES), header-only
├── resources/                # Game assets (auto-packed by the `pack_resources` target)
│   └── rabbit.png            # sample asset
├── tools/
│   └── rres_pack.c           # open rres packer (AES-256) — no paid tooling needed
├── cmake/
│   └── toolchain-riscv64-linux.cmake   # cross-toolchain for Linux RISC-V
├── raymob/                   # Android app shell (Gradle). See "Android (raymob)".
├── ios/                      # iOS app scaffold (XcodeGen). See "iOS".
├── .github/
│   ├── workflows/build.yaml  # multiplatform CI (14 targets) + release
│   └── scripts/web_boot_test.js  # Playwright web boot test
└── thirdparty/
    ├── raylib/               # raylib 6.0 (frozen, slightly patched for this template)
    ├── raylib-ios/           # raylib-iOS fork (submodule, tag 6.0.3-iOS) — iOS only
    ├── raymob/               # raymob C sources (Android native bridge + admob)
    └── rres/                 # rres.h + rres-raylib.h + externals (aes/monocypher/lz4/qoi)
```

`examples/` holds small reference examples of the template's own features
(lifecycle, AdMob, raymob mobile API, asset loading). They are **not** compiled
by the build — read them and copy what you need into `src/`.

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

```cpp
Texture2D tex = LoadTexture(RESOURCES_PATH "player.png");  // raw path form
```

---

## Resources: `RESOURCES_PATH` and rres

`RESOURCES_PATH` is an absolute path in development (run from any CWD) and `"./resources/"`
in production (must sit next to the executable).

Assets can be packed into a single `resources.rres` container
([rres](https://github.com/raysan5/rres)), **optionally AES-256 encrypted**:

```bash
cmake --build build --target pack_resources    # -> resources/resources.rres
cmake --build build --target unpack_resources  # remove the pack
```

At startup `Assets::Init()` auto-detects: if `resources.rres` exists it loads from it,
otherwise it falls back to loose files. **No code change** needed to switch.

- The packer is the open **`tools/rres_pack`** (built as a CMake target). It's compatible with
  the official `rrespacker` output if you own it. Format: AES-256-CTR, key = Argon2i(password, salt)
  (16 MiB / 3 passes / 1 lane), MD5 integrity per resource.
- Password: `-DRRES_PACK_PASSWORD="..."` (default `raylib-template`).

> **Security note:** the password is embedded in the binary, so this is **obfuscation, not real
> security**. Encryption adds only *load-time* cost (Argon2i per resource), never per-frame cost.

---

## Game lifecycle (Godot style)

Your game lives in three functions in `src/main.cpp`:

| Hook | When |
|---|---|
| `_ready()` | once at startup — window, assets, preload |
| `_process()` | every frame |
| `_exit()` | once at shutdown — unload |

A small platform runner drives them:
- **Desktop / BSD / Android / Web** (`-s ASYNCIFY`): a classic `while (!WindowShouldClose())` loop.
- **iOS**: callbacks `ios_ready` / `ios_update` / `ios_destroy` (iOS has no blocking main loop;
  the OS drives the frame via `CADisplayLink`).

This is what lets the **same game code** run on every platform, including iOS.

A CI smoke-test hook is built in: set the env var `RAY_TEST_MAX_FRAMES=N` and the game renders
N frames then exits with code 0, printing `RAY_TEST_BOOT_OK` and `RAY_TEST_DONE_FRAMES`. The CI
uses this for the runtime tests. The hook itself lives in **`tests/smoke_test.h`** (header-only,
so it compiles on every target without wiring extra sources); `src/main.cpp` only calls
`SmokeTest_Begin()` / `SmokeTest_ReportBoot()` / `SmokeTest_Tick()`.

---

## AdMob (Android)

Interstitial + rewarded ads via raymob. Include `<admob.h>` and call the API — it's **real on
Android** and a **no-op on every other platform**, so no `#ifdef`s in your game code.

| Function | Purpose |
|---|---|
| `RequestInterstitialAd()` / `IsInterstitialAdLoaded()` / `ShowInterstitialAd()` | interstitial |
| `RequestRewardedAd()` / `IsRewardedAdLoaded()` / `ShowRewardedAd()` | rewarded |
| `TakeRewardEarned()` (returns once & clears) + `GetRewardAmount()` | poll the reward |

Configuration is in `raymob/gradle.properties`:

```properties
admob.app_id=ca-app-pub-...~...          # AdMob application id (manifest)
admob.interstitial_id=ca-app-pub-.../... # interstitial ad unit
admob.rewarded_id=ca-app-pub-.../...     # rewarded ad unit
```

These default to **Google's official test ids**; replace them before publishing. The app id is
injected into the manifest, the ad units are exposed to Java via `BuildConfig`. The Google Mobile
Ads SDK (`play-services-ads`, pinned) is added automatically. Ads are **Android-only**; iOS ads
are not implemented.

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

App identity (`app.name`, `app.application_id`, `app.native_library_name`) is set in
`raymob/gradle.properties`. The build renames the Java package + native lib accordingly.

### Publishing to Google Play (signed AAB)

Google Play requires a **signed Android App Bundle (AAB)** — not an APK, and not the debug
build. The template produces it via `./gradlew bundleRelease` plus an env-driven signing
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

3. **CI (automatic).** Add four **repository secrets** with those exact names
   (*Settings → Secrets and variables → Actions*), where `ANDROID_KEYSTORE_BASE64` is
   `base64 -w0 upload-keystore.jks`. Every push then builds and static-tests the signed AAB
   (artifact `android-release-aab`), and tag pushes attach it to the Release. If the secrets
   aren't set, those steps are skipped and everything else still builds.

> **Never commit the keystore or its passwords.** And before publishing, replace the AdMob
> test ids in `raymob/gradle.properties` and set your real `app.application_id`.

---

## iOS

iOS uses the **`raylib-iOS` fork** (submodule `thirdparty/raylib-ios`, frozen at tag `6.0.3-iOS`)
because upstream raylib has no iOS backend. The app scaffold is in `ios/` (XcodeGen `project.yml`).

- The fork builds `raylib.xcframework` (device + simulator) via
  `thirdparty/raylib-ios/projects/scripts/build-ios-xcframework.sh`.
- Graphics go through **ANGLE** (OpenGL ES → Metal), bundled in the fork.
- The game uses the same `_ready/_process/_exit` lifecycle; the runner maps it to
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

`.github/workflows/build.yaml` builds **14 targets** and cuts a Release on tag push:

- **Linux jobs** run **inside a frozen Docker build image** (`container:`), so toolchains are
  pinned and nothing is downloaded at job time. The image is built from its **own repo**
  (`raylib-build-image`: `Dockerfile` + `docker-image.yaml` → GHCR). `BUILD_IMAGE` at the top of
  `build.yaml` points at the shared public image `ghcr.io/omardev29/raylib-build:latest` — **you
  can use it as-is** (it's public). If you prefer full control you can build & publish your own and
  point `BUILD_IMAGE` at it, but that's usually unnecessary.
- **Windows / macOS / BSD** jobs run on native runners. Windows uses MSVC; BSDs use QEMU VMs.
- **Reproducibility:** every third-party action is pinned by full commit SHA; raylib (6.0) and
  raylib-iOS (`6.0.3-iOS`) are frozen. See `thirdparty/FROZEN_VERSIONS.md`.
- **Google Play AAB (opt-in):** if the repo defines the four `ANDROID_KEYSTORE_*` secrets, the
  Android job also builds, static-tests and uploads a **signed release AAB**
  (`android-release-aab`), and tag pushes attach it to the Release. Without the secrets those
  steps are skipped. See [Publishing to Google Play (signed AAB)](#publishing-to-google-play-signed-aab).
- **Tests (two levels):**
  - *Static* — every target: binary exists, is executable, and has the right format/architecture
    (ELF `e_machine`, PE/Mach-O, web artifacts, APK contents).
  - *Runtime* — Linux x64/ARM64 boot headless under `xvfb` with `RAY_TEST_MAX_FRAMES`; Web boots
    in **headless Chromium via Playwright** (`.github/scripts/web_boot_test.js`) and checks the
    engine initialised a canvas; Windows boots and verifies boot + asset load.
- On a tag push the `release` job downloads **all** artifacts (including the BSDs) and publishes a
  Release. It only runs when every required build succeeded.

> **What the tests do NOT cover.** These tests prove each target *builds* and (where feasible)
> *boots* — they are **not end-to-end**. The BSD jobs in particular only compile the sample inside a
> headless VM; they never open a window or run your real game. macOS/iOS/Android are likewise only
> built (no device/emulator run). **You must test your actual game on each platform you ship**, and
> add your own tests if you need stronger guarantees.

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

- **Android:** Google Play requires new apps/updates to target an API level within ~1 year of the
  latest (e.g. 35 in 2025, 36 in 2026…). The template currently targets SDK 34. To keep publishing
  you bump `compileSdk`/`targetSdk` in `raymob/app/build.gradle`, plus the NDK, AGP, Gradle and Java
  versions to compatible ones. The native raylib/raymob code usually needs no change.
- **iOS:** Apple requires submission with a recent Xcode. The `raylib-iOS` fork (and its bundled
  ANGLE) must be updated to build against the new SDK/Xcode. Watch the fork for releases.
- **Note:** the frozen image pins today's SDKs. When you bump SDKs you may also rebuild the build
  image (for Android SDK/NDK) — the image repo is the single place that changes.

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
