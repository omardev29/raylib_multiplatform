# This is a MODIFIED copy of raylib 6.2

Upstream: <https://github.com/raysan5/raylib>, tag `6.2` (`02566453`), without
`.github/`, `examples/`, `projects/`, `tools/`, `logo/`, `build.zig` and
`build.zig.zon`. Licence: zlib/libpng, reproduced unaltered in `LICENSE`.

The zlib licence's second clause requires altered source versions to be plainly
marked as such, and not misrepresented as the original. This file is that mark.
The copy is raylib_cmake_template's (6.2 and its patches, commented
`PATCHED (raylib_cmake_template)`), with this framework's own on top, commented
`PATCHED (raylib_multiplatform)`. Every change is at its site; none changes a
signature. Under `src/external/` -- the libraries raylib bundles -- three files
are changed: `rlsw.h`, `tinyobj_loader_c.h` and `RGFW/deps/minigamepad.h`.

| File | Line | From | Change | Why |
|---|---|---|---|---|
| `CMakeOptions.txt` | 9 | raylib_multiplatform | `enum_option(PLATFORM ...)` gains `WebEmscripten` | it `FATAL_ERROR`s on anything not listed, so a branch in `LibraryConfigurations.cmake` alone cannot be selected |
| `cmake/LibraryConfigurations.cmake` | 117 | raylib_multiplatform | a `PLATFORM=WebEmscripten` branch setting `PLATFORM_WEB_EMSCRIPTEN` | raylib ships the GLFW-free web backend (`platforms/rcore_web_emscripten.c`) and nothing selected it; drives `[web] backend` in `raylib_multiplatform.toml` |
| `cmake/LibraryConfigurations.cmake` | 236 | raylib_cmake_template | the RGFW branch also links Xrandr | RGFW's X11 backend calls it for the monitors, so `PLATFORM=RGFW` did not link on Linux |
| `src/rcore.c` | 526, 608 | raylib_multiplatform | `#elif defined(PLATFORM_WEB_EMSCRIPTEN)` in the platform include chain, and its `TRACELOG` line | the header of `rcore.c` lists the platform as supported; without the branch the build fell through to `#else` and failed at link |
| `src/rcore.c` | 3816 | raylib_cmake_template | a replayed automation event with values out of range is dropped | what raylib 6.2's security audit left open |
| `src/CMakeLists.txt` | 80 | raylib_multiplatform | `-sUSE_GLFW=3` only for `PLATFORM STREQUAL "Web"` | it was `PUBLIC` for everything matching `Web` and leaked into the GLFW-free web backends, which then linked the GLFW-in-JavaScript shim anyway |
| `src/platforms/rcore_android.c` | 954 | raylib_multiplatform | the EGL context asks for client version 3 when raylib was compiled for ES 3.0, 2 otherwise | it asked for 2 whatever it was compiled for, and EGL may answer that with an ES 2.0 context; `[android] gl_version = "ES30"` calls ES 3.0 functions in it |
| `src/platforms/rcore_drm.c` | 1176, 1205 | raylib_multiplatform | the card probe asks a card for its DRM resources only while no card before it has answered, and asks card2, the last it tries, too | each line called `drmModeGetResources` again, so the card that answered leaked its `drmModeRes` (LeakSanitizer, 92 bytes, on vkms); and card2 was opened and never asked. `DrmCardProbeTest` in `tests/configure_test.py` reads the probe |
| `src/platforms/rcore_drm.c` | 1785 | raylib_cmake_template | the terminal keyboard turns flow control (`IXON`) off | Ctrl+S, pressed as a game key, stopped the console's output and the game at its next `TraceLog()` |
| `src/platforms/rcore_drm.c` | 1856 | raylib_multiplatform | a lone ESC byte on the terminal is `KEY_ESCAPE`, not the exit key's slot | with `SetExitKey(KEY_NULL)` that slot is 0, the exit check read it as pressed, and Escape closed a DRM game that never saw `KEY_ESCAPE` |
| `src/platforms/rcore_web.c` | 1001 | raylib_cmake_template | `OpenURL()` refuses quotes and anything but `http(s)://`, as the desktop does | the gap raylib 6.2's audit left in this backend |
| `src/raudio.c` | 494 | raylib_multiplatform | `InitAudioDevice()` tries miniaudio's null backend when the default one has no device | ALSA's library loads on a machine with no sound card, wins the context and then opens no device, and miniaudio reaches its null backend only when every other context fails: so a machine without libasound got a silent device and one with it got none. The build image carries libasound for Wine; `tests/audio_device_test.cpp` requires the device there |
| `src/rtextures.c` | 515, 548 | raylib_cmake_template | raylib's `b9758a64` and `226b281e`, from after 6.2: a QOI file under 13 bytes is not read past its end, and a PEP image does not come back freed | raylib's own fixes, brought back (PEP is off: `SUPPORT_FILEFORMAT_PEP` is 0) |
| `src/external/rlsw.h` | 2388 | raylib_multiplatform | `sw_texture_sample_nearest` clamps the texel it reads to the texture | `sw_saturate()` gives 1.0 at the far edge, and 1.0 times the width is texel `width`: the next row's first texel, or a read past the end of the texture. `tests/rlsw_test.cpp` calls the lookup at the edges |
| `src/external/tinyobj_loader_c.h` | 131, 1027, 1101 | raylib_cmake_template | the face code of raylib's `430439e6`, from after 6.2 | an OBJ n-gon of up to 23 vertices loads, triangulated, where 6.2 overflowed the stack on more than 16; a line over 4095 bytes is dropped cleanly |
| `src/external/RGFW/deps/minigamepad.h` | 613 | raylib_cmake_template | defines `ABS_PROFILE` where the kernel headers do not | RGFW did not build on Ubuntu 22.04's headers |

What 6.2 brought that this copy no longer patches: the Win32 backend in
`CMakeOptions.txt` and `LibraryConfigurations.cmake`, and its `SetExitKey()`;
the three frees in `raudio.c`; and the software renderer's scissor. And what
raylib_cmake_template patches that this copy does not take: its console guard
in `rcore_drm.c`, a process forked to keep the console's keys from the shell
after a crash -- `src/rmp/console.cpp` puts the terminal back here, from
inside the game, and a fork inside raylib would run beside it.

When bumping raylib, re-apply every row whose fix upstream has not made by
then: `tests/rlsw_test.cpp` and `DrmCardProbeTest` say for theirs, and each
comment at its site says what to look for. `thirdparty/FROZEN_VERSIONS.md`
carries the same list, and `tools/license_db.py --check` fails if this file goes
missing while the component is recorded as modified, or if a bundled file
marked modified is not named here.
