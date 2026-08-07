# Frozen dependency versions

These third-party dependencies are **frozen** to guarantee reproducible builds.
Do NOT casually update them — a newer version can change behavior and introduce
bugs. If an update is genuinely needed, do it deliberately and re-test all targets.

| Dependency | Version | How it is pinned |
|---|---|---|
| raylib | 6.0.0 | Vendored snapshot committed under `thirdparty/raylib/` (see `src/raylib.h` `RAYLIB_VERSION_*`). The exact committed files ARE the pin. |
| raylib-iOS (ghera fork) | tag `6.0.3-iOS` (commit `29ce933d`) | Git submodule `thirdparty/raylib-ios`, pinned to that commit. |
| rres | master @ vendor time | Vendored headers committed under `thirdparty/rres/` (`rres.h`, `rres-raylib.h`, `external/`). |

## Notes

- `thirdparty/raylib` is a committed copy (not a submodule) because the template
  applies small local adjustments to it. Its contents are the frozen 6.0.0 source.
- `thirdparty/raylib-ios` is used **only** for the iOS target (Xcode/xcframework
  build). Desktop/Android/Web keep using `thirdparty/raylib`.
- CI actions are pinned by full commit SHA in `.github/workflows/` for the same
  reproducibility reason.
