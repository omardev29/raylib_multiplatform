// ===========================================================================
// tests/ui_render_test.cpp — rmp::ui on a real GPU.
//
//   cmake --preset debug -DBUILD_UI_TESTS=ON
//   cmake --build --preset debug --target ui_render_test
//   ./build/debug/ui_render_test        (CI: xvfb-run -a, Mesa's llvmpipe)
//
// Everything else the interface does is checked with no window at all:
// tests/ui_layout_test.cpp and the unit tests lay it out headless, and the
// examples job draws it with raylib's software renderer. What overlay()'s
// backdrop draws cannot be checked either way -- the software renderer has
// no shaders, and the blur is a shader -- so this opens a hidden window on a
// real OpenGL, draws, and reads the pixels back.
//
// The blur's shader is built here in every dialect it ships in: GLSL ES 1.00
// (OpenGL ES 2.0, WebGL 1), ES 3.00 (ES 3.0, WebGL 2) and 330 (the desktop).
// Mesa takes ES shaders under the desktop context raylib opens, as long as
// both halves of a program are one version (the backdrop brings its own
// vertex shader for that), so the shader a phone or a browser builds is
// compiled, linked and drawn with here first. Measured on 2026-10-11 under
// llvmpipe: both ES pairs build and draw; an ES fragment shader beside
// raylib's 330 vertex shader is refused with "all shaders must use same
// shading language version".
//
// It prints one line per check and PASS or FAILED, and its exit status is the
// verdict: `rmp test ui-render`, and a step of the CI lint job. A picture that
// fails is written next to the binary, in ui-render/: what was drawn, and,
// where the check compared it with something, what was expected and the
// difference between them.
// ===========================================================================

#include "../src/rmp/internal.h"
#include "../src/rmp/ui/backdrop.h"
#include "../src/rmp/ui/internal.h"

#include <raylib.h>
#include <rlgl.h>
#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/ui.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr int WIDTH = 320;
constexpr int HEIGHT = 180;

struct {
    int failures = 0;
} results;

bool check(bool ok, std::string_view what) {
    std::printf("%s  %.*s\n", ok ? "ok  " : "FAIL", static_cast<int>(what.size()),
                what.data());
    if (!ok) ++results.failures;
    return ok;
}

bool same(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// A picture read back from the screen, unloaded when it goes.
class Shot {
public:
    explicit Shot(::Image image) : _image(image) {}
    Shot(const Shot &) = delete;
    Shot &operator=(const Shot &) = delete;
    Shot(Shot &&other) noexcept : _image(other.take()) {}
    Shot &operator=(Shot &&) = delete;
    ~Shot() { UnloadImage(_image); }

    [[nodiscard]] const ::Image &image() const { return _image; }
    [[nodiscard]] int width() const { return _image.width; }
    [[nodiscard]] int height() const { return _image.height; }
    [[nodiscard]] bool usable() const {
        return _image.width == WIDTH && _image.height == HEIGHT;
    }
    [[nodiscard]] Color at(int x, int y) const { return GetImageColor(_image, x, y); }
    // The grey level along a line through the middle: a row for a split
    // left to right, a column for one top to bottom.
    [[nodiscard]] std::vector<int> line(bool across) const {
        std::vector<int> out;
        const int n = across ? _image.width : _image.height;
        out.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            out.push_back(across ? at(i, _image.height / 2).r
                                 : at(_image.width / 2, i).r);
        }
        return out;
    }

private:
    // The picture handed over, and none left here to unload.
    ::Image take() noexcept { return std::exchange(_image, ::Image{}); }

    ::Image _image;
};

// The frame as it is now, read back before EndDrawing() swaps it away -- with
// raylib's batch drawn first, or the last of it is missing (CLAUDE.md).
Shot read_back() {
    rlDrawRenderBatchActive();
    return Shot{ LoadImageFromScreen() };
}

// The colour at a fraction of the picture's width and height.
Color colour_at(const Shot &shot, float fx, float fy) {
    return shot.at(static_cast<int>(fx * static_cast<float>(shot.width() - 1)),
                   static_cast<int>(fy * static_cast<float>(shot.height() - 1)));
}

// --- what a failure leaves behind -------------------------------------------

std::string out_dir() {
    std::string dir = std::string(GetApplicationDirectory()) + "ui-render/";
    MakeDirectory(dir.c_str());
    return dir;
}

void save(const Shot &shot, std::string_view name, std::string_view what) {
    const std::string path =
        out_dir() + std::string(name) + "." + std::string(what) + ".png";
    ExportImage(shot.image(), path.c_str());
    std::printf("      wrote %s\n", path.c_str());
}

// Every pixel of two pictures the same; on a failure, the expected, the actual
// and their difference, four times as bright, beside the binary.
bool identical(const Shot &actual, const Shot &expected, std::string_view name) {
    if (actual.width() != expected.width() || actual.height() != expected.height())
        return false;
    int differ = 0;
    ::Image diff = GenImageColor(actual.width(), actual.height(), BLACK);
    for (int y = 0; y < actual.height(); ++y) {
        for (int x = 0; x < actual.width(); ++x) {
            const Color a = actual.at(x, y);
            const Color e = expected.at(x, y);
            if (same(a, e)) continue;
            ++differ;
            auto scaled = [](int d) {
                return static_cast<unsigned char>(d * 4 > 255 ? 255 : d * 4);
            };
            ImageDrawPixel(&diff, x, y,
                           Color{ scaled(std::abs(a.r - e.r)),
                                  scaled(std::abs(a.g - e.g)),
                                  scaled(std::abs(a.b - e.b)), 255 });
        }
    }
    const Shot difference{ diff };
    if (differ > 0) {
        std::printf("      %d pixels differ\n", differ);
        save(actual, name, "actual");
        save(expected, name, "expected");
        save(difference, name, "diff");
    }
    return differ == 0;
}

// --- the frame, as rmp::app::detail::frame() draws it -----------------------

// Black on the left, white on the right -- or on top and below.
void draw_split(bool across) {
    ClearBackground(BLACK);
    if (across) {
        DrawRectangle(WIDTH / 2, 0, WIDTH - WIDTH / 2, HEIGHT, WHITE);
    } else {
        DrawRectangle(0, HEIGHT / 2, WIDTH, HEIGHT - HEIGHT / 2, WHITE);
    }
}

// One frame: the UI's frame boundary, BeginDrawing(), the backdrop's two
// hooks around what `draw` draws -- the scenes, in an app -- and the picture
// read back before EndDrawing().
template <class Draw> Shot frame(Draw &&draw) {
    rmp::app::detail::step_delta();
    rmp::ui::detail::begin_frame();
    BeginDrawing();
    rmp::ui::detail::begin_backdrop();
    draw();
    rmp::ui::detail::finish_backdrop();
    Shot shot = read_back();
    EndDrawing();
    rmp::ui::detail::end_frame();
    return shot;
}

// The split, and an overlay over it with nothing in it.
Shot split_under(const rmp::ui::OverlayOptions &o, bool across) {
    return frame([&] {
        draw_split(across);
        rmp::ui::overlay(o, [] {});
    });
}

// The split, and what the veil of `dim` is: a rectangle of black at that alpha.
Shot split_under_rectangle(unsigned char alpha) {
    return frame([&] {
        draw_split(true);
        DrawRectangle(0, 0, WIDTH, HEIGHT, Color{ 0, 0, 0, alpha });
    });
}

// The theme's transition, for a check that wants the blur at once.
void set_transition(float seconds) {
    rmp::ui::Theme t = rmp::ui::current_theme();
    t.transition = seconds;
    rmp::ui::set_theme(t);
}

// The width of the ramp along `line`: how many of its pixels are neither the
// black nor the white either side of it.
int ramp_width(const std::vector<int> &line) {
    int n = 0;
    for (const int v : line) {
        if (v > 12 && v < 243) ++n;
    }
    return n;
}

// --- the checks ---------------------------------------------------------------

// Through the backdrop's hooks, as rmp::app::detail::frame() calls them, with
// no overlay anywhere: the frame is exactly what was drawn.
void untouched_without_an_overlay() {
    const Shot shot = frame([] { draw_split(true); });
    check(!rmp::ui::detail::backdrop_redirected(),
          "no overlay asked for a blur, so the frame was drawn straight to the screen");
    check(shot.width() > 0 && shot.height() > 0, "the frame reads back");
    if (shot.width() > 0 && shot.height() > 0) {
        check(same(colour_at(shot, 0.05f, 0.5f), BLACK) &&
                  same(colour_at(shot, 0.45f, 0.5f), BLACK),
              "the left half is black, to its edge");
        check(same(colour_at(shot, 0.55f, 0.5f), WHITE) &&
                  same(colour_at(shot, 0.95f, 0.5f), WHITE),
              "the right half is white, from its edge");
    }
}

// blur 0 is a rectangle of the veil at dim's alpha over the frame, and not a
// pixel more: the same picture as drawing that rectangle by hand.
void blur_zero_is_the_veil() {
    const Shot veiled = split_under({ .blur = 0, .dim = 0.45f }, true);
    check(!rmp::ui::detail::backdrop_redirected() && !rmp::ui::detail::backdrop_blurred(),
          "blur 0: nothing redirected, nothing blurred");
    const Shot by_hand = split_under_rectangle(115); // 0.45 x 255, rounded
    check(identical(veiled, by_hand, "blur0_veil"),
          "blur 0, dim 0.45: exactly the split under a rectangle of black at alpha 115");
    const Shot next = split_under({ .blur = 0, .dim = 0.45f }, true);
    check(!rmp::ui::detail::backdrop_redirected(),
          "...and the frame after it is not redirected either");
}

// dim is how far towards the veil: half way turns white into half white.
void dim_half_is_half() {
    const Shot shot = split_under({ .blur = 0, .dim = 0.5f }, true);
    const Color white_side = colour_at(shot, 0.75f, 0.5f);
    const Color black_side = colour_at(shot, 0.25f, 0.5f);
    std::printf("      white under dim 0.5 = %d\n", white_side.r);
    if (!check(white_side.r >= 125 && white_side.r <= 131 && black_side.r == 0,
               "dim 0.5: white comes out 128 +/- 3, black stays black")) {
        save(shot, "dim_half", "actual");
    }
    // Out of range is the game's mistake, reported once, and made into the
    // nearest dim there is.
    const int reports = rmp::detail::report_count();
    const Shot full = split_under({ .blur = 0, .dim = 7 }, true);
    check(same(colour_at(full, 0.75f, 0.5f), BLACK) &&
              same(colour_at(full, 0.25f, 0.5f), BLACK),
          "dim 7 is dim 1: the veil all over");
    const Shot none = split_under({ .blur = 0, .dim = -1 }, true);
    const Shot plain = frame([] { draw_split(true); });
    check(identical(none, plain, "dim_negative"), "dim -1 is dim 0: the frame untouched");
    check(rmp::detail::report_count() == reports + 1,
          "...and the two out-of-range dims are one report");
}

// What a blur of 8 design units -- 4 pixels here, the UI's scale being its
// floor of 0.5 at 320 x 180 -- does to the edge of the split: far from it,
// nothing; across it, a smooth ramp centred where the edge was.
bool ramp_is_a_blur(const Shot &shot, bool across, std::string_view name) {
    const std::vector<int> line = shot.line(across);
    const int edge = (across ? WIDTH : HEIGHT) / 2; // first white pixel
    const int length = static_cast<int>(line.size());
    const auto level = [&line](int i) { return line.at(static_cast<size_t>(i)); };
    constexpr int SIGMA = 4;
    bool ok = true;
    // Far from the edge the picture is what it was, within 2 levels.
    for (int i = 0; i < length; ++i) {
        if (i < edge - 6 * SIGMA && level(i) > 2) ok = false;
        if (i >= edge + 6 * SIGMA && level(i) < 253) ok = false;
    }
    check(ok,
          std::string(name) +
              ": far from the edge, black is black and white white (+/-2)");
    // Across it, never darker on the way to the white side.
    bool rising = true;
    for (int i = edge - 6 * SIGMA; i < edge + 6 * SIGMA; ++i) {
        if (level(i + 1) + 1 < level(i)) rising = false;
    }
    check(rising, std::string(name) + ": the ramp only rises across the edge");
    // The two pixels either side of the edge are the middle of it...
    const int before = level(edge - 1);
    const int after = level(edge);
    std::printf("      %.*s: at the edge %d | %d, a sigma out %d | %d\n",
                static_cast<int>(name.size()), name.data(), before, after,
                level(edge - SIGMA), level(edge + SIGMA - 1));
    const bool middle = before >= 77 && before <= 178 && after >= 77 && after <= 178;
    check(middle, std::string(name) + ": the pixels either side of the edge are 30-70 %");
    // ...and it is a blur, not a step: a sigma out, it has not got there yet.
    const int out_black = level(edge - SIGMA);
    const int out_white = level(edge + SIGMA - 1);
    const bool soft =
        out_black >= 5 && out_black <= 115 && out_white >= 140 && out_white <= 250;
    check(soft,
          std::string(name) +
              ": a sigma either side, still on its way (2-45 %, 55-98 %)");
    // Symmetric about the edge: nothing moved.
    const bool centred = std::abs((before + after) - 255) <= 12;
    check(centred, std::string(name) + ": centred on the edge, not moved by a pixel");
    const bool all = ok && rising && middle && soft && centred;
    if (!all) save(shot, name, "actual");
    return all;
}

// The blur, in the dialect `source` names (nullopt: rlGetVersion()'s), both
// ways round.
void blurs_in(std::optional<rmp::ui::detail::ShaderSource> source,
              std::string_view name) {
    rmp::ui::detail::set_test_blur_shader(std::move(source));
    for (const bool across : { true, false }) {
        const std::string label = std::string(name) + (across ? " across" : " down");
        const Shot plain = frame([&] { draw_split(across); });
        // The frame it is asked for has the veil alone: nothing was redirected.
        const Shot first = split_under({ .blur = 8, .dim = 0 }, across);
        check(identical(first, plain, label + "_first"),
              label + ": the frame that first asks has no blur yet (dim 0: untouched)");
        // From the next, blurred.
        const Shot blurred = frame([&] {
            draw_split(across);
            check(rmp::ui::detail::backdrop_redirected(),
                  label + ": the frame after it is drawn into the backdrop");
            rmp::ui::overlay({ .blur = 8, .dim = 0 }, [] {});
            check(!rmp::ui::detail::backdrop_redirected(),
                  label + ": the overlay took it back to the screen");
        });
        check(rmp::ui::detail::backdrop_blurred(), label + ": and put it there blurred");
        ramp_is_a_blur(blurred, across, label);
        // A frame no overlay asks for again: drawn into the backdrop once
        // more, and put on screen exactly as drawn.
        const Shot after = frame([&] { draw_split(across); });
        check(identical(after, plain, label + "_untaken"),
              label + ": a redirected frame no overlay took is the frame, exactly");
        const Shot settled = frame([&] { draw_split(across); });
        check(!rmp::ui::detail::backdrop_redirected(),
              label + ": and then nothing is redirected");
    }
}

// dim and blur together: the blur under the veil.
void blur_under_a_veil() {
    rmp::ui::detail::set_test_blur_shader(std::nullopt);
    split_under({ .blur = 8, .dim = 0.5f }, true);
    const Shot shot = split_under({ .blur = 8, .dim = 0.5f }, true);
    const Color far_white = colour_at(shot, 0.95f, 0.5f);
    const Color edge = shot.at(WIDTH / 2, HEIGHT / 2);
    if (!check(far_white.r >= 125 && far_white.r <= 131 && edge.r > 30 && edge.r < 100,
               "blur 8 under dim 0.5: far white is half white, the edge half of that")) {
        save(shot, "blur_dim", "actual");
    }
    frame([] { draw_split(true); });
}

// The blur eases in over the theme's transition: narrow the first blurred
// frame, all of it a transition later -- and then exactly the blur asked for.
void eases_in() {
    rmp::ui::detail::set_test_blur_shader(std::nullopt);
    const Shot full = [&] {
        split_under({ .blur = 8, .dim = 0 }, true);
        return split_under({ .blur = 8, .dim = 0 }, true);
    }();
    frame([] { draw_split(true); });
    frame([] { draw_split(true); });

    set_transition(0.12f);
    rmp::app::detail::set_fixed_delta(1.0f / 60.0f);
    split_under({ .blur = 8, .dim = 0 }, true); // asks
    const Shot first = split_under({ .blur = 8, .dim = 0 }, true);
    std::array<int, 12> widths{};
    widths[0] = ramp_width(first.line(true));
    for (size_t i = 1; i < widths.size(); ++i) {
        const Shot later = split_under({ .blur = 8, .dim = 0 }, true);
        widths[i] = ramp_width(later.line(true));
    }
    const Shot last = split_under({ .blur = 8, .dim = 0 }, true);
    std::printf(
        "      ramp width, frame by frame: %d %d %d %d ... %d; asked for at once: %d\n",
        widths[0], widths[1], widths[2], widths[3], widths[11],
        ramp_width(full.line(true)));
    bool growing = true;
    for (size_t i = 1; i < widths.size(); ++i) {
        if (widths[i] < widths[i - 1]) growing = false;
    }
    check(widths[0] < widths[11] && growing,
          "the blur widens frame by frame over the transition, never narrowing");
    check(identical(last, full, "eased_in"),
          "a transition later it is exactly the blur asked for at once");
    // Gone for a transition, it eases back out to none: the next overlay
    // blurs in from sharp again, as the first did.
    for (int i = 0; i < 12; ++i) frame([] { draw_split(true); });
    split_under({ .blur = 8, .dim = 0 }, true); // asks
    const Shot again = split_under({ .blur = 8, .dim = 0 }, true);
    check(ramp_width(again.line(true)) == widths[0],
          "gone for a transition, the next overlay blurs in from sharp again");
    // Back within a transition, it carries on from where it was: wider at
    // once than a blur starting from sharp.
    for (int i = 0; i < 4; ++i) split_under({ .blur = 8, .dim = 0 }, true);
    frame([] { draw_split(true); });
    frame([] { draw_split(true); });
    split_under({ .blur = 8, .dim = 0 }, true); // asks
    const Shot back = split_under({ .blur = 8, .dim = 0 }, true);
    std::printf("      back within a transition: ramp width %d\n",
                ramp_width(back.line(true)));
    check(ramp_width(back.line(true)) > widths[0],
          "back within a transition, it carries on from where it was");
    rmp::app::detail::set_fixed_delta(0);
    set_transition(0);
    frame([] { draw_split(true); });
    frame([] { draw_split(true); });
}

// A shader that does not build is a machine's condition: the veil alone, a
// line in the log, and no report.
void a_shader_that_fails() {
    const int reports = rmp::detail::report_count();
    rmp::ui::detail::ShaderSource broken =
        rmp::ui::detail::blur_shader_source(rmp::ui::detail::Glsl::CORE_330);
    broken.fragment = "#version 330\nthis is not GLSL\n";
    rmp::ui::detail::set_test_blur_shader(broken);
    bool redirected = false;
    const Shot plain = frame([] { draw_split(true); });
    bool all_plain = true;
    for (int i = 0; i < 4; ++i) {
        const Shot shot = frame([&] {
            draw_split(true);
            redirected = redirected || rmp::ui::detail::backdrop_redirected();
            rmp::ui::overlay({ .blur = 8, .dim = 0 }, [] {});
        });
        all_plain = identical(shot, plain, "broken_shader") && all_plain;
    }
    check(!redirected && all_plain,
          "a shader that does not build: never redirected, the frame untouched (dim 0)");
    check(rmp::detail::report_count() == reports,
          "...and it is not the game's mistake: nothing reported");
    rmp::ui::detail::set_test_blur_shader(std::nullopt);
    frame([] { draw_split(true); });
}

// A scene that draws through its own render texture while the frame is
// redirected has sent the rest of the frame to the screen: reported once, and
// the blur off -- the veil alone -- until the UI starts again.
void a_scene_with_its_own_texture() {
    rmp::ui::detail::set_test_blur_shader(std::nullopt);
    const RenderTexture2D own = LoadRenderTexture(32, 32);
    const int reports = rmp::detail::report_count();
    split_under({ .blur = 8, .dim = 0 }, true); // asks
    frame([&] {
        draw_split(true);
        BeginTextureMode(own);
        ClearBackground(RED);
        EndTextureMode();
        rmp::ui::overlay({ .blur = 8, .dim = 0 }, [] {});
    });
    check(rmp::detail::report_count() == reports + 1 &&
              !rmp::ui::detail::backdrop_blurred(),
          "a scene's own EndTextureMode() under the blur: reported, nothing blurred");
    bool redirected = false;
    for (int i = 0; i < 3; ++i) {
        frame([&] {
            draw_split(true);
            redirected = redirected || rmp::ui::detail::backdrop_redirected();
            rmp::ui::overlay({ .blur = 8, .dim = 0 }, [] {});
        });
    }
    check(!redirected && rmp::detail::report_count() == reports + 1,
          "...once, and the blur is off from there on");
    // The UI again from nothing: the blur with it.
    rmp::ui::shutdown();
    split_under({ .blur = 8, .dim = 0 }, true);
    split_under({ .blur = 8, .dim = 0 }, true);
    check(rmp::ui::detail::backdrop_blurred(),
          "after rmp::ui::shutdown() it blurs again");
    frame([] { draw_split(true); });
    UnloadRenderTexture(own);
}

// The window changes size: the backdrop follows it. A window manager may take
// a few frames to say so, and a frame drawn between the screen's size and the
// framebuffer's agreeing is not one to measure.
bool window_is(int width, int height) {
    for (int i = 0; i < 60; ++i) {
        if (GetScreenWidth() == width && GetScreenHeight() == height &&
            GetRenderWidth() == width && GetRenderHeight() == height) {
            return true;
        }
        frame([] { ClearBackground(BLACK); });
    }
    return false;
}

void follows_a_resize() {
    SetWindowSize(WIDTH * 2, HEIGHT * 2);
    if (!check(window_is(WIDTH * 2, HEIGHT * 2), "the window doubled its size")) return;
    // Black on the left, white from x = 320 of 640; the blur is 8 x the
    // scale at 640 x 360 (0.8) = 6.4 pixels.
    auto split = [] {
        ClearBackground(BLACK);
        DrawRectangle(WIDTH, 0, WIDTH, HEIGHT * 2, WHITE);
        rmp::ui::overlay({ .blur = 8, .dim = 0 }, [] {});
    };
    frame(split);
    const Shot shot = frame(split);
    const bool sized = shot.width() == WIDTH * 2 && shot.height() == HEIGHT * 2;
    std::printf("      read back %d x %d, blurred: %s\n", shot.width(), shot.height(),
                rmp::ui::detail::backdrop_blurred() ? "yes" : "no");
    check(sized && rmp::ui::detail::backdrop_blurred(), "at the new size, it blurs");
    if (sized) {
        const int left = shot.at(WIDTH - 40, HEIGHT).r;
        const int before = shot.at(WIDTH - 1, HEIGHT).r;
        const int after = shot.at(WIDTH, HEIGHT).r;
        const int right = shot.at(WIDTH + 40, HEIGHT).r;
        std::printf("      at twice the size: %d .. %d | %d .. %d\n", left, before, after,
                    right);
        if (!check(left <= 2 && right >= 253 && before >= 77 && before <= 178 &&
                       after >= 77 && after <= 178,
                   "...centred on the edge at its new place, the whole window covered")) {
            save(shot, "resized", "actual");
        }
    }
    SetWindowSize(WIDTH, HEIGHT);
    check(window_is(WIDTH, HEIGHT), "and back");
    frame([] { draw_split(true); });
    frame([] { draw_split(true); });
}

// The two GPU objects rmp::assets makes for the backdrop.
void what_assets_makes() {
    const rmp::RenderTexture texture = rmp::assets::detail::make_render_texture(16, 8);
    check(texture.valid() && texture.raw().id != 0 && texture.raw().texture.width == 16 &&
              texture.raw().texture.height == 8,
          "make_render_texture(16, 8): a framebuffer 16 x 8");
    const rmp::ui::detail::ShaderSource source =
        rmp::ui::detail::blur_shader_source(rmp::ui::detail::Glsl::CORE_330);
    const rmp::Shader shader =
        rmp::assets::detail::make_shader(source.vertex, source.fragment);
    check(shader.valid() && shader.raw().id != rlGetShaderIdDefault(),
          "make_shader(): the blur, built, and not raylib's default in its place");
    const rmp::Shader fragment_only =
        rmp::assets::detail::make_shader({},
                                         "#version 330\nout vec4 finalColor;\n"
                                         "void main() { finalColor = vec4(1.0); }\n");
    check(fragment_only.valid(),
          "make_shader() with no vertex source: raylib's own vertex shader");
    const rmp::Shader not_glsl =
        rmp::assets::detail::make_shader(source.vertex, "not GLSL");
    check(!not_glsl.valid(), "a fragment that does not compile: empty");
    const rmp::ui::detail::ShaderSource es =
        rmp::ui::detail::blur_shader_source(rmp::ui::detail::Glsl::ES_100);
    const rmp::Shader mixed =
        rmp::assets::detail::make_shader(es.vertex, source.fragment);
    check(!mixed.valid(),
          "two halves that do not link (ES 1.00 and 330): empty, not raylib's default");
}

} // namespace

int main() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_HIDDEN);
    InitWindow(WIDTH, HEIGHT, "ui_render_test");
    check(IsWindowReady(), "a hidden window opened");
    if (!IsWindowReady()) {
        std::printf("\nFAILED: no window -- under CI this runs inside xvfb-run\n");
        return 1;
    }

    // A GL that can build a shader. 1.1 and 2.1 cannot run the blur, and the
    // software renderer is not a GPU: either one here is the wrong machine.
    const int gl = rlGetVersion();
    std::printf("      rlGetVersion() = %d\n", gl);
    check(gl == RL_OPENGL_33 || gl == RL_OPENGL_43 || gl == RL_OPENGL_ES_20 ||
              gl == RL_OPENGL_ES_30,
          "the context is OpenGL 3.3, 4.3, ES 2.0 or ES 3.0");
    check(rmp::ui::detail::glsl_for(gl) != rmp::ui::detail::Glsl::NONE,
          "...and the backdrop has a dialect of GLSL for it");

    // The blur at once, unless a check says otherwise.
    set_transition(0);

    untouched_without_an_overlay();
    blur_zero_is_the_veil();
    dim_half_is_half();
    using rmp::ui::detail::Glsl;
    blurs_in(rmp::ui::detail::blur_shader_source(Glsl::ES_100), "GLSL ES 1.00");
    blurs_in(rmp::ui::detail::blur_shader_source(Glsl::ES_300), "GLSL ES 3.00");
    blurs_in(rmp::ui::detail::blur_shader_source(Glsl::CORE_330), "GLSL 330");
    blurs_in(std::nullopt, "rlGetVersion()'s");
    blur_under_a_veil();
    eases_in();
    a_shader_that_fails();
    a_scene_with_its_own_texture();
    follows_a_resize();
    what_assets_makes();

    rmp::ui::detail::set_test_blur_shader(std::nullopt);
    rmp::ui::shutdown();
    CloseWindow();

    std::printf("\n%s\n", results.failures == 0 ? "PASS" : "FAILED");
    return results.failures == 0 ? 0 : 1;
}
