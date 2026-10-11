/* ---------------------------------------------------------------------------
 * raylib's software renderer (thirdparty/raylib/src/external/rlsw.h), reached
 * below its API: one texel lookup, for tests/rlsw_test.cpp.
 *
 * rlsw.h is C and does not compile as C++, so the call is made here. It is
 * compiled into this file a second time with every function static (SWAPI), so
 * nothing clashes with the copy inside raylib when raylib is the PLATFORM=Memory
 * build, and its static helpers -- sw_texture_sample_nearest among them -- can
 * be called by name. Built quietly, like raylib itself (CMakeLists.txt).
 *
 * Two of its functions are defined without SWAPI, so SWAPI cannot make them
 * static, and they are renamed here instead: the coverage build links this
 * file with raylib's PLATFORM=Memory archive, which defines both too, and
 * failed with "multiple definition of `swGetIntegerv'" -- the one build of
 * unit_test whose raylib carries the software renderer.
 * ------------------------------------------------------------------------- */

/* rlsw's own knobs, under their own names. */
#define SWAPI static /* NOLINT(readability-identifier-naming): rlsw.h reads SWAPI */
/* NOLINTNEXTLINE(readability-identifier-naming): rlsw.h reads RLSW_IMPLEMENTATION */
#define RLSW_IMPLEMENTATION
/* NOLINTNEXTLINE(readability-identifier-naming): renames rlsw.h's swGetIntegerv */
#define swGetIntegerv rmp_test_sw_get_integerv
/* NOLINTNEXTLINE(readability-identifier-naming): renames rlsw.h's swTexStorage2D */
#define swTexStorage2D rmp_test_sw_tex_storage_2d
#include "external/rlsw.h"

int rmp_test_rlsw_nearest(const unsigned char *rgba, int width, int height, float u,
                          float v, float out[4]);

/* The colour the nearest-texel lookup gives at (u, v) on a width x height RGBA8
 * texture that clamps at its edges, 0..1 a channel; 0 when it could not be made.
 * The renderer is started for it, as raylib starts it -- swInit() is what fills
 * the table a byte becomes a channel through -- once, because it says so on
 * stdout, and closed at exit. */
int rmp_test_rlsw_nearest(const unsigned char *rgba, int width, int height, float u,
                          float v, float out[4]) {
    sw_texture_t texture = {
        .minFilter = SW_NEAREST,
        .magFilter = SW_NEAREST,
        .sWrap = SW_CLAMP,
        .tWrap = SW_CLAMP,
    };
    static int started = 0;
    if (!started) {
        if (!swInit(1, 1)) return 0;
        started = 1;
        atexit(swClose);
    }
    const int made =
        sw_texture_alloc(&texture, rgba, width, height, SW_PIXELFORMAT_COLOR_R8G8B8A8);
    if (made) sw_texture_sample_nearest(out, &texture, u, v);
    sw_texture_free(&texture);
    return made;
}
