# This is a MODIFIED copy of raylib 6.0.0

Upstream: <https://github.com/raysan5/raylib>, tag `6.0.0`. Licence: zlib/libpng,
reproduced unaltered in `LICENSE`.

The zlib licence's second clause requires altered source versions to be plainly
marked as such, and not misrepresented as the original. This file is that mark.
Nothing below changes a public raylib function: every change is in the build
system or in platform selection, each one is commented `PATCHED
(raylib_multiplatform)` at its site, and nothing under `src/external/` -- the
libraries raylib bundles -- is touched at all.

| File | Line | Change | Why |
|---|---|---|---|
| `CMakeOptions.txt` | 9 | `enum_option(PLATFORM ...)` gains `WebEmscripten` and `Win32` | it `FATAL_ERROR`s on anything not listed, so a branch in `LibraryConfigurations.cmake` alone cannot be selected |
| `cmake/LibraryConfigurations.cmake` | 78 | a `PLATFORM=WebEmscripten` branch setting `PLATFORM_WEB_EMSCRIPTEN` | raylib 6.0 ships the GLFW-free web backend (`platforms/rcore_web_emscripten.c`) and `rcore.c` selects it, but CMake had no way to ask for it; drives `[web] backend` in `raylib_multiplatform.toml` |
| `cmake/LibraryConfigurations.cmake` | 197 | the RGFW branch also links Xrandr, Xcursor and Xi | RGFW's X11 backend calls them, so `PLATFORM=RGFW` did not link on Linux |
| `cmake/LibraryConfigurations.cmake` | 216 | a `PLATFORM=Win32` branch setting `PLATFORM_DESKTOP_WIN32` | same situation as WebEmscripten: the backend ships, nothing selected it; drives `[windows] backend` |
| `src/rcore.c` | 546, 630 | `#elif defined(PLATFORM_WEB_EMSCRIPTEN)` in the platform include chain, and a matching `TRACELOG` line | the header of `rcore.c` lists the platform as supported; without the branch the build fell through to `#else` and failed at link |
| `src/CMakeLists.txt` | 72 | `-sUSE_GLFW=3` narrowed from `PLATFORM MATCHES Web` to `PLATFORM STREQUAL "Web"` | it was `PUBLIC` and leaked into the GLFW-free web backends, which then linked the GLFW-in-JavaScript shim anyway |

Re-apply all six when bumping raylib. `thirdparty/FROZEN_VERSIONS.md` carries the
same list with the full reasoning, and `tools/license_check.sh` fails if this
file goes missing while the component is recorded as modified.
