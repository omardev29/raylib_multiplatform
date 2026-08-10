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
build_image_digest        sha256:a79c538bfa11d0732b7dc2994ee8c255acba2d136696e497cbf2ea59b77d9655
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
```

| Dependency | Version | How it is pinned |
|---|---|---|
| raylib | 6.0.0 | Vendored snapshot committed under `thirdparty/raylib/` (see `src/raylib.h` `RAYLIB_VERSION_*`). The exact committed files ARE the pin. |
| raylib-iOS | tag `6.0.3-iOS` (commit `29ce933d`) | Git submodule `thirdparty/raylib-ios`, pinned to that commit, pointing at **our fork** `omardev29/raylib-iOS`. |
| rres | master @ vendor time | Vendored headers committed under `thirdparty/rres/` (`rres.h`, `rres-raylib.h`, `external/`). |
| Build image | digest above | `ghcr.io/omardev29/raylib-build@sha256:…`, built from the [raylib-build-image](https://github.com/omardev29/raylib-build-image) repo. Digest, never `:latest`. |
| Ubuntu / apt (in image) | snapshot `20260801T000000Z` | Every apt source points at `snapshot.ubuntu.com`, so package resolution is frozen in time. |
| CMake / Ninja / Emscripten (in image) | 3.30.3 / 1.12.1 / 3.1.61 | Downloaded with hard-coded sha256; emsdk fetched by commit, not tag. |
| Ninja (macOS/Windows jobs) | 1.13.2 | GitHub release asset + sha256. Replaces `brew install ninja`. |
| XcodeGen | 2.46.0 | GitHub release asset + sha256. Replaces `brew install xcodegen`. |
| Xcode | 26.6 on `macos-26` | Selected explicitly with `xcode-select`, with a guard that fails if the pin is gone. |
| Mesa (Windows render test) | mesa-dist-win 26.1.6 | GitHub release asset + sha256. Test-only; asserted absent from the release zip. |
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
