# raylib_multiplatform

A **C++20** framework for shipping [raylib](https://www.raylib.com/) games to 14 targets from one
codebase — Windows, Linux, macOS, Web, Android, iOS and the three BSDs — with a CI pipeline that
builds them, **boots them, and checks they actually put pixels on screen**.

> [!WARNING]
> **This is experimental only, and it is not for production.** The pipeline is green and the render gates
> are real, but the features are very experimental and not intended to production

---

## Look how little you have to do

This is the whole list. Everything else is generated, pinned or automated, and you should never
need to open it.

| You edit | For |
| --- | --- |
| `src/main.cpp` | Your game. Every `.cpp`/`.c` under `src/` is compiled automatically, subfolders included. |
| `include/` | Your headers. |
| `resources/` | Your assets — images, sounds, fonts, levels, models. |
| `branding/icon.png` | Your app icon. One 1024×1024 PNG. |
| `raylib_multiplatform.toml` | Your name, app ids, which platforms to build, everything else. |

That is the list. You do **not** edit CMakeLists.txt to rename your game, or `gradle.properties`
for Android, or `project.yml` for iOS, or any workflow file to choose platforms. Those are
generated from the config on every build, which is why they cannot drift out of sync with it.

Two paths in there are the framework's, not yours, and they carry the same name so you can tell
at a glance: `include/rmp/` (the headers you include) and `src/rmp/` (the implementation).
Everything else under `src/` and `include/` is yours. You can delete both — see [`examples/plain_c/main.c`](examples/plain_c/main.c), which is a plain C
entry point that keeps the fourteen build targets and none of the runtime layer.

`branding/` is yours too, including the name: the path in `[icon] source` is the only thing that
has to agree with it, so `art/logo.png` is just as valid. If the file is missing the build warns
and keeps whatever icons are already there instead of failing — `python3 tools/configure.py
--make-default-icon` writes a fresh 1024×1024 placeholder if you want one to draw over.

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

If you have [`just`](https://just.systems), there is a `Justfile` with the handful of commands you
end up typing several times a day — and nothing else, so `just --list` stays readable:

```bash
just run       # build if needed, then play
just test      # examples compile, layout is right, the game boots and draws
just rel       # release build
just web       # or android
```

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

[android.permissions]
internet  = false                      # each one shows up on your Play listing
vibration = false

[android.admob]
enabled = false                        # true = ads; also off when android is not a target

[ios]
bundle_id = "com.yourname.yourgame"
deployment_target = "15.6"

[icon]
source = "branding/icon.png"           # one 1024x1024 PNG -> every Android density + iOS AppIcon
adaptive_background = "#3DDC84"

[ui]
font      = ""                         # "" = raylib's built-in font; or a .ttf in resources/
font_size = 20                         # at the [window] resolution; rmp::ui scales from there
scale     = 0                          # 0 = automatic

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
#include <rmp/app.h>                       // the entry point
#include <rmp/ui.h>                        // and whatever else you use

static inline void on_ready()   { InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE); }
static inline void on_frame(float delta) { BeginDrawing(); /* ... */ EndDrawing(); }
static inline void on_exit()    { /* unload */ CloseWindow(); }

RMP_ENTRY_POINT(on_ready, on_frame, on_exit);
```

There is no umbrella header: you include what you use, and each one is a concept you can name —
`rmp/app.h`, `rmp/ui.h`, `rmp/assets.h`, `rmp/ads.h`, `rmp/math.h`, `rmp/config.h`. See
[`examples/platform/03_minimal_includes.cpp`](examples/platform/03_minimal_includes.cpp).

The macro on the last line writes the entry point for whichever platform you are building —
`main()` with a frame loop on desktop and Web, the three callbacks UIKit demands on iOS — and
opens and closes the asset pack around your three functions. Nothing about it is yours to
remember.

### What we add on top of raylib

All of raylib is there, unchanged: `DrawTexture`, `LoadModel`, `IsKeyPressed`, everything. On top
of it this framework adds four small namespaces, all under `rmp::`. They exist because they are
the things every game needs and raylib deliberately does not decide for you.

| Namespace | What it is for |
| --- | --- |
| **`rmp::ui`** | Menus, buttons, text, lists, and the controls a settings screen is made of. Responsive by default: written once, a menu is centred and correctly sized from 800×600 to 4K, on a phone and on a desktop, without your code knowing which. Playable with a mouse, a finger and a controller, for free. |
| **`rmp::assets`** | Loading from `resources/` by name, without caring whether the game is running from loose files or from a packed, encrypted `.rres`. |
| **`rmp::ads`** | Interstitial and rewarded ads. Real on Android, silently nothing everywhere else, so there are no `#ifdef`s in your game. |
| **`rmp::app`** | The entry point, and `quit()`: closing the app cleanly from anywhere, on every platform, including the two where ending the process yourself is wrong. |

A main menu, complete:

```cpp
rmp::ui::begin();
if (rmp::ui::button("Play"))    play();
if (rmp::ui::button("Options")) options();
if (rmp::ui::button("Quit"))    quit();
rmp::ui::end();
```

No coordinates, no sizes, no fonts, no hitboxes, and it does not change when the window does. When
you need structure, containers take their contents as a lambda, so there is no closing call to
forget:

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

You say what a control *means*, never what colour it is — `Variant::PRIMARY`, `Variant::DANGER`,
`Size::LARGE` — so restyling the whole game is one call and not a tour of every call site:

```cpp
rmp::ui::set_theme(rmp::ui::theme_light());     // or theme_dark(), or your own
```

Everything past that — your own Theme, Sizing, scaling, breakpoints, dropping to the layout engine
directly — is optional and costs you nothing until you ask for it.

**The full API of all three is in [TECHNICAL.md](TECHNICAL.md)**; there are working examples of
each in [`examples/`](examples/). Everything under `rmp::` is ours, everything else is raylib's, so
in a file that mixes them you can always tell which is which.

### Assets

Put files in `resources/` and load them by name:

```cpp
Texture2D tex = rmp::assets::load_texture("player.png");
Sound     sfx = rmp::assets::load_sound("jump.wav");
Font      f   = rmp::assets::load_font("ui.ttf", 32);
unsigned char *lvl = rmp::assets::load_data("level1.json", &size);
```

`cmake --build build --target pack_resources` bundles everything into one AES-encrypted
[rres](https://github.com/raysan5/rres) file, which is what a release ships. Without it the game
reads loose files, so you can iterate without repacking, and the same code reads whichever exists.
You do **not** need the paid rrespacker tool; `tools/rres_pack.c` does the packing.

**Plain raylib works too.** `LoadTexture(RESOURCES_PATH "player.png")`, `LoadModel`, `LoadShader`
— all of them read the pack, because opening it also routes raylib's own file loading through it.
`rmp::assets::` is the shorter spelling, not a requirement, and mixing the two is fine.

Two things stay outside that, and both are raylib's design rather than a gap here:

- **`LoadMusicStream`** opens the file itself so it can stream instead of holding the song in
  memory. Ship music as a loose file next to the executable.
- **Subfolders.** Resource names are flat and the packer does not recurse, so `resources/art/x.png`
  is not packed. Keep assets directly in `resources/`; a release build warns you if it finds a
  subfolder.

See [TECHNICAL.md](TECHNICAL.md) for how the pack actually works, the platform-detection macros,
AdMob, and adding third-party libraries.

### If you would rather write plain C

[`examples/plain_c/main.c`](examples/plain_c/main.c) is a complete entry point that includes only `<raylib.h>` — no
none of our headers, no `rmp::` anything, your own `main()`. Copy it over `src/`, delete
`src/rmp/`, and you keep the fourteen build targets, the pinned toolchains, the
generated icons and identifiers, and the release pipeline. You lose the resource pack, which raw
raylib cannot read, and iOS, whose entry point the macro exists to provide.

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
   | --- | --- |
   | `ANDROID_KEYSTORE_BASE64` | `base64 -w0 upload.jks` |
   | `ANDROID_KEYSTORE_PASSWORD` | keystore password |
   | `ANDROID_KEY_ALIAS` | `upload` |
   | `ANDROID_KEY_PASSWORD` | key password |

Tag a release and the signed AAB is attached to it. CI verifies the signature with `jarsigner` and
asserts the signer is not the throwaway key, so a CI-signed bundle can never masquerade as a
publishable one.

**Ads are opt-in.** `[android.admob] enabled = false` removes AdMob from the build completely: no
Google Mobile Ads dependency, no `AD_ID` permission, no SDK init at startup. Your code does not
change — the `<admob.h>` calls stay compilable and do nothing, as they already do everywhere except
Android. It switches itself off too when `android` is not in `[targets]`. Leave it off unless you
actually ship ads: the `AD_ID` permission alone obliges you to declare advertising-id collection in
Play's **Data safety** form.

> [!WARNING]
> **TODO — consent (UMP) is not implemented.** Showing ads to users in the EEA or the UK requires a
> Google-certified consent platform, and this framework does not ship one. With ads on, that traffic
> will be served badly or not at all until you add it. See
> [AdMob](TECHNICAL.md#admob-android) in TECHNICAL.md for what it takes; it is a call and a form,
> not a new dependency.

### iOS

This is the roughest corner of the project, and it is worth being precise about what you get.

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

### Cutting a release

Releases are driven entirely by git tags. There is no version anywhere in the
repo to bump — `v1.2.3` becomes the version name on every platform and the
Android `versionCode`, so there is nothing to forget.

```bash
git tag -a v1.2.3 -m "What changed in this release"
git push origin v1.2.3
```

**Annotated (`-a`), not lightweight.** A lightweight tag is just a moving
pointer; an annotated one is a real object with an author, a date and a message,
and it is what `git describe` and most tooling expect from a release.

The tag must be `vMAJOR.MINOR.PATCH`:

| Tag | versionName | Android versionCode | Release |
| --- | --- | --- | --- |
| `v1.2.3` | `1.2.3` | `1002003` | normal |
| `v1.2.3-rc1` | `1.2.3-rc1` | `1002003` | marked pre-release |
| `v1.2` | rejected — CI fails at config | | |

`versionCode` is `major*1000000 + minor*1000 + patch`, so it only ever increases
as long as your versions do. Minor and patch must stay under 1000; the config
refuses a tag that would break monotonicity, because Play rejects an upload
whose versionCode is not higher than the last one.

A pre-release tag shares its base version's `versionCode` (`v1.2.3-rc1` and
`v1.2.3` are both `1002003`). Fine here — nothing uploads to Play
automatically — but do not hand Play both.

Tagging runs all 14 targets, then publishes. It also runs one extra check the
fast lane skips: **the build is refused while your application id is still
`com.example.*`**, so you cannot accidentally cut your first release under a
placeholder identity you can never change.

Undo a tag you have not published yet:

```bash
git tag -d v1.2.3
git push origin :refs/tags/v1.2.3      # only if you already pushed it
```

Deleting a pushed tag does not delete the GitHub Release it created — remove
that from the Releases page, or with `gh release delete v1.2.3 --cleanup-tag`.

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
| --- | --- |
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

## Layout

```
raylib_multiplatform.toml   your configuration — the only non-code file you edit
src/main.cpp                your game
resources/                  your assets — flat, the pack does not recurse
branding/icon.png           the source for every app icon on every platform
include/rmp/               the framework's headers — app, ui, assets, ads, math, config.
src/rmp/                    its implementation. Not yours; deletable.
tests/smoke_test.h          the CI boot + render hook
tests/ui_layout_test.cpp    layout checks that run with no window (-DBUILD_UI_TESTS=ON)
examples/                   ui/ ads/ assets/ platform/ plain_c/ — read, copy, ignore
Justfile                    the handful of commands you type: just run, just test
tools/configure.py          turns the config into build files
cmake/  raymob/  ios/       CMake, the Android shell, the iOS scaffold — generated or fixed
thirdparty/                 raylib 6.0, raymob, rres, Clay, the raylib-iOS fork
.github/workflows/          ci.yml + one reusable workflow per platform
```

`git status` stays clean after a build: everything generated is git-ignored on purpose.

---

## Licence

MIT — see [LICENSE](LICENSE). Third-party licences are in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
