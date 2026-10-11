# This is a MODIFIED copy of raylib 6.0.0

Upstream: <https://github.com/raysan5/raylib>, tag `6.0.0`. Licence: zlib/libpng,
reproduced unaltered in `LICENSE`.

The zlib licence's second clause requires altered source versions to be plainly
marked as such, and not misrepresented as the original. This file is that mark.
Every change is in the build system, in platform selection, a backport of
a fix upstream has already merged, one of two lines that make a backend obey
`SetExitKey()`, the one that asks Android's EGL for the context raylib was
compiled for, or a fix of our own that upstream does not have yet; each one is
commented `PATCHED (raylib_multiplatform)` at its site. Under `src/external/` --
the libraries raylib bundles -- there are two changes, both to its software
renderer: such a backport, and that fix of our own. None of them changes a
signature, and what a game can see is only that memory is freed, a
software-rendered scissor clips where it says, and a software-rendered
texture's far edge is its own last texel.

| File | Line | Change | Why |
|---|---|---|---|
| `CMakeOptions.txt` | 9 | `enum_option(PLATFORM ...)` gains `WebEmscripten` and `Win32` | it `FATAL_ERROR`s on anything not listed, so a branch in `LibraryConfigurations.cmake` alone cannot be selected |
| `cmake/LibraryConfigurations.cmake` | 78 | a `PLATFORM=WebEmscripten` branch setting `PLATFORM_WEB_EMSCRIPTEN` | raylib 6.0 ships the GLFW-free web backend (`platforms/rcore_web_emscripten.c`) and `rcore.c` selects it, but CMake had no way to ask for it; drives `[web] backend` in `raylib_multiplatform.toml` |
| `cmake/LibraryConfigurations.cmake` | 197 | the RGFW branch also links Xrandr, Xcursor and Xi | RGFW's X11 backend calls them, so `PLATFORM=RGFW` did not link on Linux |
| `cmake/LibraryConfigurations.cmake` | 216 | a `PLATFORM=Win32` branch setting `PLATFORM_DESKTOP_WIN32` | same situation as WebEmscripten: the backend ships, nothing selected it; drives `[windows] backend` |
| `src/rcore.c` | 546, 630 | `#elif defined(PLATFORM_WEB_EMSCRIPTEN)` in the platform include chain, and a matching `TRACELOG` line | the header of `rcore.c` lists the platform as supported; without the branch the build fell through to `#else` and failed at link |
| `src/CMakeLists.txt` | 72 | `-sUSE_GLFW=3` narrowed from `PLATFORM MATCHES Web` to `PLATFORM STREQUAL "Web"` | it was `PUBLIC` and leaked into the GLFW-free web backends, which then linked the GLFW-in-JavaScript shim anyway |
| `src/platforms/rcore_android.c` | 949 | the EGL context asks for client version 3 when raylib was compiled for ES 3.0, 2 otherwise | it asked for 2 whatever it was compiled for, and EGL may answer that with an ES 2.0 context; `[android] gl_version = "ES30"` compiles raylib for ES 3.0 and calls ES 3.0 functions in that context |
| `src/platforms/rcore_desktop_win32.c` | 2037 | the Win32 backend closes on `CORE.Input.Keyboard.exitKey` instead of on `KEY_ESCAPE` by name | it was the one desktop backend that ignored `SetExitKey()`; the framework sets `KEY_NULL` so that Escape is the game's key ("back", "pause", `ui_cancel`), and `[windows] backend = "win32"` closed the game on it anyway |
| `src/platforms/rcore_drm.c` | 1818 | the terminal keyboard writes a lone ESC byte as `KEY_ESCAPE`, not into the exit key's slot | with `SetExitKey(KEY_NULL)` that slot is 0, and the exit check one function over read slot 0 as pressed: Escape on a DRM game's terminal still closed it, and the game never saw `KEY_ESCAPE` at all |
| `src/raudio.c` | 1044, 1769 | three frees corrected in `UnloadSoundAlias` and `UnloadMusicStream`: backports of upstream `dcb0ca5d14` (#5857), `86c7edfd74` (#5963) and `8d31d0c3cc` (#5916) | 6.0.0 leaked each sound alias's converter cache and each WAV music stream's decoder, and double-freed a FLAC one. LeakSanitizer found the first two the first time `rmp::audio` ran on a real device. Drop at the next bump: upstream master has all three |
| `src/external/rlsw.h` | 2415 | `sw_texture_sample_nearest` clamps the texel it reads to the texture: u or v of exactly 1 is the edge texel, not one past it. Our own fix: upstream master samples the same way | `sw_saturate()` gives 1.0 at the far edge, and 1.0 times the width is texel `width`: on any row but the last that is the next row's first texel, the wrong colour, and on the last it is a read past the end of the texture. AddressSanitizer found it in `tests/e2e/games/07_platformer.e2e`, on a bat's sprite sheet drawn at the edge of its frame; `tests/rlsw_test.cpp` calls the lookup at the edges |
| `src/external/rlsw.h` | 4220 | `swScissor` reads the scissor's y from the bottom: a backport of upstream `7a247ff40f` (#5976) | raylib passes `rlScissor` a bottom-origin y, as `glScissor` takes it, and the software renderer read it from the top, so every scissored draw under `PLATFORM=Memory` (the CI screenshots, the examples' posters) was clipped to the mirrored band. Drop at the next bump: upstream has it |

Re-apply the first six and the three backend lines when bumping raylib, and
the `rlsw.h` texel clamp unless upstream has fixed it by then
(`tests/rlsw_test.cpp` says which); the `raudio.c` and `rlsw.h` backports are
already upstream. `thirdparty/FROZEN_VERSIONS.md` carries the
same list with the full reasoning, and `tools/license_db.py --check` fails if
this file goes missing while the component is recorded as modified.
