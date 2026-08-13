# raylib_multiplatform

A **C++20** template for shipping [raylib](https://www.raylib.com/) games to 14 targets from one
codebase — Windows, Linux, macOS, Web, Android, iOS and the three BSDs — with a CI pipeline that
builds them, **boots them, and checks they actually put pixels on screen**.

> [!WARNING]
> **This is experimental and not fully battle-tested.** The pipeline is green and the render gates
> are real, but no game has shipped on it yet. Some corners are known-rough and labelled as such
> below (the iOS simulator test is switched off; BSD builds against rolling package mirrors). Read
> the "What CI does and does not cover" section before you rely on it for a release.

Based on [meemknight/raylibCmakeSetup](https://github.com/meemknight/raylibCmakeSetup).

---

## Look how little you have to do

Three things are yours. Everything else is generated, pinned or automated, and you should never
need to open it.

| You edit | For |
|---|---|
| `src/` | Your game. Every `.cpp`/`.c` in here is compiled automatically, subfolders included. |
| `include/` | Your headers. |
| `raylib_multiplatform.toml` | Your name, app ids, icon, which platforms to build, everything else. |

That is the list. You do **not** edit CMakeLists.txt to rename your game, or `gradle.properties`
for Android, or `project.yml` for iOS, or any workflow file to choose platforms. Those are
generated from the config on every build, which is why they cannot drift out of sync with it.

If you want CD you will also add a few **secrets** on GitHub — never in the repo. See
[Publishing](#publishing).

---

## Quick start

```bash
git clone <your repo>          # or click "Use this template"
cmake --preset debug           # configures AND generates everything from the .toml
cmake --build build
./build/ray_test
```

On Linux you need the X11 development libraries:

```bash
sudo apt install libx11-dev libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev
```

You need **Python 3.11+** on PATH. It is what turns the config into build files; CMake calls it
for you, so there is no separate step to remember.

Then open `raylib_multiplatform.toml`, set your name and app ids, and build again.

---

## The config file

Everything below has a sensible default. Delete the whole file and the project still builds — with
a warning telling you what it assumed.

```toml
[project]
name = "my_game"                       # binary, CMake target, Xcode scheme, artifact names

[window]
title  = "My Game"
width  = 800
height = 450
orientation = "landscape"              # applied to Android and iOS at once

[targets]
enabled  = ["all"]                     # groups: all desktop mobile linux windows apple bsd web android
disabled = []                          # or exact ids: linux-x64, netbsd-x64, ios, ...

[android]
application_id = "com.yourname.yourgame"
min_sdk = 24
gl_version = "ES30"

[ios]
bundle_id = "com.yourname.yourgame"
deployment_target = "15.6"

[icon]
source = "resources/icon.png"          # one 1024x1024 PNG -> every Android density + iOS AppIcon
adaptive_background = "#3DDC84"

[raylib]
disabled_modules = []                  # rshapes | rmodels | raudio — shrink the binary

[dev]                                  # local development only; CI is unaffected
compiler = "clang"
linker   = "auto"

[deploy.itch]
user = ""                              # empty = do not publish
game = ""
```

The file in the repo is fully commented — read that rather than this summary.

**There is no `version`.** It comes from the git tag: `v1.2.3` becomes the version name everywhere
and the Android `versionCode`. Untagged builds are `0.0.0-dev`. One source, nothing to bump twice.

### Picking platforms

`enabled` expands groups and deduplicates; `disabled` is subtracted from the result. There is no
precedence to reason about because there are no conflicts:

```toml
enabled  = ["all"]
disabled = ["ios"]        # everything except iOS

enabled  = ["desktop", "web"]
```

Turning a platform off removes it from the build, the tests and the release.

### Turning raylib modules off

`rcore` and `rlgl` are mandatory. `rshapes`, `rmodels` and `raudio` can go.

`rtextures` and `rtext` cannot, and the config will tell you why if you try: the asset layer, the
rres loader and the CI render gate are all built on them, and `rres-raylib.h` calls into `rtext`
from a code path that is always live — no amount of dead-code elimination drops it.

On a release build the size win is smaller than you would expect, because LTO and `--gc-sections`
already strip what you do not call. The real gains are on Web and Android, and in compile time.
**It does not apply to iOS**, which links a prebuilt xcframework.

---

## Writing your game

The lifecycle is Godot-shaped, and the same three functions run on every platform including iOS,
where the OS owns the run loop:

```cpp
#include <raylib_multi.h>

inlining void _ready()   { /* InitWindow is already done for you */ }
inlining void _process(float delta) { BeginDrawing(); /* ... */ EndDrawing(); }
inlining void _exit()    { /* unload */ }

RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY;
```

Assets go in `resources/`. `cmake --build build --target pack_resources` packs them into a single
AES-encrypted [rres](https://github.com/raysan5/rres) file; without it the game reads loose files,
so you can iterate without repacking.

See [TECHNICAL.md](TECHNICAL.md) for the asset layer, the platform-detection macros, AdMob, and
adding third-party libraries.

---

## Publishing

Nothing below ever goes in the repo. Everything is a GitHub **secret** except where noted.

### Android

> [!CAUTION]
> **Your `application_id` is permanent.** Once an app is published on Google Play under an
> application id, it can never be changed — not renamed, not migrated. Getting it wrong means a new
> listing and losing every install and review. CI refuses to build a tag while it still says
> `com.example.*`, but only you know whether `com.yourname.yourgame` is the one you want to live
> with.

The APK builds with no setup. The **AAB** — what Play actually accepts — is built, signed and
verified on every run too, but with a throwaway key, and the artifact is named
`*-NOT-FOR-PLAY.aab` and deliberately left out of the release. To sign for real:

1. Create an upload keystore once, and keep it somewhere you will not lose it:
   ```bash
   keytool -genkey -v -keystore upload.jks -keyalg RSA -keysize 2048 -validity 10000 -alias upload
   ```
   Enable **Play App Signing** in the Play Console so Google can reset it if you do lose it.
2. Add four repository secrets:

   | Secret | Value |
   |---|---|
   | `ANDROID_KEYSTORE_BASE64` | `base64 -w0 upload.jks` |
   | `ANDROID_KEYSTORE_PASSWORD` | keystore password |
   | `ANDROID_KEY_ALIAS` | `upload` |
   | `ANDROID_KEY_PASSWORD` | key password |

Tag a release and the signed AAB is attached to it. CI verifies the signature with `jarsigner` and
asserts the signer is not the throwaway key, so a CI-signed bundle can never masquerade as a
publishable one.

### iOS

This is the roughest corner of the template, and it is worth being precise about what you get.

CI produces two things: `raylib.xcframework` (the engine, built from the pinned fork) and a
**simulator `.app`**. Neither is installable on a physical iPhone, and no amount of CI will change
that — Apple requires a signed `.ipa`, and signing requires a paid Apple Developer account, a
provisioning profile and a certificate that cannot live in a public repo.

What the `.app` **is** good for: dropping onto a running simulator to check your game works.

```bash
# from a release asset, or ios/dd/Build/Products/Debug-iphonesimulator/ after a local build
unzip ios-app-simulator.zip
xcrun simctl boot "iPhone 16"
open -a Simulator
xcrun simctl install booted my_game.app
xcrun simctl launch --console booted com.yourname.yourgame
```

To get onto a real device you need a Mac and Xcode:

```bash
python3 tools/configure.py     # generates ios/project.yml
cd ios && xcodegen generate
open my_game.xcodeproj
```

Then set your team in the config rather than clicking around in Xcode, so it survives
regeneration:

```toml
[ios.settings]
DEVELOPMENT_TEAM = "ABCDE12345"
CODE_SIGN_STYLE  = "Automatic"
```

Select your device and press Run. For TestFlight and the App Store, archive from Xcode — that path
is deliberately not automated here, because it needs credentials that should not be in CI.

### itch.io

Set `user` and `game` under `[deploy.itch]` in the config, add `BUTLER_API_KEY` as a secret
(from <https://itch.io/user/settings/api-keys>), and tag pushes publish automatically. Leave them
empty and the job skips with a warning.

Channels are per platform, and itch infers the OS from the channel name. HTML5 is uploaded as a
directory with `index.html` at the root, so it is playable in the browser rather than a zip
somebody has to download — a detail that is very easy to get wrong by hand.

### Firebase Test Lab (optional)

Runs the debug APK on **real Android hardware**. An emulator inside an unaccelerated runner is slow
and fails for reasons that have nothing to do with your game.

Set `project_id` under `[deploy.firebase]`, add `GCP_SA_KEY` (the service-account JSON), and:

1. Enable Blaze billing. Test Lab's free Spark quota no longer exists.
2. `gcloud services enable testing.googleapis.com toolresults.googleapis.com`
3. Grant the service account `roles/cloudtesting.testAdmin` and `roles/cloudtoolresults.testAdmin`.

Only the debug APK is ever submitted: the robo test crawls the UI and will click ad banners, and
the debug build uses Google's official test ad units.

---

## What CI does and does not cover

Push and pull requests get a **fast lane** — Linux x64, Web, Android and Windows x64, about ten
minutes. Tags build all 14 targets and then release.

**It really renders.** Booting proves the window opened and the assets loaded, and nothing more; a
broken shader or a lost texture binding still boots and still exits 0. So the game is started, a
frame is read back, and the fraction of pixels differing from the background has to be in range:

| Target | How |
|---|---|
| Linux x64 / ARM64 | headless under `xvfb` |
| Windows x64 | on the real runner, with Mesa's software rasteriser next to the `.exe` |
| Web | headless Chromium; the composited canvas is screenshotted and measured |

That check earns its keep: it is how we found that **iOS had been shipping with no textures at
all** — the app booted, drew its text, and every `LoadTexture` returned 0x0, because nothing had
ever actually run the app.

**Not covered, and you should not assume otherwise:**

- **BSD, RISC-V and Windows ARM64** are compiled and format-checked but never executed. There is no
  runner for them.
- **iOS** is built and statically verified (the bundle must carry a readable identifier and its
  `resources/`), but the runtime test is **switched off**: the hosted simulator does not boot
  reliably under `simctl`. Set `vars.IOS_SIMULATOR_TEST=true` to try it.
- **Android** is built, and optionally smoke-run on real hardware if you configure Test Lab.
- Nothing here tests *your game*. Test it on the platforms you ship.

**What is pinned:** the build image by digest (not a tag), every apt package by an Ubuntu snapshot
timestamp, every download by sha256, the runner images and Xcode explicitly, every GitHub Action by
commit SHA. `tools/versions_check.sh` fails CI when any of it drifts apart.

The honest exception is **BSD**: the QEMU images and the `pkg`/`pkgsrc` mirrors are both rolling and
neither project runs a snapshot service, so those jobs install whatever the mirror serves that day.
If a BSD job fails for no reason you caused, suspect that first.

---

## The canary, and the thing that fixes itself

Pinning protects you from change; it does not tell you change happened. Left alone, this pipeline
would stay green on Xcode 26.6 while the world moved to 27, and you would find out the day you
finally needed to bump it.

So every Monday a **canary** builds everything against the versions we are deliberately *not*
pinned to — the `:latest` image, `macos-latest` with the newest Xcode, `windows-latest` with the
newest Mesa — using the same workflows the real pipeline uses. When it breaks it produces a report
that leads with *what moved*, because "a build broke" is a ticket and "Xcode went to 27" is a fix.
It only ever runs on the template's own repository, so a project made from this template never sees
it.

If a key is configured, an **agent** then gets dispatched onto the runner of the family that broke —
macOS for an Xcode problem, the build container for a Linux one — so it can compile and test rather
than guess, and it opens a draft PR. Two guardrails make that safe to leave running, and neither is
a polite instruction: `GITHUB_TOKEN` physically cannot modify anything under `.github/workflows`,
and `tools/versions_check.sh` fails the PR if a version is bumped in one place and not another. It
is entirely optional and skips cleanly when no key is set.

---

## Layout

```
raylib_multiplatform.toml   your configuration — the only non-code file you edit
src/  include/              your game
resources/                  your assets, plus icon.png
tests/smoke_test.h          the CI boot + render hook
tools/configure.py          turns the config into build files
cmake/  raymob/  ios/       CMake, the Android shell, the iOS scaffold — generated or fixed
thirdparty/                 raylib 6.0, raymob, rres, the raylib-iOS fork
.github/workflows/          ci.yml + one reusable workflow per platform
```

`git status` stays clean after a build: everything generated is git-ignored on purpose.

---

## Licence

MIT — see [LICENSE](LICENSE). Third-party licences are in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
