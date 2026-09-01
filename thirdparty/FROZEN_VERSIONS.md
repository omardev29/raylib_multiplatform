# Frozen dependency versions

These dependencies are **frozen** to guarantee reproducible builds. Do NOT
casually update them — a newer version can change behaviour and introduce bugs.
If an update is genuinely needed, do it deliberately and re-test all targets.

The block below is **machine-readable and enforced**. `tools/versions_check.sh`
parses it and fails CI if any of these values disagrees with the file that
actually controls it (`raymob/app/build.gradle`, `gradle-wrapper.properties`,
the workflow's image pin) — or, inside a container job, with
`/etc/raylib-build-image.json` from the running build image. Documentation that
CI checks is documentation that stays true.

```versions
# key                     value
build_image_digest        sha256:a3b5bc2190612d99000e2048ef36e315b9a97f8dc9cca25e3e66dc96e604b670
android_platform          android-36
android_build_tools       36.0.0
android_ndk               28.2.13676358
android_sdk_cmake         3.31.6
android_compile_sdk       36
android_target_sdk        36
android_min_sdk           24
agp                       8.13.2
gradle                    8.14.5
gradle_sha256             6f74b601422d6d6fc4e1f9a1ab6522f642c2fdcbc15ae33ebd30ba3d7198e854
# Runner images and host toolchains. These live here rather than in the
# workflow YAML for two reasons: the weekly canary overrides them with floating
# values to find out what is about to break, and the autofix agent can only
# push changes to files that are NOT under .github/workflows — GITHUB_TOKEN is
# refused there without the `workflows` permission.
macos_runner              macos-26
xcode                     26.6
ninja_mac                 1.13.2
ninja_mac_sha256          c99048673aa765960a99cf10c6ddb9f1fad506099ff0a0e137ad8960a88f321b
xcodegen                  2.46.0
xcodegen_sha256           4d9e34b62172d645eed6457cac13fc222569974098ef4ee9c3368bedf0196806
windows_runner            windows-2025
mesa                      26.1.6
mesa_sha256               86b506ad38b8dae9d37bdade656a9003518d717bf4ff5475ff3f746e4ee768eb
# The formatter and the linter. Pinned because a different minor version of
# clang-format reformats files that were already formatted, which turns every
# diff into noise and makes `just fmt check` fail for a reason that has nothing
# to do with the change. The lint job reads these two values out of this block
# rather than repeating them, so there is one number and nothing to drift.
# UPX, for [upx] in the .toml. Downloaded at this exact version with the
# checksum below rather than installed from a distro package, because the
# version an image happens to carry is a version nobody chose.
upx                       5.2.0
upx_sha256_amd64          3db5d3294707439db97866feab8d75d800f028f48481a40547411824da4288a1
upx_sha256_arm64          55d48a61e8ffd17152db871c855376cba7f08e830b37799d0947a16dff8ec36c
upx_sha256_win64          b471ebf1b7f20f4a89150264ed9a008a2a5bfd247f3c6d1184a75bb59ca08f5d

# Zig, used ONLY as a C/C++ cross-compiler for the Linux targets, so that the
# shipped binary can ask for an older glibc than the machine that built it.
# Downloaded at this exact version with a checked sha256, the same way upx,
# ninja and butler are — the version an image happens to carry is a version
# nobody chose. See tools/linux_build.sh and [linux] glibc in the .toml.
zig                       0.16.0
zig_sha256_x86_64         70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00
zig_sha256_aarch64        ea4b09bfb22ec6f6c6ceac57ab63efb6b46e17ab08d21f69f3a48b38e1534f17
clang_format              22.1.8
clang_tidy                22.1.8
freebsd                   15.1
openbsd                   7.9
netbsd                    10.1
```

| Dependency | Version | How it is pinned |
|---|---|---|
| raylib | 6.0.0 | Vendored snapshot committed under `thirdparty/raylib/` (see `src/raylib.h` `RAYLIB_VERSION_*`). The exact committed files ARE the pin. |
| raylib-iOS | tag `6.0.3-iOS` (commit `29ce933d`) | Git submodule `thirdparty/raylib-ios`, pinned to that commit, pointing at **our fork** `omardev29/raylib-iOS`. |
| rres | master @ vendor time | Vendored headers committed under `thirdparty/rres/` (`rres.h`, `rres-raylib.h`, `external/`). |
| Clay | 0.14 (one-line patch) | Vendored snapshot committed under `thirdparty/clay/clay.h`. The committed file IS the pin — Clay is pre-1.0 and its API has moved between minor versions, so updating is a decision, never an accident. **Patched:** the C++ version guard accepts `201709L`, because NetBSD's system GCC 10 reports that under `-std=c++20` and would otherwise refuse to build. The patch is commented at the site; re-apply it when bumping. |
| raylib — patch | one branch added | `cmake/LibraryConfigurations.cmake` gains a `PLATFORM=Win32` branch setting `PLATFORM_DESKTOP_WIN32`. raylib 6.0 ships the backend (`platforms/rcore_desktop_win32.c`) and `rcore.c` selects it, but upstream never added a way to ask for it from CMake. Commented at the site. **Re-apply when bumping raylib.** |
| raylib-cpp | 6.0.3 | Vendored snapshot committed under `thirdparty/raylib-cpp/`. **The MATH subset only** — Vector2/3/4, Matrix, Rectangle, Color and the two headers they need. The rest is deliberately absent: its resource wrappers throw `RaylibException` on a failed load and this framework does not throw. Quaternion.hpp is also out, because it redeclares `raylib::Vector4` and collides with Vector4.hpp. |
| doctest | 2.5.0 | Vendored snapshot committed under `thirdparty/doctest/doctest.h`. The committed file IS the pin. Chosen for compile time above all: this builds on fourteen toolchains, three of them BSD in virtual machines. |
| raylib — patch (rcore web) | one branch + one log line | `src/rcore.c` gains `#elif defined(PLATFORM_WEB_EMSCRIPTEN)` in the platform include chain, and a matching `TRACELOG` line. The header of rcore.c itself lists `PLATFORM_WEB_EMSCRIPTEN` as a supported platform and the backend file is right there in `platforms/`, but nothing ever selected it — the build fell through to `#else` and failed at link with undefined `InitPlatform`/`ClosePlatform`/`SwapScreenBuffer`. **Re-apply when bumping raylib.** |
| raylib — patch (USE_GLFW) | one condition | `src/CMakeLists.txt` applied `-sUSE_GLFW=3` to every `PLATFORM` matching `Web`, PUBLIC — so it propagated to the game and the GLFW-free web backends linked the GLFW-in-JavaScript shim anyway, which is most of what choosing them was for. Narrowed to `PLATFORM STREQUAL "Web"`. **Re-apply when bumping raylib.** |
| raylib — patch (enum) | two values added | `CMakeOptions.txt`'s `enum_option(PLATFORM ...)` gains `WebEmscripten` and `Win32`. It FATAL_ERRORs on anything not listed, so adding a branch in `LibraryConfigurations.cmake` is only half of shipping a backend — `[web] backend = "emscripten"` failed on exactly this, and `[windows] backend = "win32"` had the same hole and nobody had hit it because the .toml says rgfw. `tests/configure_test.py` now reads both lists and compares them. **Re-apply when bumping raylib.** |
| raylib — patch (web) | one branch added | `cmake/LibraryConfigurations.cmake` gains a `PLATFORM=WebEmscripten` branch setting `PLATFORM_WEB_EMSCRIPTEN`. Same situation as the Win32 one: raylib 6.0 ships the backend (`platforms/rcore_web_emscripten.c`) and `rcore.c` selects it, but there is no way to ask for it from CMake. Drives `[web] backend` in the .toml. Commented at the site. **Re-apply when bumping raylib.** |
| raylib — patch (RGFW) | one line | `cmake/LibraryConfigurations.cmake`'s RGFW branch links only X11 and GL upstream, but RGFW's X11 backend calls Xrandr, Xcursor and Xi — so `PLATFORM=RGFW` does not link at all on Linux. Commented at the site. **Re-apply when bumping raylib.** |
| Build image | digest above | `ghcr.io/omardev29/raylib-build@sha256:…`, built from the [raylib-build-image](https://github.com/omardev29/raylib-build-image) repo. Digest, never `:latest`. |
| Ubuntu / apt (in image) | snapshot `20260801T000000Z` | Every apt source points at `snapshot.ubuntu.com`, so package resolution is frozen in time. |
| CMake / Ninja / Emscripten (in image) | 3.30.3 / 1.12.1 / 3.1.61 | Downloaded with hard-coded sha256; emsdk fetched by commit, not tag. |
| Ninja (macOS/Windows jobs) | 1.13.2 | GitHub release asset + sha256. Replaces `brew install ninja`. |
| XcodeGen | 2.46.0 | GitHub release asset + sha256. Replaces `brew install xcodegen`. |
| Xcode | 26.6 on `macos-26` | Selected explicitly with `xcode-select`, with a guard that fails if the pin is gone. |
| Mesa (Windows render test) | mesa-dist-win 26.1.6 | GitHub release asset + sha256. Test-only; asserted absent from the release zip. |
| clang-format / clang-tidy | 22.1.8 | Installed from PyPI at the exact version in the block above, which the `lint` job reads out of this file. `tools/versions_check.sh` then compares the pin against the binary that is actually on PATH, so a local formatter that disagrees with CI is reported before it produces a diff. |
| butler (itch.io) | 15.24.0 | Downloaded from `broth.itch.zone` at that exact version. |
| Playwright | see `package-lock.json` | `npm ci`; the Chromium build is keyed to the Playwright version. |
| GitHub Actions | full commit SHAs | Every `uses:` in `.github/workflows/`, with the tag in a trailing comment. |

## Notes

- `thirdparty/raylib` is a committed copy (not a submodule) because the template
  applies small local adjustments to it. Its contents are the frozen 6.0.0 source.
- `thirdparty/raylib-ios` is used **only** for the iOS target (Xcode/xcframework
  build). Desktop/Android/Web keep using `thirdparty/raylib`.
- The Android versions are a compatibility **set**, not independent knobs: AGP
  8.13 requires Gradle ≥ 8.13 and JDK 17 and caps at API 36.1. Bump them
  together. Do not move to AGP 9 without first rewriting
  `raymob/app/build.gradle`: Gradle 9 removes `gradle.buildFinished` and AGP 9
  removes `applicationVariants`, and that module uses both.
- `play-services-ads` is intentionally **not** in the enforced block. An AAR
  compiled against a lower SDK is always fine under a higher `compileSdk`, so it
  moves on its own schedule and can be bisected independently.
