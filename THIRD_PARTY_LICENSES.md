# Third-party licenses

This template vendors several libraries. Each keeps its own license; the files
below are summaries — the authoritative texts are the `LICENSE` files in each
directory (linked where present).

| Component | Location | License |
|---|---|---|
| raylib | `thirdparty/raylib/` | zlib/libpng — see `thirdparty/raylib/LICENSE` |
| raymob | `raymob/`, `thirdparty/raymob/` | MIT — see `raymob/LICENSE`, `thirdparty/raymob/LICENSE` |
| raylib-iOS (fork of raylib) | `thirdparty/raylib-ios/` (submodule) | zlib/libpng (raylib's) — see `thirdparty/raylib-ios/LICENSE` |
| ANGLE (prebuilt, bundled by the raylib-iOS fork) | `thirdparty/raylib-ios/deps/ANGLE/` | BSD-3-Clause (Google) — see note below |
| Clay (UI layout engine behind `rmp::ui`) | `thirdparty/clay/` | zlib/libpng — see `thirdparty/clay/LICENSE.md` |
| rres | `thirdparty/rres/` | MIT — see `thirdparty/rres/LICENSE` |
| tiny-AES-c | `thirdparty/rres/external/aes.{h,c}` | Public domain / Unlicense |
| Monocypher | `thirdparty/rres/external/monocypher.{h,c}` | BSD-2-Clause OR CC0 (dual-licensed) |
| LZ4 | `thirdparty/rres/external/lz4.{h,c}` | BSD-2-Clause |
| QOI | `thirdparty/rres/external/qoi.h` | MIT |
| rres_pack tool + MD5 | `tools/` | MIT (this template's license) |

## Notes

- **raylib-iOS** is a fork of raylib and is distributed under raylib's
  zlib/libpng license; the fork inherits that license. It is only used for the
  iOS target. Credit: https://github.com/ghera/raylib-iOS (based on raylib by
  Ramon Santamaria, iOS rcore from PR raysan5/raylib#3880 by blueloveTH).
- **ANGLE** (OpenGL ES → Metal) is copyright The ANGLE Project Authors and
  licensed under BSD-3-Clause. The raylib-iOS fork ships prebuilt
  `libEGL.xcframework` / `libGLESv2.xcframework` under
  `thirdparty/raylib-ios/deps/ANGLE/` but does not include the ANGLE LICENSE
  file, so the full BSD text is reproduced at `ios/ANGLE-LICENSE.txt` for
  attribution (upstream: https://github.com/google/angle/blob/main/LICENSE).
- The **template's own code** (`src/`, `include/`, `tools/`, `cmake/`, `ios/`,
  build files) is under the root `LICENSE` (MIT).
