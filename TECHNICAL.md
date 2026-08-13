# Technical reference

How this template works, in depth. For the quick-start see [README.md](README.md).

## Table of contents

- [Directory layout](#directory-layout)
- [Build system](#build-system)
- [Editor / clangd (LSP)](#editor--clangd-lsp)
- [Compile-time definitions](#compile-time-definitions)
- [Platform detection macros](#platform-detection-macros)
- [Resources: `RESOURCES_PATH` and rres](#resources-resources_path-and-rres)
- [Game lifecycle (Godot style)](#game-lifecycle-godot-style)
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
├── CMakeLists.txt            # Root build: links raylib statically, presets, rres
├── CMakePresets.json         # debug / release / web profiles
├── src/                      # YOUR code. Every .cpp here is auto-compiled (GLOB_RECURSE).
│   ├── main.cpp              # lifecycle runner + your game
│   ├── assets.cpp            # rres-backed asset layer (Assets::*)
│   └── assets_rres.c         # rres implementation TU (C)
├── include/                  # YOUR headers (already on the include path)
│   ├── assets.h              # asset-layer API
│   ├── raylib_multi.h        # the umbrella header + lifecycle macros
│   └── test.h                # sample asset struct (replace with your game)
├── tests/smoke_test.h        # CI boot + render hook (RAY_TEST_MAX_FRAMES), header-only
├── resources/                # Your assets, and icon.png (the source for every app icon)
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
├── ios/                      # iOS scaffold. project.yml is GENERATED.
├── .github/
│   ├── workflows/ci.yml      # orchestrator: triggers, pins, job graph
│   ├── workflows/canary.yml  # weekly build against floating versions
│   ├── workflows/autofix.yml # agent dispatched onto the runner that broke
│   ├── workflows/_*.yml      # one reusable workflow per platform group
│   ├── known-breakage.md     # canary failures we have seen and chosen not to chase
│   └── scripts/              # web boot test, canary triage, upstream report
└── thirdparty/
    ├── raylib/               # raylib 6.0 (frozen, slightly patched for this template)
    ├── raylib-ios/           # raylib-iOS fork (submodule) — iOS only
    ├── raymob/               # raymob C sources (Android native bridge + admob)
    ├── rres/                 # rres.h + rres-raylib.h + externals
    └── FROZEN_VERSIONS.md    # every pin, machine-readable and CI-enforced
```

**Generated, and never committed** — `cmake/generated/`, `include/generated/app_config.h`,
`raymob/generated.properties`, `ios/project.yml`, `ios/Assets.xcassets/` and the Android
`mipmap-*` icons. They are rebuilt from `raylib_multiplatform.toml` on every configure, which is
why they cannot drift out of sync with it. `cmake --preset debug` produces all of them; Gradle and
XcodeGen never invoke CMake, so those two jobs run `python3 tools/configure.py` explicitly.

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

Example — this is exactly how `src/main.cpp` picks the runner, and how `include/raylib_multi.h`
pulls in `<raymob.h>`:

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
- For anything the template's own headers gate (raymob/admob), match them with `__ANDROID__`.

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

Configuration is in `raylib_multiplatform.toml`:

```toml
[android.admob]
app_id          = "ca-app-pub-...~..."   # AdMob application id (goes into the manifest)
interstitial_id = "ca-app-pub-.../..."   # interstitial ad unit
rewarded_id     = "ca-app-pub-.../..."   # rewarded ad unit
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
   throwaway signature, and vice versa. (`unzip -l | grep META-INF/.*\.SF`, which the template
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

Set these on the repository, and tag pushes publish automatically:

| Key | Type |
|---|---|
| `ITCH_USER` | Variable — your itch.io username |
| `ITCH_GAME` | Variable — the game's URL slug |
| `BUTLER_API_KEY` | Secret — from <https://itch.io/user/settings/api-keys> |

Without them the job logs a warning and skips; cloning this template must not give you a red
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

| Key | Type |
|---|---|
| `FIREBASE_PROJECT` | Variable — the GCP project id |
| `GCP_SA_KEY` | Secret — the service-account JSON, whole file |
| `FIREBASE_DEVICE` | Variable, optional — Google retires device models every few years |
| `FIREBASE_RESULTS_BUCKET` | Variable, optional — a GCS bucket to pull results back from |

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
  year of the latest — **API 36 from 2026-08-31**, which is what the template targets today. To
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
