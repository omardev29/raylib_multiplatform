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
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <limits>
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
// read back before EndDrawing(). The screen holds something else when the
// frame starts, as a back buffer after a swap does: magenta, which no check
// expects to see.
template <class Draw> Shot frame(Draw &&draw) {
    rmp::app::detail::step_delta();
    rmp::ui::detail::begin_frame();
    BeginDrawing();
    ClearBackground(MAGENTA);
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
    bool redirected = true;
    const Shot shot = frame([&] {
        redirected = rmp::ui::detail::backdrop_redirected();
        draw_split(true);
    });
    check(!redirected,
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
    const Shot by_hand = split_under_rectangle(115); // 0.45 x 255, rounded
    // Frame after frame of it: whether each one was drawn into the backdrop
    // is asked while it is drawn, finish_backdrop() having put every frame
    // back on screen by the time frame() returns.
    bool redirected = false;
    bool blurred = false;
    bool all_veil = true;
    for (int i = 0; i < 3; ++i) {
        const Shot veiled = frame([&] {
            redirected = redirected || rmp::ui::detail::backdrop_redirected();
            draw_split(true);
            rmp::ui::overlay({ .blur = 0, .dim = 0.45f }, [] {});
        });
        blurred = blurred || rmp::ui::detail::backdrop_blurred();
        all_veil = identical(veiled, by_hand, "blur0_veil") && all_veil;
    }
    check(!redirected && !blurred,
          "blur 0, frame after frame: nothing redirected, nothing blurred");
    check(all_veil,
          "blur 0, dim 0.45: exactly the split under a rectangle of black at alpha 115");
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
    // Not a number at all is the default dim: the veil the game did not
    // change, not none.
    const Shot nan_dim =
        split_under({ .blur = 0, .dim = std::numeric_limits<float>::quiet_NaN() }, true);
    const Shot default_dim = split_under({ .blur = 0 }, true);
    check(identical(nan_dim, default_dim, "dim_nan"),
          "dim NaN is the default dim, 0.45: the same picture");
    const Shot none = split_under({ .blur = 0, .dim = -1 }, true);
    const Shot plain = frame([] { draw_split(true); });
    check(identical(none, plain, "dim_negative"), "dim -1 is dim 0: the frame untouched");
    check(rmp::detail::report_count() == reports + 1,
          "...and the three dims that are not one are one report");
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
        // Whether a frame is drawn into the backdrop is asked while it is
        // drawn: by the time frame() returns, it is back on screen.
        bool redirected = false;
        const auto plain_frame = [&] {
            return frame([&] {
                redirected = rmp::ui::detail::backdrop_redirected();
                draw_split(across);
            });
        };
        const Shot after = plain_frame();
        check(redirected && identical(after, plain, label + "_untaken"),
              label + ": a redirected frame no overlay took is the frame, exactly");
        plain_frame();
        check(!redirected, label + ": and then nothing is redirected");
    }
}

// A redirected frame no overlay took goes on screen as it was drawn, the
// pixels drawn see-through included: a copy that blended them over the screen
// would show what the screen held before through them.
void untaken_translucent() {
    rmp::ui::detail::set_test_blur_shader(std::nullopt);
    auto draw = [] {
        draw_split(true);
        DrawRectangle(WIDTH / 4, HEIGHT / 4, WIDTH / 2, HEIGHT / 2,
                      Color{ 255, 0, 0, 128 });
    };
    const Shot plain = frame(draw);
    split_under({ .blur = 8, .dim = 0 }, true); // asks
    bool redirected = false;
    const Shot after = frame([&] {
        redirected = rmp::ui::detail::backdrop_redirected();
        draw();
    });
    check(redirected && identical(after, plain, "untaken_translucent"),
          "a redirected frame no overlay took: its see-through pixels exactly as drawn");
    frame(draw);
}

// rmp::ui::shutdown() in the middle of a redirected frame lets the texture go
// with the rest of the UI: what was drawn into it so far goes on screen
// first, and what is drawn after it goes straight there.
void shut_down_mid_frame() {
    rmp::ui::detail::set_test_blur_shader(std::nullopt);
    const Shot plain = frame([] { draw_split(true); });
    split_under({ .blur = 8, .dim = 0 }, true); // asks
    bool redirected = false;
    const Shot shot = frame([&] {
        ClearBackground(BLACK);
        redirected = rmp::ui::detail::backdrop_redirected();
        rmp::ui::shutdown();
        DrawRectangle(WIDTH / 2, 0, WIDTH - WIDTH / 2, HEIGHT, WHITE);
    });
    check(
        redirected && identical(shot, plain, "shutdown_mid_frame"),
        "rmp::ui::shutdown() in a redirected frame: the frame is all there, both halves");
    frame([] { draw_split(true); });
}

// The game's blend factors are the game's: raylib keeps one set, which
// BLEND_CUSTOM draws with, and a frame the backdrop drew leaves it as it was.
void leaves_the_blend_factors() {
    rmp::ui::detail::set_test_blur_shader(std::nullopt);
    // Multiply: black stays black, white turns the grey drawn over it.
    rlSetBlendFactors(RL_DST_COLOR, RL_ZERO, RL_FUNC_ADD);
    auto draw = [] {
        draw_split(true);
        BeginBlendMode(BLEND_CUSTOM);
        DrawRectangle(0, 0, WIDTH, HEIGHT, Color{ 128, 128, 128, 255 });
        EndBlendMode();
    };
    const Shot before = frame(draw);
    check(colour_at(before, 0.25f, 0.5f).r == 0 &&
              colour_at(before, 0.75f, 0.5f).r == 128,
          "the game's BLEND_CUSTOM multiplies (the check below means something)");
    split_under({ .blur = 8, .dim = 0 }, true); // asks
    split_under({ .blur = 8, .dim = 0 }, true); // blurred
    const bool blurred = rmp::ui::detail::backdrop_blurred();
    const Shot after = frame(draw); // redirected, and taken by no overlay
    const Shot settled = frame(draw);
    check(blurred && identical(after, before, "blend_factors_redirected") &&
              identical(settled, before, "blend_factors_after"),
          "after a blurred frame the game's BLEND_CUSTOM still multiplies");
    rlSetBlendFactors(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_FUNC_ADD);
}

// One-pixel white lines on black, `every` pixels apart from `phase`: upright
// for a blur across, lying for one down.
void draw_lines(int every, int phase, bool across) {
    ClearBackground(BLACK);
    const int length = across ? WIDTH : HEIGHT;
    for (int at = phase; at < length; at += every) {
        if (across) {
            DrawRectangle(at, 0, 1, HEIGHT, WHITE);
        } else {
            DrawRectangle(0, at, WIDTH, 1, WHITE);
        }
    }
}

// A blur loses nothing: lines one pixel wide come out as their average grey,
// wherever they fall. The small pictures the blur is drawn at must be made
// from every pixel of the frame -- read sparsely, a line on one pixel
// vanished and the same line one pixel over came out twice as bright -- and
// their taps must be one texel apart: spread further, a wide blur over lines
// 16 pixels apart came out in stripes from 2 to 35 where it is 16 all over.
struct LinesUnder {
    float blur; // design units: half that in pixels, the scale being 0.5
    int every;
    int phase_step;
};
constexpr std::array<LinesUnder, 6> LINES_UNDER{
    LinesUnder{ 4, 2, 1 }, // 2 px: drawn at half the resolution
    LinesUnder{ 8, 4, 1 }, // 4 px: at a quarter
    LinesUnder{ 24, 16, 5 }, // 12 px and up: smaller still
    LinesUnder{ 40, 16, 5 }, LinesUnder{ 80, 16, 5 }, LinesUnder{ 160, 16, 5 },
};

void keeps_every_pixel() {
    rmp::ui::detail::set_test_blur_shader(std::nullopt);
    for (const LinesUnder &u : LINES_UNDER) {
        const float blur = u.blur;
        const int every = u.every;
        const int expected = 255 / every;
        for (const bool across : { true, false }) {
            for (int phase = 0; phase < every; phase += u.phase_step) {
                const std::string name = "lines_" +
                    std::to_string(static_cast<int>(blur)) + "_" + std::to_string(every) +
                    "_" + std::to_string(phase) + (across ? "_across" : "_down");
                auto draw = [&] {
                    draw_lines(every, phase, across);
                    rmp::ui::overlay({ .blur = blur, .dim = 0 }, [] {});
                };
                frame(draw); // asks
                const Shot shot = frame(draw);
                int low = 255;
                int high = 0;
                for (int y = HEIGHT / 4; y < 3 * HEIGHT / 4; ++y) {
                    for (int x = WIDTH / 4; x < 3 * WIDTH / 4; ++x) {
                        const int v = shot.at(x, y).r;
                        low = v < low ? v : low;
                        high = v > high ? v : high;
                    }
                }
                std::printf("      %s: %d..%d, the lines' average %d\n", name.c_str(),
                            low, high, expected);
                const int tolerance = every > 4 ? 4 : 10;
                const bool kept = rmp::ui::detail::backdrop_blurred() &&
                    low >= expected - tolerance && high <= expected + tolerance;
                if (!check(kept, name + ": the lines come out as their average grey")) {
                    save(shot, name, "actual");
                }
            }
        }
    }
    // Wider than the screen: the frame halved down to one texel, and that
    // one colour all over.
    auto widest = [] {
        draw_split(true);
        rmp::ui::overlay({ .blur = 1.0e6f, .dim = 0 }, [] {});
    };
    frame(widest); // asks
    const Shot flat = frame(widest);
    const Color corner = flat.at(0, 0);
    bool one_colour = rmp::ui::detail::backdrop_blurred();
    for (int y = 0; y < HEIGHT; y += 7) {
        for (int x = 0; x < WIDTH; x += 7) {
            if (std::abs(flat.at(x, y).r - corner.r) > 1) one_colour = false;
        }
    }
    std::printf("      a blur wider than the screen: %d all over\n", corner.r);
    if (!check(one_colour && corner.r > 32 && corner.r < 223,
               "a blur wider than the screen: one grey all over")) {
        save(flat, "widest", "actual");
    }
    frame([] { draw_split(true); });
    frame([] { draw_split(true); });
}

// The blur is in design units, as every size of the interface is: the same
// blur at twice the scale is twice as wide on screen.
void scales_with_the_interface() {
    rmp::ui::detail::set_test_blur_shader(std::nullopt);
    auto width_at = [](float scale) {
        rmp::ui::set_scale(scale);
        split_under({ .blur = 8, .dim = 0 }, true); // asks
        const Shot shot = split_under({ .blur = 8, .dim = 0 }, true);
        return ramp_width(shot.line(true));
    };
    const int half = width_at(0.5f);
    const int whole = width_at(1.0f);
    rmp::ui::set_scale(0);
    frame([] { draw_split(true); });
    frame([] { draw_split(true); });
    std::printf("      blur 8: a ramp %d pixels wide at scale 0.5, %d at 1\n", half,
                whole);
    check(half > 0 && 2 * whole >= 3 * half && whole <= 3 * half,
          "the blur is in design units: twice the scale, about twice as wide");
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

// What the backdrop said in the log while a check listened: how many times it
// gave up on the blur, a machine's condition said once.
struct {
    int gave_up = 0;
} heard;

void hear(int /*level*/, const char *text, va_list args) {
    std::array<char, 512> line{};
    std::vsnprintf(line.data(), line.size(), text, args);
    if (std::string_view(line.data()).find("cannot blur here") !=
        std::string_view::npos) {
        ++heard.gave_up;
    }
}

// A shader that does not build is a machine's condition: the veil alone, a
// line in the log -- one, however many frames ask -- and no report.
void a_shader_that_fails() {
    const int reports = rmp::detail::report_count();
    rmp::ui::detail::ShaderSource broken =
        rmp::ui::detail::blur_shader_source(rmp::ui::detail::Glsl::CORE_330);
    broken.fragment = "#version 330\nthis is not GLSL\n";
    rmp::ui::detail::set_test_blur_shader(broken);
    bool redirected = false;
    const Shot plain = frame([] { draw_split(true); });
    bool all_plain = true;
    heard.gave_up = 0;
    SetTraceLogLevel(LOG_INFO); // where the line is said
    SetTraceLogCallback(hear);
    for (int i = 0; i < 4; ++i) {
        const Shot shot = frame([&] {
            draw_split(true);
            redirected = redirected || rmp::ui::detail::backdrop_redirected();
            rmp::ui::overlay({ .blur = 8, .dim = 0 }, [] {});
        });
        all_plain = identical(shot, plain, "broken_shader") && all_plain;
    }
    SetTraceLogCallback(nullptr);
    SetTraceLogLevel(LOG_WARNING);
    check(!redirected && all_plain,
          "a shader that does not build: never redirected, the frame untouched (dim 0)");
    std::printf("      \"cannot blur here\" said %d times over four frames\n",
                heard.gave_up);
    check(heard.gave_up == 1,
          "...said once in the log, and not tried again every frame after it");
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
    // The frame after it is redirected too, and no overlay takes it: a scene
    // drawing through its own texture there is said as well -- by the
    // frame's last hook -- and the blur is off after it all the same.
    rmp::detail::reset_reports_for_tests();
    bool asked_for = false;
    frame([&] {
        asked_for = rmp::ui::detail::backdrop_redirected();
        draw_split(true);
        BeginTextureMode(own);
        ClearBackground(RED);
        EndTextureMode();
    });
    redirected = false;
    for (int i = 0; i < 3; ++i) {
        frame([&] {
            draw_split(true);
            redirected = redirected || rmp::ui::detail::backdrop_redirected();
            rmp::ui::overlay({ .blur = 8, .dim = 0 }, [] {});
        });
    }
    check(asked_for && rmp::detail::report_count() == 1 && !redirected,
          "...in a redirected frame no overlay took, too: reported, and the blur off");
    rmp::ui::shutdown();
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

// A size no divisor goes into: the small pictures are made from the frame as
// if it were a little larger, its edge going on, and cut back to it on the way
// to the screen. Cut wrong, the picture would be squeezed or stretched, and
// the blurred edge would no longer sit where the edge was.
void follows_an_odd_size() {
    constexpr int ODD_WIDTH = 333;
    constexpr int ODD_HEIGHT = 187;
    SetWindowSize(ODD_WIDTH, ODD_HEIGHT);
    if (!check(window_is(ODD_WIDTH, ODD_HEIGHT), "the window took an odd size")) return;
    for (const bool across : { true, false }) {
        const int edge = (across ? ODD_WIDTH : ODD_HEIGHT) / 2; // the first white pixel
        auto split = [&] {
            ClearBackground(BLACK);
            if (across) {
                DrawRectangle(edge, 0, ODD_WIDTH - edge, ODD_HEIGHT, WHITE);
            } else {
                DrawRectangle(0, edge, ODD_WIDTH, ODD_HEIGHT - edge, WHITE);
            }
            rmp::ui::overlay({ .blur = 8, .dim = 0 }, [] {});
        };
        frame(split);
        const Shot shot = frame(split);
        const int length = across ? ODD_WIDTH : ODD_HEIGHT;
        auto level = [&](int i) {
            return across ? shot.at(i, ODD_HEIGHT / 2).r : shot.at(ODD_WIDTH / 2, i).r;
        };
        const int before = level(edge - 1);
        const int after = level(edge);
        const std::string name = std::string("odd size ") + (across ? "across" : "down");
        std::printf("      %s: %d | %d at the edge, %d and %d at the ends\n",
                    name.c_str(), before, after, level(0), level(length - 1));
        if (!check(shot.width() == ODD_WIDTH && rmp::ui::detail::backdrop_blurred() &&
                       std::abs(before + after - 255) <= 12 && level(0) <= 2 &&
                       level(length - 1) >= 253,
                   name +
                       ": blurred, centred on the edge, black and white to the ends")) {
            save(shot, across ? "odd_across" : "odd_down", "actual");
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
    // Wider than any GPU takes a texture: raylib still hands back ids, of a
    // framebuffer the driver will not complete -- one that draws nothing.
    const rmp::RenderTexture refused =
        rmp::assets::detail::make_render_texture(1 << 17, 1);
    check(!refused.valid(),
          "make_render_texture(131072, 1), past every GPU's limit: empty, not a "
          "framebuffer "
          "that draws nothing");
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
    keeps_every_pixel();
    scales_with_the_interface();
    untaken_translucent();
    shut_down_mid_frame();
    leaves_the_blend_factors();
    blur_under_a_veil();
    eases_in();
    a_shader_that_fails();
    a_scene_with_its_own_texture();
    follows_a_resize();
    follows_an_odd_size();
    what_assets_makes();

    rmp::ui::detail::set_test_blur_shader(std::nullopt);
    rmp::ui::shutdown();
    CloseWindow();

    std::printf("\n%s\n", results.failures == 0 ? "PASS" : "FAILED");
    return results.failures == 0 ? 0 : 1;
}
