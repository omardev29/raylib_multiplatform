// ---------------------------------------------------------------------------
// raylib's software renderer, the one every headless picture is drawn with:
// the CI screenshots, the render check, and every e2e golden.
//
// Its nearest-texel lookup clamps u and v to 0..1 and multiplies by the size,
// so the far edge, u or v of exactly 1, was texel `width`: one past the row.
// On any row but the last that is the next row's first texel, the wrong
// colour; on the last it is past the end of the picture.
// tests/e2e/games/07_platformer.e2e found it, as AddressSanitizer reading past
// a bat's sprite sheet drawn at the edge of its frame: nothing had drawn a bat
// under ASan before -- the platformer's bot plays its levels without drawing
// them. The fix is in thirdparty/raylib/src/external/rlsw.h (PATCHES.md); the
// lookup is called through tests/rlsw_sample.c, because rlsw.h is C.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include <array>

extern "C" int rmp_test_rlsw_nearest(const unsigned char *rgba, int width, int height,
                                     float u, float v, float out[4]);

namespace {

// 2 x 2, RGBA: red, green on the top row; blue, white below.
constexpr auto TEXELS = std::to_array<unsigned char>(
    { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 });

struct Rgb {
    float r, g, b;
};

Rgb sample(float u, float v) {
    std::array<float, 4> out{};
    REQUIRE(rmp_test_rlsw_nearest(TEXELS.data(), 2, 2, u, v, out.data()) == 1);
    return { out[0], out[1], out[2] };
}

bool is(Rgb got, Rgb want) {
    return got.r == want.r && got.g == want.g && got.b == want.b;
}

constexpr Rgb RED_TEXEL{ 1, 0, 0 };
constexpr Rgb GREEN_TEXEL{ 0, 1, 0 };
constexpr Rgb BLUE_TEXEL{ 0, 0, 1 };
constexpr Rgb WHITE_TEXEL{ 1, 1, 1 };

} // namespace

TEST_SUITE("rlsw: the software renderer") {
    TEST_CASE("nearest sampling inside the texture finds the texel under it") {
        CHECK(is(sample(0.25f, 0.25f), RED_TEXEL));
        CHECK(is(sample(0.75f, 0.25f), GREEN_TEXEL));
        CHECK(is(sample(0.25f, 0.75f), BLUE_TEXEL));
        CHECK(is(sample(0.75f, 0.75f), WHITE_TEXEL));
    }

    TEST_CASE("the far edge, u or v of exactly 1, is the edge texel") {
        // The top row's right edge: it was the bottom row's first texel.
        CHECK(is(sample(1.0f, 0.25f), GREEN_TEXEL));
        // The bottom row: it was past the end of the texture.
        CHECK(is(sample(0.25f, 1.0f), BLUE_TEXEL));
        CHECK(is(sample(1.0f, 1.0f), WHITE_TEXEL));
    }

    TEST_CASE("past either edge clamps, as the texture is set to") {
        CHECK(is(sample(-1.0f, -1.0f), RED_TEXEL));
        CHECK(is(sample(2.0f, 0.25f), GREEN_TEXEL));
        CHECK(is(sample(2.0f, 2.0f), WHITE_TEXEL));
    }
}
