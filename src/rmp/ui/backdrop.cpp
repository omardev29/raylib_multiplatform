// ===========================================================================
// The backdrop: what overlay() blurs.
//
// GLES2 cannot copy the framebuffer (rlBlitFramebuffer is GL 3.3 and ES 3
// only), so a frame that will be blurred is drawn into a texture from the
// start: begin_backdrop(), right after BeginDrawing(), when the last frame's
// overlay asked for a blur. The overlay then blurs what is in it and puts it
// on screen (blur_backdrop()); finish_backdrop() puts it there unblurred when
// no overlay took it. The hooks are declared in ../ui_internal.h and
// internal.h.
//
// So the first frame an overlay asks for a blur has its veil and no blur --
// nothing was redirected yet -- and the blur eases in from the next one, over
// the theme's transition (ease_towards()). Every frame after that it is drawn
// in three steps:
//
//   1. the frame, across: into a texture 1/2, 1/4 or less of its size
//      (plan_blur()), each pixel the Gaussian-weighted sum of BLUR_TAPS taps
//      either side of it -- read, below a half, from the frame halved down to
//      twice that size first, so that every pixel of it counts;
//   2. that, down: the same sum, vertically, into a second small texture;
//   3. that, on screen: stretched back to the screen's size, which bilinear
//      filtering smooths.
//
// One shader does both passes, written once and given the prologue the
// context takes (glsl_for()): GLSL ES 1.00 for OpenGL ES 2.0 and WebGL 1,
// ES 3.00 for ES 3.0 and WebGL 2, 330 for desktop OpenGL 3.3 and 4.3. It
// brings its own vertex shader in the same dialect, because a program is one
// version of GLSL: raylib's default vertex shader is the version the library
// was built for, and an ES fragment shader beside the desktop's does not
// link. With both of its own, all three dialects are built and drawn on the
// desktop's GL by tests/ui_render_test.cpp (Mesa takes ES shaders in a
// desktop context), and the one a phone or a browser builds is not seen for
// the first time on a phone or in a browser.
//
// Where it cannot run, the veil comes alone, and the reason is a machine's,
// not the game's: raylib's software renderer and OpenGL 1.1 have no shaders,
// 2.1 takes GLSL 1.20, which the blur is not written in, and a driver may
// refuse a shader or a framebuffer. Each is one TraceLog line, never
// RMP_REPORT_ONCE: [dev] strict would stop a correct game on a CI runner.
// What IS the game's is reported: a blur asked for in a frame of its own
// (RMP_ENTRY_POINT), which nothing redirects, and a scene that draws through
// BeginTextureMode() of its own while the frame is redirected --
// EndTextureMode() sends the rest of the frame to the screen, behind the
// backdrop's back.
//
// Nothing here runs without a window. The unit tests have none: the GPU half
// is marked out of their coverage, and the pure half -- glsl_for(),
// blur_shader_source(), plan_blur() -- is theirs.
// ===========================================================================

#include "backdrop.h"
#include "internal.h"
#include "../internal.h"
#include "../scene_internal.h" // running(): an app with scenes, or a frame of the game's own

#include <raylib.h>
#include <rlgl.h>

// An OpenGL ES 2.0 build: what bound_framebuffer() asks GL itself, below.
#if defined(GRAPHICS_API_OPENGL_ES2) && !defined(GRAPHICS_API_OPENGL_ES3) && \
    (defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(PLATFORM_DRM))
#include <GLES2/gl2.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rmp::ui::detail {

namespace {

// A blur under this many pixels does not show, and is drawn as a copy.
constexpr float SMALLEST_SIGMA = 0.5f;
// From here up the small textures are a quarter of the screen, not a half.
constexpr float QUARTER_FROM = 3.0f;

struct BackdropState {
    // Between the app's begin_backdrop() and finish_backdrop(): a frame of
    // RMP_GAME's, which is the only kind that can be redirected.
    bool in_app_frame = false;
    // An overlay asked for a blur this frame, so the next one is redirected.
    bool asked = false;
    // This frame is being drawn into gpu.scene.
    bool redirected = false;
    // An overlay put a blurred backdrop on screen this frame.
    bool blurred = false;
    // This machine cannot blur, and has said so: the veil comes alone until
    // the UI starts again.
    bool off = false;
    // How far the blur has eased in, 0..1, along the theme's transition.
    float eased = 0;
};
BackdropState backdrop;

// The halvings of the frame a blur at BLUR_MAX_DIVISOR is read from: the
// last is twice the small picture.
constexpr int MAX_HALVINGS = 11;
static_assert((2 << MAX_HALVINGS) == BLUR_MAX_DIVISOR);

// What the blur is drawn with: the frame, the frame halved as many times as
// a blur needs, the two small textures, the shader and its two uniforms.
// Made when first needed, released with the UI.
struct Gpu {
    rmp::RenderTexture scene;
    std::array<rmp::RenderTexture, MAX_HALVINGS> halves;
    rmp::RenderTexture across;
    rmp::RenderTexture down;
    rmp::Shader shader;
    int direction = -1;
    int sigma = -1;
    // tests/ui_render_test.cpp's: a dialect, or a shader that does not build.
    std::optional<ShaderSource> test_source;
};
Gpu gpu;

// --- the shader, in three dialects ----------------------------------------
//
// Adjacent literals rather than one raw string: no line of this file starts
// with a `#`, so nothing that reads C++ for its preprocessor lines takes a
// GLSL `#version` for one.

constexpr std::string_view VERTEX_100 = "#version 100\n"
                                        "attribute vec3 vertexPosition;\n"
                                        "attribute vec2 vertexTexCoord;\n"
                                        "attribute vec4 vertexColor;\n"
                                        "varying vec2 fragTexCoord;\n"
                                        "varying vec4 fragColor;\n";
// ES 3.00 and 330 differ in their first line only.
constexpr std::string_view VERTEX_IN_OUT = "in vec3 vertexPosition;\n"
                                           "in vec2 vertexTexCoord;\n"
                                           "in vec4 vertexColor;\n"
                                           "out vec2 fragTexCoord;\n"
                                           "out vec4 fragColor;\n";
// raylib's names: it binds the attributes and sets `mvp` by them.
constexpr std::string_view VERTEX_BODY =
    "uniform mat4 mvp;\n"
    "void main() {\n"
    "    fragTexCoord = vertexTexCoord;\n"
    "    fragColor = vertexColor;\n"
    "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
    "}\n";

// highp where the GPU has it: a texture coordinate of a 1920-pixel frame in
// mediump's ten bits of mantissa is half a texel out.
constexpr std::string_view FRAGMENT_100 =
    "#version 100\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 fragTexCoord;\n"
    "varying vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "vec4 sample_at(vec2 uv) { return texture2D(texture0, uv); }\n"
    "void write_out(vec4 c) { gl_FragColor = c; }\n";
constexpr std::string_view FRAGMENT_IN_OUT =
    "in vec2 fragTexCoord;\n"
    "in vec4 fragColor;\n"
    "uniform sampler2D texture0;\n"
    "out vec4 finalColor;\n"
    "vec4 sample_at(vec2 uv) { return texture(texture0, uv); }\n"
    "void write_out(vec4 c) { finalColor = c; }\n";
// One pass of the separable Gaussian: BLUR_TAPS taps either side, each
// `direction` apart in texture coordinates, weighted by a Gaussian of
// `sigma` taps. A loop with constant bounds, which is what GLSL ES 1.00
// allows. The tint is raylib's vertex colour: white.
constexpr std::string_view FRAGMENT_BODY =
    "uniform vec2 direction;\n"
    "uniform float sigma;\n"
    "const int TAPS = 8;\n"
    "void main() {\n"
    "    float s = max(sigma, 0.01);\n"
    "    vec4 sum = vec4(0.0);\n"
    "    float total = 0.0;\n"
    "    for (int i = -TAPS; i <= TAPS; ++i) {\n"
    "        float x = float(i);\n"
    "        float w = exp(-0.5 * x * x / (s * s));\n"
    "        sum += sample_at(fragTexCoord + direction * x) * w;\n"
    "        total += w;\n"
    "    }\n"
    "    write_out(sum / total * fragColor);\n"
    "}\n";

static_assert(BLUR_TAPS == 8, "FRAGMENT_BODY's TAPS is BLUR_TAPS");

std::string joined(std::string_view a, std::string_view b, std::string_view c) {
    std::string out;
    out.reserve(a.size() + b.size() + c.size());
    out.append(a).append(b).append(c);
    return out;
}

// Is there a screen to draw on? Not in a test that set a viewport, and not
// before InitWindow(): the viewport is the window's, and a window that is
// not there is 0 by 0 (context.cpp is the seam that asks raylib).
bool screen_ready() {
    if (test_mode()) return false;
    const Clay_Dimensions v = viewport();
    return v.width >= 1 && v.height >= 1;
}

// RMP_COVERAGE_OFF(on the GPU: the unit tests have no window; tests/ui_render_test.cpp draws every line)

// A machine that cannot blur. A condition of the MACHINE -- the software
// renderer every CI job draws with, a driver that refuses -- so a plain
// TraceLog, said once by the state that remembers it (`off`), and never
// RMP_REPORT_ONCE: strict would stop a correct game where it runs.
bool give_up(std::string_view why) {
    backdrop.off = true;
    TraceLog(LOG_INFO, "UI: overlay() cannot blur here (%.*s); the veil comes alone",
             static_cast<int>(why.size()), why.data());
    return false;
}

// What a context cannot blur with, in its own words.
std::string_view without_shaders(int gl_version) {
    switch (gl_version) {
        case RL_OPENGL_SOFTWARE:
            return "raylib's software renderer has no shaders";
        case RL_OPENGL_11:
            return "OpenGL 1.1 has no shaders";
        case RL_OPENGL_21:
            return "OpenGL 2.1 takes GLSL 1.20, which the blur is not written in";
        default:
            return "this OpenGL is not one raylib names";
    }
}

// The blur's share of what was asked, eased one frame further towards all of
// it or none, over the theme's transition and along the curve every
// transition of the interface takes. Its own state and not anim_value()'s:
// that table is every control's, and a slot the backdrop took every frame
// could land on a control's and snap its fade.
float ease_towards(bool on) {
    const float duration = current_theme().transition;
    if (duration <= 0.0f) {
        backdrop.eased = on ? 1.0f : 0.0f;
    } else {
        // As anim_value() takes it: a stall does not finish a transition.
        float delta = frame_time();
        if (delta > 0.1f) delta = 0.1f;
        if (delta < 0.0f) delta = 0.0f;
        const float step = delta / duration;
        backdrop.eased = on ? std::fmin(1.0f, backdrop.eased + step)
                            : std::fmax(0.0f, backdrop.eased - step);
    }
    const float t = backdrop.eased;
    return t * t * (3.0f - 2.0f * t);
}

// The framebuffer bound right now, or nullopt where nobody can say.
//
// rlgl's rlGetActiveFramebuffer() asks GL for GL_DRAW_FRAMEBUFFER_BINDING,
// which OpenGL ES 2.0 does not have, so under ES 2.0 it answers 0 whatever
// is bound. ES 2.0's own name for it is GL_FRAMEBUFFER_BINDING, and an ES
// 2.0 build -- WebGL 1, Android at [android] gl_version "ES20", DRM -- asks
// for it here: the framework's one direct GL call, through the
// <GLES2/gl2.h> rlgl itself was built against. Without it a scene drawing
// through its own render texture under a blur, the pixel-art pattern, sent
// the rest of its frame to the screen unseen, and the blur showed the
// clear colour alone. A desktop build for ES 2.0 loads GL through rlgl's
// glad, which nothing outside rlgl can call, and stays unable to tell.
//
// tests/ui_render_test.cpp proves what is done with the answer, on the
// desktop's GL, where the answer is rlgl's. The ES 2.0 call itself is only
// compiled and linked, by the web, Android and DRM builds of the full
// matrix: no job draws a blur on an ES 2.0 context.
std::optional<unsigned int> bound_framebuffer() {
    if (rlGetVersion() != RL_OPENGL_ES_20) return rlGetActiveFramebuffer();
#if defined(GRAPHICS_API_OPENGL_ES2) && !defined(GRAPHICS_API_OPENGL_ES3) && \
    (defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(PLATFORM_DRM))
    GLint bound = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    return static_cast<unsigned int>(bound);
#else
    return std::nullopt;
#endif
}

// A scene that drew through a BeginTextureMode() of its own has ended it,
// and raylib went back to the screen: the rest of the frame is there, not in
// gpu.scene. Only where the bound framebuffer can be known.
bool escaped() {
    const std::optional<unsigned int> bound = bound_framebuffer();
    return bound.has_value() && *bound != gpu.scene.raw().id;
}

void report_escape() {
    RMP_REPORT_ONCE(
        "UI: a scene drew through its own BeginTextureMode()/EndTextureMode() "
        "under a blurred overlay(), which sends the rest of the frame to the "
        "screen: the blur is off from here on, and the veil stays. Draw that "
        "texture before the frame, or ask overlay() for no blur");
    backdrop.off = true;
}

// A small texture of the blur, the size asked for, made again when it is not.
bool fit(rmp::RenderTexture &target, int width, int height) {
    const RenderTexture2D &raw = target.raw();
    if (target.valid() && raw.texture.width == width && raw.texture.height == height) {
        return true;
    }
    target = rmp::assets::detail::make_render_texture(width, height);
    if (!target.valid()) return false;
    // Bilinear: each tap between two texels reads both, and the last step
    // stretches the small picture back up smoothly. Clamped: the edge of the
    // screen is not the other edge's neighbour.
    SetTextureFilter(target.raw().texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(target.raw().texture, TEXTURE_WRAP_CLAMP);
    return true;
}

// The shader, and the frame's texture at the screen's size. False, said once,
// where the machine cannot have them.
bool ready() {
    if (!gpu.shader.valid()) {
        const int version = rlGetVersion();
        const Glsl glsl = glsl_for(version);
        if (!gpu.test_source && glsl == Glsl::NONE)
            return give_up(without_shaders(version));
        const ShaderSource source =
            gpu.test_source ? *gpu.test_source : blur_shader_source(glsl);
        gpu.shader = rmp::assets::detail::make_shader(source.vertex, source.fragment);
        if (!gpu.shader.valid())
            return give_up("its shader did not build: raylib said why");
        gpu.direction = GetShaderLocation(gpu.shader, "direction");
        gpu.sigma = GetShaderLocation(gpu.shader, "sigma");
    }
    const Clay_Dimensions v = viewport();
    const int width = static_cast<int>(std::lround(v.width));
    const int height = static_cast<int>(std::lround(v.height));
    if (!fit(gpu.scene, width, height))
        return give_up("no render texture the frame fits in");
    return true;
}

// Draw `part` of `from` onto what is bound, over `width` x `height`,
// replacing what is there instead of blending with it: a copy of the frame is
// the frame, its alpha included. `part` is in texels from the top-left corner
// of the picture as it was drawn, and may reach past its right and bottom
// edges, where the edge texels go on (every texture here is clamped). A
// render texture is stored upside down, so the source rectangle raylib is
// handed is measured from the bottom, with a negative height.
//
// Blending off for this one quad, and not BLEND_CUSTOM with ONE and ZERO:
// rlgl keeps one set of custom factors, and it is the game's -- set once,
// it is what every BLEND_CUSTOM of the game draws with. rlgl batches, so
// what is queued is drawn first, with blending, and the quad alone without.
void copy(const rmp::RenderTexture &from, const Rectangle part, float width,
          float height) {
    const Texture2D &t = from.raw().texture;
    const Rectangle source{ part.x, static_cast<float>(t.height) - part.y - part.height,
                            part.width, -part.height };
    rlDrawRenderBatchActive();
    rlDisableColorBlend();
    DrawTexturePro(t, source, Rectangle{ 0, 0, width, height }, Vector2{ 0, 0 }, 0,
                   WHITE);
    rlDrawRenderBatchActive();
    rlEnableColorBlend();
}

// All of `from`, over `width` x `height`.
void copy(const rmp::RenderTexture &from, float width, float height) {
    const Texture2D &t = from.raw().texture;
    copy(from,
         Rectangle{ 0, 0, static_cast<float>(t.width), static_cast<float>(t.height) },
         width, height);
}

// The frame drawn into gpu.scene, on screen as it was drawn: what no overlay
// took, or what was drawn before the UI shut down.
void back_on_screen() {
    EndTextureMode();
    const Texture2D &frame = gpu.scene.raw().texture;
    copy(gpu.scene, static_cast<float>(frame.width), static_cast<float>(frame.height));
}

// One pass of the blur, from `part` of `from` into all of `into`:
// `direction` in `from`'s texture coordinates per tap.
void blur_pass(const rmp::RenderTexture &from, const Rectangle part,
               const rmp::RenderTexture &into, Vector2 direction, float sigma) {
    const Texture2D &target = into.raw().texture;
    BeginTextureMode(into);
    BeginShaderMode(gpu.shader);
    SetShaderValue(gpu.shader, gpu.direction, &direction, SHADER_UNIFORM_VEC2);
    SetShaderValue(gpu.shader, gpu.sigma, &sigma, SHADER_UNIFORM_FLOAT);
    copy(from, part, static_cast<float>(target.width), static_cast<float>(target.height));
    EndShaderMode();
    EndTextureMode();
}

// The frame, or its halving number `halvings` (1 is half of it).
const rmp::RenderTexture &halved(int halvings) {
    return halvings == 0 ? gpu.scene : gpu.halves.at(static_cast<size_t>(halvings - 1));
}

// The frame in gpu.scene, on screen, blurred by `sigma_px` pixels.
//
// Each step reads a picture exactly twice the size of the one it draws, at
// the texel centres of the one it draws: there bilinear filtering averages
// the two by two texels around each, so every pixel of the frame counts, and
// counts once. Read from further than twice -- the frame straight into a
// quarter of it -- a line one pixel wide vanished, or came out twice as
// bright, by which pixel it fell on; read from not quite twice -- 45 rows
// into 23 -- the texel centres drifted off the boundaries, and the lines came
// out half as bright again. So the frame is halved down to twice the small
// picture first, and is taken to be as large as the small picture times the
// divisor -- its last row and column going on into what it lacks, as the
// blur's clamped edges do anyway -- and cut back to its own size on the way
// back to the screen.
void draw_blurred(float sigma_px) {
    const Texture2D &frame = gpu.scene.raw().texture;
    const auto width = static_cast<float>(frame.width);
    const auto height = static_cast<float>(frame.height);
    const BlurPlan plan = plan_blur(sigma_px, std::max(frame.width, frame.height));
    if (plan.divisor == 0) {
        copy(gpu.scene, width, height);
        return;
    }
    const int small_width = (frame.width + plan.divisor - 1) / plan.divisor;
    const int small_height = (frame.height + plan.divisor - 1) / plan.divisor;
    const auto give_up_and_copy = [&] {
        give_up("no render texture for the blur");
        copy(gpu.scene, width, height);
    };
    // The frame as large as the small picture times the divisor, and its
    // halvings: each of the size of the part the next one reads.
    int part_width = small_width * plan.divisor;
    int part_height = small_height * plan.divisor;
    int halvings = 0;
    for (; part_width > 2 * small_width; ++halvings) {
        const Rectangle part{ 0, 0, static_cast<float>(part_width),
                              static_cast<float>(part_height) };
        part_width /= 2;
        part_height /= 2;
        rmp::RenderTexture &into = gpu.halves.at(static_cast<size_t>(halvings));
        if (!fit(into, part_width, part_height)) {
            give_up_and_copy();
            return;
        }
        BeginTextureMode(into);
        copy(halved(halvings), part, static_cast<float>(part_width),
             static_cast<float>(part_height));
        EndTextureMode();
    }
    if (!fit(gpu.across, small_width, small_height) ||
        !fit(gpu.down, small_width, small_height)) {
        give_up_and_copy();
        return;
    }
    // A tap is a texel of the small picture: two of its source's, and one of
    // its own.
    const rmp::RenderTexture &source = halved(halvings);
    blur_pass(source,
              Rectangle{ 0, 0, static_cast<float>(part_width),
                         static_cast<float>(part_height) },
              gpu.across,
              Vector2{ 2.0f / static_cast<float>(source.raw().texture.width), 0 },
              plan.sigma);
    const auto small_w = static_cast<float>(small_width);
    const auto small_h = static_cast<float>(small_height);
    blur_pass(gpu.across, Rectangle{ 0, 0, small_w, small_h }, gpu.down,
              Vector2{ 0, 1.0f / small_h }, plan.sigma);
    // Back up to the screen, the frame's own part of it.
    const auto divisor = static_cast<float>(plan.divisor);
    copy(gpu.down, Rectangle{ 0, 0, width / divisor, height / divisor }, width, height);
}

// RMP_COVERAGE_ON

} // namespace

// --- what the app and overlay() call ----------------------------------------

void begin_backdrop() {
    backdrop.in_app_frame = true;
    backdrop.blurred = false;
    const bool wanted = backdrop.asked;
    backdrop.asked = false;
    if (!wanted || backdrop.off || !screen_ready()) return;
    // RMP_COVERAGE_OFF(on the GPU: the unit tests have no window; tests/ui_render_test.cpp draws every line)
    if (!ready()) return;
    BeginTextureMode(gpu.scene);
    backdrop.redirected = true;
}
// RMP_COVERAGE_ON

void blur_backdrop(const float blur) {
    // 0 is no blur, and the next frame is not redirected. Anything that is
    // not a distance overlay.cpp has already reported.
    if (!(blur > 0)) return;
    if (!backdrop.in_app_frame) {
        // With the scene stack running this is not a scene's _draw() -- an
        // overlay in _update(), which begin() itself reports for what it is.
        // Without it, the frame is the game's own.
        if (!rmp::scenes::detail::running()) {
            RMP_REPORT_ONCE(
                "UI: overlay() blurs what the app's frame drew under it, and "
                "this frame is the game's own (RMP_ENTRY_POINT): nothing was "
                "drawn into a texture to blur, so the veil comes alone. "
                "RMP_GAME draws the frame for you; or ask overlay() for no blur");
        }
        return;
    }
    backdrop.asked = true;
    if (!backdrop.redirected) return;
    // RMP_COVERAGE_OFF(on the GPU: the unit tests have no window; tests/ui_render_test.cpp draws every line)
    backdrop.redirected = false;
    if (escaped()) {
        report_escape();
        return;
    }
    EndTextureMode();
    const float amount = ease_towards(true);
    draw_blurred(blur * ui_scale() * amount);
    backdrop.blurred = true;
}
// RMP_COVERAGE_ON

void finish_backdrop() {
    backdrop.in_app_frame = false;
    if (backdrop.redirected) {
        // RMP_COVERAGE_OFF(on the GPU: the unit tests have no window; tests/ui_render_test.cpp draws every line)
        // No overlay took it: the frame goes on screen as it was drawn.
        backdrop.redirected = false;
        if (escaped()) {
            report_escape();
        } else {
            back_on_screen();
        }
    }
    // RMP_COVERAGE_ON
    // A frame with no blur on screen lets the ease go back towards none, so
    // the next overlay blurs in from sharp -- or from where the last one left
    // it, when it comes back within a transition.
    if (!backdrop.blurred && backdrop.eased > 0.0f) ease_towards(false);
}

void release_backdrop() {
    if (backdrop.redirected && !escaped()) {
        // RMP_COVERAGE_OFF(on the GPU: the unit tests have no window; tests/ui_render_test.cpp draws every line)
        // rmp::ui::shutdown() in the middle of a redirected frame: what was
        // drawn so far goes on screen before its texture goes, and the rest
        // of the frame after it.
        back_on_screen();
    }
    // RMP_COVERAGE_ON
    // Not in_app_frame: the frame this may be called in is still the app's.
    backdrop.asked = false;
    backdrop.redirected = false;
    backdrop.blurred = false;
    backdrop.off = false;
    backdrop.eased = 0;
    gpu.scene = rmp::RenderTexture{};
    for (rmp::RenderTexture &half : gpu.halves) half = rmp::RenderTexture{};
    gpu.across = rmp::RenderTexture{};
    gpu.down = rmp::RenderTexture{};
    gpu.shader = rmp::Shader{};
    gpu.direction = -1;
    gpu.sigma = -1;
}

bool backdrop_redirected() { return backdrop.redirected; }
bool backdrop_blurred() { return backdrop.blurred; }

// --- the pure half --------------------------------------------------------------

Glsl glsl_for(const int gl_version) {
    switch (gl_version) {
        case RL_OPENGL_ES_20:
            return Glsl::ES_100;
        case RL_OPENGL_ES_30:
            return Glsl::ES_300;
        case RL_OPENGL_33:
        case RL_OPENGL_43:
            return Glsl::CORE_330;
        default:
            return Glsl::NONE;
    }
}

ShaderSource blur_shader_source(const Glsl glsl) {
    switch (glsl) {
        case Glsl::ES_100:
            return ShaderSource{ joined(VERTEX_100, {}, VERTEX_BODY),
                                 joined(FRAGMENT_100, {}, FRAGMENT_BODY) };
        case Glsl::ES_300:
            return ShaderSource{ joined("#version 300 es\n", VERTEX_IN_OUT, VERTEX_BODY),
                                 joined("#version 300 es\nprecision highp float;\n",
                                        FRAGMENT_IN_OUT, FRAGMENT_BODY) };
        case Glsl::CORE_330:
            return ShaderSource{ joined("#version 330\n", VERTEX_IN_OUT, VERTEX_BODY),
                                 joined("#version 330\n", FRAGMENT_IN_OUT,
                                        FRAGMENT_BODY) };
        case Glsl::NONE:
            break;
    }
    return ShaderSource{};
}

void set_test_blur_shader(std::optional<ShaderSource> source) {
    // What was built from the old source goes, and so does a machine given
    // up on because of it.
    release_backdrop();
    gpu.test_source = std::move(source);
}

BlurPlan plan_blur(const float sigma_px, const int frame_size) {
    if (!(sigma_px >= SMALLEST_SIGMA) || !std::isfinite(sigma_px)) return BlurPlan{};
    BlurPlan plan;
    plan.divisor = sigma_px < QUARTER_FROM ? 2 : 4;
    // The kernel reaches three sigmas with taps one texel apart: past what
    // BLUR_TAPS of them reach, the picture is halved again. Spreading the
    // taps instead skips texels, and a blur that skips them draws stripes.
    // Until it is one texel across: halving that makes nothing smaller, and
    // only pads the frame it is made from.
    while (3.0f * sigma_px > static_cast<float>(BLUR_TAPS * plan.divisor) &&
           plan.divisor < frame_size && plan.divisor < BLUR_MAX_DIVISOR) {
        plan.divisor *= 2;
    }
    plan.sigma = sigma_px / static_cast<float>(plan.divisor);
    return plan;
}

} // namespace rmp::ui::detail
