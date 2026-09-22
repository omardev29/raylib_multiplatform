#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. The half of the catalogue that needs no GPU.
//
// The public surface is include/rmp/behavior.h. Everything here exists so that
// tests/behaviors_test.cpp can check arithmetic whose only other home is inside
// a _draw -- and a _draw cannot run in a unit test, because raylib's drawing
// reaches into a render batch that InitWindow() creates and calling it without
// one is a segfault rather than a failed assertion. Splitting the pure half out
// is the same seam rmp::ui and the animation clock already have.
// ---------------------------------------------------------------------------

namespace rmp::behavior::detail {

// Where a Parallax layer's copies go. The twenty lines everybody writes wrong
// the first time: one copy leaves a gap the moment the offset passes the
// texture's width, and a second copy placed a pixel out is a seam that crawls
// sideways across the screen forever.
struct ParallaxTiling {
    float offset = 0; // the FIRST copy's left edge, always in (-width, 0]
    int copies = 0; // how many to draw to cover `screen`
};

// `shift` is the object's position times its factor, plus its own drift.
// A width of zero or less gives no copies rather than a division by it.
ParallaxTiling parallax_tiling(float shift, float width, float screen);

} // namespace rmp::behavior::detail
