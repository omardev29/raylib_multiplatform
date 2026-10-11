#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/ui/backdrop.cpp and its tests: what overlay()'s blur is
// made of, for a test to look at. The hooks the app and overlay() call are
// declared with the rest, in ../ui_internal.h and internal.h.
//
// The pure half -- which GLSL a context takes, the shader's source, how a blur
// of so many pixels is cut into passes -- runs with no window, and
// tests/ui_overlay_test.cpp holds it there. What it draws needs a GPU, and is
// tests/ui_render_test.cpp's.
// ---------------------------------------------------------------------------

#include <optional>
#include <string>

namespace rmp::ui::detail {

// Is this frame being drawn into the backdrop's texture -- because the last
// frame's overlay() asked for a blur -- rather than straight onto the screen?
// Never without a window.
bool backdrop_redirected();

// Did an overlay() put a blurred backdrop on screen this frame? False on the
// first frame an overlay asks for one (that frame was not redirected), and
// wherever the machine cannot blur.
bool backdrop_blurred();

// The dialect of GLSL the blur is written in. One shader, three prologues:
// OpenGL ES 2.0 and WebGL 1 take GLSL ES 1.00, ES 3.0 and WebGL 2 take 3.00,
// OpenGL 3.3 and 4.3 take 330. NONE where the blur has no shader to run:
// raylib's software renderer and OpenGL 1.1, which run none at all, and 2.1,
// whose GLSL 1.20 the blur is not written in (nothing raylib builds for us
// opens a 2.1 context).
enum class Glsl { NONE, ES_100, ES_300, CORE_330 };

// The dialect for what rlGetVersion() answers. Pure.
Glsl glsl_for(int gl_version);

// The blur's vertex and fragment shaders in that dialect -- both, because a
// program is linked from two shaders of ONE version: the ES ones next to
// raylib's own vertex shader do not link under a desktop context ("all
// shaders must use same shading language version", Mesa). Empty for NONE.
struct ShaderSource {
    std::string vertex;
    std::string fragment;
};
ShaderSource blur_shader_source(Glsl glsl);

// For tests/ui_render_test.cpp: build the blur from `source` instead of the
// dialect rlGetVersion() names -- an ES dialect under the desktop's GL, or a
// shader that does not build. What was built, and a machine given up on, are
// forgotten; nullopt goes back to rlGetVersion()'s.
void set_test_blur_shader(std::optional<ShaderSource> source);

// How a Gaussian blur of `sigma_px` pixels -- the standard deviation, as CSS's
// blur() means it -- is drawn over a frame whose longer side is `frame_size`
// pixels: at 1/divisor of its resolution, with BLUR_TAPS taps either side of
// each pixel, one texel of that small picture apart, weighted by a Gaussian
// of `sigma` of its texels. The divisor is a power of two: 2 below 3 pixels,
// 4 from there, and past what the taps reach at three sigmas, the next power
// that they do -- never the taps spread apart, which skip texels and draw
// stripes -- until the small picture is one texel across, and never past
// BLUR_MAX_DIVISOR. divisor 0: too small to show, and copied as it is.
inline constexpr int BLUR_TAPS = 8;
inline constexpr int BLUR_MAX_DIVISOR = 4096;
struct BlurPlan {
    int divisor = 0;
    float sigma = 0;
};
BlurPlan plan_blur(float sigma_px, int frame_size);

} // namespace rmp::ui::detail
