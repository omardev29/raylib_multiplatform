// ---------------------------------------------------------------------------
// rmp::Camera, held to what rmp/scene.h says of it for any camera a game can
// set up, on any screen, in each of the three [window] stretch modes:
//
//   - to_screen() is the middle of the game's area plus the distance from the
//     camera, rotated and zoomed -- the stretch's scale on top of the zoom --
//     checked against the area and scale rmp/app.h defines for each mode,
//     written down here a second time and in double; view() is that area
//     divided by the same scale, centred on the position, and what lands on
//     the game's area is exactly what view() says is visible;
//   - to_world() undoes to_screen(), shaking or not;
//   - after a settle the view keeps inside the limits on every bounded axis
//     and is centred on a limit narrower than itself, and what is DRAWN --
//     shaking -- never shows past them either;
//   - a shake stays within the strongest strength asked for, never moves the
//     position or view(), is exactly nothing once its time has run out, and
//     one that is refused changes nothing at all;
//   - smoothing composes (two settles of half a delta land where one whole
//     delta does), never passes its target, and a new target is snapped to;
//   - no number a game can hand it -- NaN, the infinities, the largest
//     floats, in any field, any argument, the followed object's position --
//     leaves a number that is not finite in what it draws or where it is, a
//     target that was not a number for a while is followed again once it is,
//     a read never reports, a camera handed only finite numbers reports
//     nothing, and one that meets a number that is not finite says so once.
//
// A counterexample one of these finds becomes a named case in
// tests/camera_test.cpp. Each was seen red: the commits that added them name
// the defect or the hand mutation.
//
// covers: rmp::Camera::detail_settle(float)
// covers: rmp::Camera::raylib() const
// covers: rmp::Camera::shake(float, float)
// covers: rmp::Camera::shake_offset() const
// covers: rmp::Camera::to_screen(Vector2) const
// covers: rmp::Camera::to_world(Vector2) const
// covers: rmp::Camera::view() const
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/internal.h"
#include "../../src/rmp/object_internal.h"

#include <rmp/app.h>
#include <rmp/object.h>
#include <rmp/scene.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <ostream>
#include <string_view>
#include <vector>

namespace {

using rmp::app::Stretch;

constexpr double DESIGN_W = RMP_WINDOW_WIDTH;
constexpr double DESIGN_H = RMP_WINDOW_HEIGHT;
constexpr float INF = std::numeric_limits<float>::infinity();

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

// What the camera reports is logged, and hundreds of cases would print
// hundreds of lines: quiet for the length of one property -- silent, for the
// one that turns [dev] strict on, whose lines are errors.
struct QuietLog {
    explicit QuietLog(const int level = LOG_ERROR) { SetTraceLogLevel(level); }
    ~QuietLog() { SetTraceLogLevel(LOG_INFO); }
    QuietLog(const QuietLog &) = delete;
    QuietLog &operator=(const QuietLog &) = delete;
    QuietLog(QuietLog &&) = delete;
    QuietLog &operator=(QuietLog &&) = delete;
};

// How often [dev] strict would have stopped the program. The dot says "file
// state".
struct {
    int stops = 0;
} strictness;

// One of 0..count-1, every one as likely at any size of the run: RapidCheck's
// inRange() narrows its range with the size, and the first cases of a
// property would only ever see the first category.
int pick(int count) {
    return *rc::gen::resize(rmp::test::gen::NOMINAL_SIZE, rc::gen::inRange(0, count));
}

// A float from lo to hi, both included, in 2^16 even steps -- at any size of
// the run, for the same reason as pick().
float real(float lo, float hi) {
    constexpr int STEPS = 1 << 16;
    const int k =
        *rc::gen::resize(rmp::test::gen::NOMINAL_SIZE, rc::gen::inRange(0, STEPS + 1));
    return static_cast<float>(lo + ((static_cast<double>(hi) - lo) * k) / STEPS);
}

bool finite(const Vector2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
bool finite(const Rectangle &r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
        std::isfinite(r.height);
}
bool same(const Vector2 a, const Vector2 b) { return a.x == b.x && a.y == b.y; }
bool same(const Rectangle &a, const Rectangle &b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}
double length(const Vector2 v) { return std::hypot(double{ v.x }, double{ v.y }); }

std::string_view name_of(const Stretch mode) {
    switch (mode) {
        case Stretch::NONE:
            return "NONE";
        case Stretch::LETTERBOX:
            return "LETTERBOX";
        case Stretch::EXPAND:
            return "EXPAND";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// The screen, and the stretch on it as rmp/app.h defines each mode: NONE draws
// the world at 1:1 over the whole window; LETTERBOX fits the design in it,
// centred, at the scale that keeps its shape; EXPAND scales the same and gives
// the camera the whole window. Written from those words, in double -- not from
// src/rmp/stretch.cpp.
// ---------------------------------------------------------------------------

struct Screen {
    float width = static_cast<float>(DESIGN_W);
    float height = static_cast<float>(DESIGN_H);
    Stretch mode = Stretch::NONE;
};

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const Screen &s, std::ostream &os) {
    os << s.width << "x" << s.height << " " << name_of(s.mode);
}

rc::Gen<Screen> screen() {
    return rc::gen::exec([] {
        Screen s;
        s.mode = std::to_array({ Stretch::NONE, Stretch::LETTERBOX,
                                 Stretch::EXPAND })[static_cast<std::size_t>(pick(3))];
        switch (pick(7)) {
            case 0: // the design size
                break;
            case 1: // a window of one pixel
                s.width = 1;
                s.height = 1;
                break;
            case 2:
                s.width = 4096;
                s.height = 4096;
                break;
            case 3: // a phone held upright
                s.width = 1080;
                s.height = 2400;
                break;
            case 4: // much wider than the design
                s.width = 3840;
                s.height = 1080;
                break;
            default:
                s.width = std::round(real(1, 8192));
                s.height = std::round(real(1, 8192));
                break;
        }
        return s;
    });
}

struct Area {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    double scale = 1; // the stretch's pixels per world unit, before the zoom
};

Area area_of(const Screen &s) {
    const double fit = std::min(s.width / DESIGN_W, s.height / DESIGN_H);
    switch (s.mode) {
        case Stretch::LETTERBOX:
            return Area{ (s.width - (DESIGN_W * fit)) / 2,
                         (s.height - (DESIGN_H * fit)) / 2, DESIGN_W * fit,
                         DESIGN_H * fit, fit };
        case Stretch::EXPAND:
            return Area{ 0, 0, s.width, s.height, fit };
        case Stretch::NONE:
            break;
    }
    return Area{ 0, 0, s.width, s.height, 1 };
}

// The screen and the mode for the length of one case, and the test's own put
// back however it ends. reset_for_tests() also puts the screen back, so a case
// that spawns calls it first.
struct OnScreen {
    Stretch was = rmp::app::stretch();
    explicit OnScreen(const Screen &s) {
        rmp::objects::detail::set_test_screen(s.width, s.height);
        rmp::app::set_stretch(s.mode);
    }
    ~OnScreen() {
        rmp::objects::detail::set_test_screen(0, 0);
        rmp::app::set_stretch(was);
    }
    OnScreen(const OnScreen &) = delete;
    OnScreen &operator=(const OnScreen &) = delete;
    OnScreen(OnScreen &&) = delete;
    OnScreen &operator=(OnScreen &&) = delete;
};

// ---------------------------------------------------------------------------
// A camera a game sets up on purpose: anywhere within a hundred thousand
// units, a zoom from a sixteenth to sixteen -- as likely to halve as to
// double -- and, when it may turn, any rotation within two turns either way.
// ---------------------------------------------------------------------------

struct Pose {
    Screen screen;
    Vector2 position{};
    float zoom = 1;
    float rotation = 0;
};

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const Pose &p, std::ostream &os) {
    showValue(p.screen, os);
    os << ", position (" << p.position.x << ", " << p.position.y << "), zoom " << p.zoom
       << ", rotation " << p.rotation;
}

float any_position() {
    switch (pick(4)) {
        case 0:
            return 0;
        case 1:
            return real(-2000, 2000);
        default:
            return real(-100000, 100000);
    }
}

rc::Gen<Pose> pose(const bool turns) {
    return rc::gen::exec([turns] {
        Pose p;
        p.screen = *screen();
        p.position = pick(4) == 0 ? Vector2{ static_cast<float>(DESIGN_W / 2),
                                             static_cast<float>(DESIGN_H / 2) }
                                  : Vector2{ any_position(), any_position() };
        switch (pick(3)) {
            case 0:
                p.zoom = 1;
                break;
            case 1:
                p.zoom = std::exp2(static_cast<float>(pick(9) - 4));
                break;
            default:
                p.zoom = std::exp2(real(-4, 4));
                break;
        }
        p.rotation = turns && pick(3) > 0 ? real(-720, 720) : 0;
        return p;
    });
}

void place(rmp::Camera &camera, const Pose &p) {
    camera.position = p.position;
    camera.zoom = p.zoom;
    camera.rotation = p.rotation;
}

// The model's view of a pose: the game's area in world units, centred on the
// position.
struct View {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
};

View view_of(const Pose &p) {
    const Area a = area_of(p.screen);
    const double ppu = p.zoom * a.scale;
    const double w = a.width / ppu;
    const double h = a.height / ppu;
    return View{ p.position.x - (w / 2), p.position.y - (h / 2), w, h };
}

// Where the model draws a world point: the middle of the area, plus the
// distance from the camera, rotated clockwise on the screen (y points down)
// and scaled by the zoom times the stretch's scale.
struct Point {
    double x = 0;
    double y = 0;
};

Point screen_of(const Pose &p, const Vector2 world) {
    const Area a = area_of(p.screen);
    const double ppu = p.zoom * a.scale;
    const double turn = p.rotation * (std::numbers::pi / 180);
    const double dx = double{ world.x } - p.position.x;
    const double dy = double{ world.y } - p.position.y;
    return Point{
        a.x + (a.width / 2) + (ppu * ((std::cos(turn) * dx) - (std::sin(turn) * dy))),
        a.y + (a.height / 2) + (ppu * ((std::sin(turn) * dx) + (std::cos(turn) * dy)))
    };
}

// A world point somewhere around the view: inside it, at its edge, or up to
// half a view past it.
Vector2 point_near(const Pose &p) {
    const View v = view_of(p);
    const double cx = v.x + (v.width / 2);
    const double cy = v.y + (v.height / 2);
    return Vector2{ static_cast<float>(cx + (real(-1, 1) * v.width)),
                    static_cast<float>(cy + (real(-1, 1) * v.height)) };
}

// What float arithmetic owes a computation whose terms reach `magnitude`:
// raylib's matrices cancel terms as large as the zoomed distance from the
// world's origin, so the error grows with that, not with the answer.
double slack(const double magnitude) { return 2e-6 * (magnitude + 1); }

// ---------------------------------------------------------------------------
// Steps a game takes, for the properties that play a sequence: every field
// it may write, the followed object moving, a shake, and the scene's settle.
// ---------------------------------------------------------------------------

enum class Kind {
    POSITION,
    ZOOM,
    ROTATION,
    SMOOTHING,
    LIMITS,
    TARGET,
    FOLLOW,
    SHAKE,
    SETTLE
};
constexpr int KINDS = 9;

struct Step {
    Kind kind = Kind::SETTLE;
    float a = 0;
    float b = 0;
    float c = 0;
    float d = 0;
};

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const Step &s, std::ostream &os) {
    constexpr auto NAMES =
        std::to_array({ "position", "zoom", "rotation", "smoothing", "limits", "target",
                        "follow", "shake", "settle" });
    os << NAMES[static_cast<std::size_t>(s.kind)] << "(" << s.a << ", " << s.b << ", "
       << s.c << ", " << s.d << ")";
}

// A finite number a game should not hand the camera, and may: the largest
// floats, the smallest, a denormal, past int's range, -0.
float odd_finite() {
    constexpr float LARGEST = std::numeric_limits<float>::max();
    return *rc::gen::element(LARGEST, -LARGEST, std::numeric_limits<float>::min(),
                             std::numeric_limits<float>::denorm_min(), 2.2e9f, -2.2e9f,
                             1e30f, -1e30f, -0.0f);
}

// A step whose every number is a hostile float: NaN, an infinity, the largest
// and smallest floats, values past int's range, or an ordinary one.
rc::Gen<Step> hostile_step() {
    return rc::gen::exec([] {
        Step s;
        s.kind = static_cast<Kind>(pick(KINDS));
        s.a = *rmp::test::gen::hostile_float();
        s.b = *rmp::test::gen::hostile_float();
        s.c = *rmp::test::gen::hostile_float();
        s.d = *rmp::test::gen::hostile_float();
        return s;
    });
}

void play(World &world, rmp::Object &target, const Step &s) {
    rmp::Camera &camera = world.camera;
    switch (s.kind) {
        case Kind::POSITION:
            camera.position = Vector2{ s.a, s.b };
            break;
        case Kind::ZOOM:
            camera.zoom = s.a;
            break;
        case Kind::ROTATION:
            camera.rotation = s.a;
            break;
        case Kind::SMOOTHING:
            camera.smoothing = s.a;
            break;
        case Kind::LIMITS:
            camera.limits = Rectangle{ s.a, s.b, s.c, s.d };
            break;
        case Kind::TARGET:
            target.position = Vector2{ s.a, s.b };
            break;
        case Kind::FOLLOW:
            camera.follow = s.a > 0 ? target.handle() : rmp::Handle<rmp::Object>();
            break;
        case Kind::SHAKE:
            camera.shake(s.a, s.b);
            break;
        case Kind::SETTLE:
            camera.detail_settle(s.a);
            break;
    }
}

// What the camera draws with, and what it says is visible, are numbers a
// renderer can use: finite, and a zoom and a view that are not empty.
void assert_drawable(const rmp::Camera &camera) {
    const Camera2D drawn = camera.raylib();
    RC_ASSERT(finite(drawn.offset));
    RC_ASSERT(finite(drawn.target));
    RC_ASSERT(std::isfinite(drawn.rotation));
    RC_ASSERT(std::isfinite(drawn.zoom));
    RC_ASSERT(drawn.zoom > 0);
    const Rectangle v = camera.view();
    RC_ASSERT(finite(v));
    RC_ASSERT(v.width > 0);
    RC_ASSERT(v.height > 0);
    RC_ASSERT(finite(camera.shake_offset()));
}

// Limits of one axis, as a game might set them around a view `view` wide
// centred on `at`: none (a zero or a negative size), wider than the view,
// exactly as wide, or narrower.
struct Span {
    float edge = 0;
    float size = 0;
};

Span any_span(const double at, const double view) {
    switch (pick(5)) {
        case 0:
            return Span{ static_cast<float>(at + real(-1000, 1000)), 0 };
        case 1:
            return Span{ static_cast<float>(at), -real(1, 1000) };
        case 2: {
            const double size = view * real(1.01f, 20);
            return Span{ static_cast<float>(at + (real(-1.5f, 0.5f) * size)),
                         static_cast<float>(size) };
        }
        case 3:
            return Span{ static_cast<float>(at - (view / 2)), static_cast<float>(view) };
        default: {
            const double size = view * real(0.01f, 0.99f);
            return Span{ static_cast<float>(at + (real(-3, 3) * view)),
                         static_cast<float>(size) };
        }
    }
}

// Whether the view [lo, lo + size) of one axis keeps inside the span, or is
// centred on a span narrower than itself -- or the axis is unbounded.
bool keeps_to(const double lo, const double size, const Span &span,
              const double magnitude) {
    if (!(span.size > 0)) return true;
    const double tolerance = slack(magnitude);
    if (span.size <= size) {
        return std::abs((lo + (size / 2)) - (span.edge + (double{ span.size } / 2))) <=
            tolerance;
    }
    return lo >= span.edge - tolerance &&
        lo + size <= double{ span.edge } + span.size + tolerance;
}

} // namespace

TEST_SUITE("property: camera") {
    TEST_CASE("to_screen is the game's area plus the zoomed, turned distance") {
        rmp::test::check(
            "to_screen agrees with the stretch's area and scale in every mode", [] {
                const Pose p = *pose(true);
                const OnScreen on(p.screen);
                World world;
                place(world.camera, p);
                const Vector2 at = point_near(p);
                const Point expected = screen_of(p, at);
                const Vector2 got = world.camera.to_screen(at);
                const Area a = area_of(p.screen);
                const double magnitude =
                    (p.zoom * a.scale *
                     (std::abs(at.x) + std::abs(at.y) + std::abs(p.position.x) +
                      std::abs(p.position.y))) +
                    a.width + a.height + a.x + a.y;
                RC_ASSERT(std::abs(got.x - expected.x) <= slack(magnitude));
                RC_ASSERT(std::abs(got.y - expected.y) <= slack(magnitude));
            });
    }

    TEST_CASE("view() is what lands on the game's area") {
        rmp::test::check(
            "view() is the area over the zoom and the stretch, centred, corner to corner",
            [] {
                const Pose p = *pose(false);
                const OnScreen on(p.screen);
                World world;
                place(world.camera, p);
                const View expected = view_of(p);
                const Rectangle got = world.camera.view();
                const double magnitude = std::abs(p.position.x) + std::abs(p.position.y) +
                    expected.width + expected.height;
                RC_ASSERT(std::abs(got.x - expected.x) <= slack(magnitude));
                RC_ASSERT(std::abs(got.y - expected.y) <= slack(magnitude));
                RC_ASSERT(std::abs(got.width - expected.width) <= slack(expected.width));
                RC_ASSERT(std::abs(got.height - expected.height) <=
                          slack(expected.height));
                // And it IS what is visible: its corners land on the area's.
                const Area a = area_of(p.screen);
                const Vector2 top_left = world.camera.to_screen(Vector2{ got.x, got.y });
                const Vector2 bottom_right = world.camera.to_screen(
                    Vector2{ got.x + got.width, got.y + got.height });
                const double pixels =
                    (p.zoom * a.scale * magnitude) + a.width + a.height + a.x + a.y;
                RC_ASSERT(std::abs(top_left.x - a.x) <= slack(pixels));
                RC_ASSERT(std::abs(top_left.y - a.y) <= slack(pixels));
                RC_ASSERT(std::abs(bottom_right.x - (a.x + a.width)) <= slack(pixels));
                RC_ASSERT(std::abs(bottom_right.y - (a.y + a.height)) <= slack(pixels));
            });
    }

    TEST_CASE("to_world undoes to_screen") {
        rmp::test::check("to_world(to_screen(p)) is p in every mode, shaking or not", [] {
            const Pose p = *pose(true);
            const OnScreen on(p.screen);
            World world;
            place(world.camera, p);
            if (pick(2) == 0) {
                world.camera.shake(real(0.5f, 200), real(0.05f, 2));
                const int frames = pick(30);
                for (int i = 0; i < frames; ++i) world.camera.detail_settle(1.0f / 60);
            }
            const Vector2 at = point_near(p);
            const Vector2 back = world.camera.to_world(world.camera.to_screen(at));
            const Area a = area_of(p.screen);
            const double magnitude = std::abs(at.x) + std::abs(at.y) +
                std::abs(p.position.x) + std::abs(p.position.y) + 400 +
                ((a.width + a.height + a.x + a.y) / (p.zoom * a.scale));
            RC_ASSERT(std::abs(back.x - at.x) <= slack(magnitude));
            RC_ASSERT(std::abs(back.y - at.y) <= slack(magnitude));
        });
    }

    TEST_CASE("limits after a settle") {
        rmp::test::check(
            "after a settle the view keeps inside the limits, "
            "or is centred on a narrower one",
            [] {
                rmp::objects::detail::reset_for_tests();
                const Pose p = *pose(false);
                const OnScreen on(p.screen);
                World world;
                place(world.camera, p);
                const View v = view_of(p);
                const Span sx = any_span(p.position.x, v.width);
                const Span sy = any_span(p.position.y, v.height);
                world.camera.limits = Rectangle{ sx.edge, sy.edge, sx.size, sy.size };
                rmp::Object &target = world.spawn({});
                const bool following = pick(2) == 0;
                if (following) {
                    target.position = Vector2{ any_position(), any_position() };
                    world.camera.follow = target.handle();
                    world.camera.smoothing = pick(2) == 0 ? 0 : real(0.1f, 30);
                    world.camera.detail_settle(0);
                    target.position = Vector2{ any_position(), any_position() };
                }
                world.camera.detail_settle(real(0, 0.1f));
                const Rectangle got = world.camera.view();
                const double magnitude = std::abs(sx.edge) + std::abs(sy.edge) +
                    std::abs(sx.size) + std::abs(sy.size) + v.width + v.height +
                    std::abs(p.position.x) + std::abs(p.position.y) +
                    std::abs(target.position.x) + std::abs(target.position.y);
                RC_ASSERT(keeps_to(got.x, got.width, sx, magnitude));
                RC_ASSERT(keeps_to(got.y, got.height, sy, magnitude));
                // An unbounded axis of a camera that follows nothing stays put.
                if (!following && !(sx.size > 0))
                    RC_ASSERT(world.camera.position.x == p.position.x);
                if (!following && !(sy.size > 0))
                    RC_ASSERT(world.camera.position.y == p.position.y);
            });
    }

    TEST_CASE("what is drawn never shows past the limits") {
        rmp::test::check(
            "a shaking camera draws nothing past its limits, and no further than its "
            "strength from its position",
            [] {
                const Pose p = *pose(false);
                const OnScreen on(p.screen);
                World world;
                place(world.camera, p);
                const View v = view_of(p);
                const Span sx = any_span(p.position.x, v.width);
                const Span sy = any_span(p.position.y, v.height);
                world.camera.limits = Rectangle{ sx.edge, sy.edge, sx.size, sy.size };
                world.camera.detail_settle(0);
                const float strength = real(0.5f, 400);
                world.camera.shake(strength, real(0.05f, 1));
                const Area a = area_of(p.screen);
                // In world units, and the screen's own rounding divided by the
                // scale: on a letterbox one pixel tall a pixel is a long way.
                const double magnitude = std::abs(sx.edge) + std::abs(sy.edge) +
                    std::abs(sx.size) + std::abs(sy.size) + v.width + v.height +
                    std::abs(p.position.x) + std::abs(p.position.y) + strength +
                    ((a.width + a.height + a.x + a.y) / (p.zoom * a.scale));
                const int frames = 1 + pick(40);
                for (int i = 0; i < frames; ++i) {
                    world.camera.detail_settle(real(0, 0.05f));
                    const Vector2 top_left = world.camera.to_world(
                        Vector2{ static_cast<float>(a.x), static_cast<float>(a.y) });
                    const Vector2 bottom_right = world.camera.to_world(
                        Vector2{ static_cast<float>(a.x + a.width),
                                 static_cast<float>(a.y + a.height) });
                    RC_ASSERT(
                        keeps_to(top_left.x, bottom_right.x - top_left.x, sx, magnitude));
                    RC_ASSERT(
                        keeps_to(top_left.y, bottom_right.y - top_left.y, sy, magnitude));
                    const Camera2D drawn = world.camera.raylib();
                    const Vector2 moved{ drawn.target.x - world.camera.position.x,
                                         drawn.target.y - world.camera.position.y };
                    RC_ASSERT(length(moved) <=
                              (strength * (1 + 1e-6)) + slack(magnitude));
                }
            });
    }

    TEST_CASE("a shake is bounded, invisible to gameplay, and ends") {
        rmp::test::check(
            "a shake stays within the strongest strength, never moves position or view, "
            "and is exactly nothing once its time has run out",
            [] {
                const Pose p = *pose(true);
                const OnScreen on(p.screen);
                World world;
                place(world.camera, p);
                const Vector2 position = world.camera.position;
                const Rectangle view = world.camera.view();
                double strongest = 0;
                double longest = 0;
                double since = 0; // since the last shake() call, accepted or not
                const int steps = 1 + pick(60);
                for (int i = 0; i < steps; ++i) {
                    if (pick(4) == 0) {
                        const float strength = pick(5) == 0 ? -real(0, 10) : real(0, 300);
                        const float seconds = pick(5) == 0 ? -real(0, 1) : real(0, 2);
                        world.camera.shake(strength, seconds);
                        if (strength > 0 && seconds > 0) {
                            strongest = std::max(strongest, double{ strength });
                            longest = std::max(longest, double{ seconds });
                        }
                        since = 0;
                        continue;
                    }
                    const float dt = pick(6) == 0 ? 0 : real(0, 0.2f);
                    world.camera.detail_settle(dt);
                    since += dt;
                    const Vector2 off = world.camera.shake_offset();
                    RC_ASSERT(length(off) <= strongest * (1 + 1e-6));
                    RC_ASSERT(same(world.camera.position, position));
                    RC_ASSERT(same(world.camera.view(), view));
                    // With no limits, what is drawn moves by exactly the offset.
                    const Camera2D drawn = world.camera.raylib();
                    RC_ASSERT(drawn.target.x == position.x + off.x);
                    RC_ASSERT(drawn.target.y == position.y + off.y);
                    if (since > (longest * (1 + 1e-4)) + 1e-6) {
                        RC_ASSERT(off.x == 0.0f);
                        RC_ASSERT(off.y == 0.0f);
                    }
                }
            });
    }

    TEST_CASE("a refused shake changes nothing") {
        rmp::test::check(
            "a shake that is not a positive finite strength and duration changes nothing",
            [] {
                const QuietLog quiet;
                World a;
                World b;
                const float bad = *rc::gen::element(
                    0.0f, -1.0f, -INF, INF, std::numeric_limits<float>::quiet_NaN());
                const float good = real(0.01f, 100);
                const bool bad_strength = pick(2) == 0;
                const int before = pick(20);
                const int after = 1 + pick(20);
                const auto settle_both = [&a, &b](const float dt) {
                    a.camera.detail_settle(dt);
                    b.camera.detail_settle(dt);
                };
                const float strength = real(1, 100);
                const float seconds = real(0.1f, 1);
                a.camera.shake(strength, seconds);
                b.camera.shake(strength, seconds);
                for (int i = 0; i < before; ++i) settle_both(1.0f / 60);
                if (bad_strength) {
                    b.camera.shake(bad, good);
                } else {
                    b.camera.shake(good, bad);
                }
                for (int i = 0; i < after; ++i) {
                    settle_both(1.0f / 60);
                    RC_ASSERT(same(a.camera.shake_offset(), b.camera.shake_offset()));
                    RC_ASSERT(same(a.camera.raylib().target, b.camera.raylib().target));
                }
                rmp::detail::reset_reports_for_tests();
            });
    }

    TEST_CASE("smoothing composes") {
        rmp::test::check(
            "two settles of half a delta land where one whole delta does", [] {
                rmp::objects::detail::reset_for_tests();
                const float rate = pick(4) == 0 ? real(30, 200) : real(0.05f, 30);
                const float dt = real(0.0001f, 0.5f);
                const int pieces = 2 + pick(7);
                const Vector2 from{ any_position(), any_position() };
                const Vector2 to{ any_position(), any_position() };
                const auto settle_in = [&](const int count) {
                    World world;
                    rmp::Object &target = world.spawn({ .position = from });
                    world.camera.follow = target.handle();
                    world.camera.smoothing = rate;
                    world.camera.detail_settle(0); // acquires it: a snap
                    target.position = to;
                    for (int i = 0; i < count; ++i)
                        world.camera.detail_settle(dt / static_cast<float>(count));
                    return world.camera.position;
                };
                const Vector2 one = settle_in(1);
                const Vector2 many = settle_in(pieces);
                const double distance =
                    std::abs(double{ to.x } - from.x) + std::abs(double{ to.y } - from.y);
                const double tolerance =
                    (1e-5 * distance) + slack(std::abs(from.x) + std::abs(from.y));
                RC_ASSERT(std::abs(double{ one.x } - many.x) <= tolerance);
                RC_ASSERT(std::abs(double{ one.y } - many.y) <= tolerance);
            });
    }

    TEST_CASE("smoothing never passes its target") {
        rmp::test::check(
            "a smoothed camera moves towards its target and never past it", [] {
                rmp::objects::detail::reset_for_tests();
                World world;
                rmp::Object &target =
                    world.spawn({ .position = { any_position(), any_position() } });
                world.camera.follow = target.handle();
                world.camera.smoothing = real(0.05f, 60);
                world.camera.detail_settle(0);
                const int steps = 1 + pick(40);
                for (int i = 0; i < steps; ++i) {
                    if (pick(5) == 0)
                        target.position = { any_position(), any_position() };
                    const Vector2 was = world.camera.position;
                    const Vector2 to = target.position;
                    world.camera.detail_settle(pick(5) == 0 ? 0 : real(0, 0.5f));
                    const Vector2 now = world.camera.position;
                    RC_ASSERT(now.x >= std::min(was.x, to.x));
                    RC_ASSERT(now.x <= std::max(was.x, to.x));
                    RC_ASSERT(now.y >= std::min(was.y, to.y));
                    RC_ASSERT(now.y <= std::max(was.y, to.y));
                }
            });
    }

    TEST_CASE("a new target is snapped to") {
        rmp::test::check(
            "a new target is snapped to, whatever the smoothing and delta", [] {
                const QuietLog quiet;
                rmp::objects::detail::reset_for_tests();
                World world;
                const rmp::Object &a =
                    world.spawn({ .position = { any_position(), any_position() } });
                const rmp::Object &b =
                    world.spawn({ .position = { any_position(), any_position() } });
                world.camera.smoothing = *rmp::test::gen::hostile_float();
                world.camera.follow = a.handle();
                world.camera.detail_settle(*rmp::test::gen::hostile_float());
                RC_ASSERT(same(world.camera.position, a.position));
                world.camera.follow = b.handle();
                world.camera.detail_settle(*rmp::test::gen::hostile_float());
                RC_ASSERT(same(world.camera.position, b.position));
                rmp::detail::reset_reports_for_tests();
            });
    }

    TEST_CASE("no number a game hands the camera leaves it undrawable") {
        rmp::test::check(
            "after any hostile number in any field, the camera draws with finite numbers "
            "and settles somewhere finite",
            [] {
                const QuietLog quiet;
                rmp::objects::detail::reset_for_tests();
                const OnScreen on(*screen());
                World world;
                rmp::Object &target = world.spawn({});
                const auto steps = *rc::gen::container<std::vector<Step>>(hostile_step());
                for (const Step &s : steps) {
                    play(world, target, s);
                    assert_drawable(world.camera);
                    if (s.kind == Kind::SETTLE) RC_ASSERT(finite(world.camera.position));
                    // Any point at all converts without undefined behaviour.
                    static_cast<void>(world.camera.to_screen(Vector2{ s.b, s.c }));
                    static_cast<void>(world.camera.to_world(Vector2{ s.c, s.d }));
                }
                rmp::detail::reset_reports_for_tests();
            });
    }

    TEST_CASE("a target that was not a number is followed again") {
        rmp::test::check("a smoothed camera whose target was not a number for a while "
                         "follows it again once it is",
                         [] {
                             const QuietLog quiet;
                             rmp::objects::detail::reset_for_tests();
                             World world;
                             rmp::Object &target = world.spawn(
                                 { .position = { any_position(), any_position() } });
                             const float rate = real(0.5f, 50);
                             world.camera.follow = target.handle();
                             world.camera.smoothing = rate;
                             world.camera.detail_settle(1.0f / 60);
                             const int lost = 1 + pick(5);
                             for (int i = 0; i < lost; ++i) {
                                 target.position =
                                     Vector2{ *rmp::test::gen::hostile_float(),
                                              *rmp::test::gen::hostile_float() };
                                 world.camera.detail_settle(1.0f / 60);
                             }
                             const Vector2 back{ any_position(), any_position() };
                             target.position = back;
                             // Every frame from then on, closer on each axis, or as close.
                             const int frames = 1 + pick(10);
                             for (int i = 0; i < frames; ++i) {
                                 const Vector2 was = world.camera.position;
                                 world.camera.detail_settle(1.0f / 60);
                                 const Vector2 now = world.camera.position;
                                 RC_ASSERT(finite(now));
                                 RC_ASSERT(std::abs(double{ now.x } - back.x) <=
                                           std::abs(double{ was.x } - back.x));
                                 RC_ASSERT(std::abs(double{ now.y } - back.y) <=
                                           std::abs(double{ was.y } - back.y));
                             }
                             // And a long enough frame is all the way: e^-40 of it left,
                             // which a float cannot tell from none.
                             world.camera.detail_settle(40 / rate);
                             RC_ASSERT(same(world.camera.position, back));
                             rmp::detail::reset_reports_for_tests();
                         });
    }

    TEST_CASE("a read never reports") {
        rmp::test::check(
            "reading a camera reports nothing, whatever its fields hold", [] {
                const QuietLog quiet;
                rmp::detail::reset_reports_for_tests();
                World world;
                world.camera.position = Vector2{ *rmp::test::gen::hostile_float(),
                                                 *rmp::test::gen::hostile_float() };
                world.camera.zoom = *rmp::test::gen::hostile_float();
                world.camera.rotation = *rmp::test::gen::hostile_float();
                world.camera.limits = *rmp::test::gen::hostile_rect();
                const Vector2 at{ *rmp::test::gen::hostile_float(),
                                  *rmp::test::gen::hostile_float() };
                static_cast<void>(world.camera.raylib());
                static_cast<void>(world.camera.view());
                static_cast<void>(world.camera.shake_offset());
                static_cast<void>(world.camera.to_screen(at));
                static_cast<void>(world.camera.to_world(at));
                RC_ASSERT(rmp::detail::report_count() == 0);
            });
    }

    TEST_CASE("a camera handed finite numbers reports nothing") {
        rmp::test::check(
            "a camera handed only finite numbers, however odd, "
            "and an infinite smoothing, never reports",
            [] {
                const QuietLog quiet;
                rmp::detail::reset_reports_for_tests();
                rmp::objects::detail::reset_for_tests();
                const Pose p = *pose(true);
                const OnScreen on(p.screen);
                World world;
                place(world.camera, p);
                // The odd ones: a zoom of nothing, less, or past what it
                // draws; the largest floats as a place, a turn or a limit.
                if (pick(3) == 0) {
                    world.camera.zoom = *rc::gen::element(
                        0.0f, -2.0f, 1e-30f, 1e30f, -std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max());
                }
                if (pick(4) == 0) world.camera.rotation = odd_finite();
                if (pick(4) == 0) world.camera.position.x = odd_finite();
                rmp::Object &target =
                    world.spawn({ .position = { any_position(), any_position() } });
                world.camera.follow =
                    pick(2) == 0 ? target.handle() : rmp::Handle<rmp::Object>();
                switch (pick(4)) {
                    case 0:
                        world.camera.smoothing = -real(0, 10);
                        break;
                    case 1:
                        world.camera.smoothing = INF; // a snap, as documented
                        break;
                    default:
                        world.camera.smoothing = real(0, 40);
                        break;
                }
                world.camera.limits = pick(4) == 0
                    ? Rectangle{ std::numeric_limits<float>::max(),
                                 -std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::max(),
                                 std::numeric_limits<float>::max() }
                    : Rectangle{ any_position(), any_position(), real(-5000, 50000),
                                 real(-5000, 50000) };
                const int steps = 1 + pick(20);
                for (int i = 0; i < steps; ++i) {
                    if (pick(3) == 0) world.camera.shake(real(-10, 300), real(-1, 2));
                    if (pick(4) == 0)
                        target.position = { any_position(), any_position() };
                    world.camera.detail_settle(real(-0.1f, 0.2f));
                }
                RC_ASSERT(rmp::detail::report_count() == 0);
            });
    }

    TEST_CASE("a number that is not finite is said once") {
        rmp::test::check(
            "each field or argument that is not a finite number is reported once, and "
            "[dev] strict stops on it",
            [] {
                const QuietLog quiet(LOG_NONE);
                rmp::objects::detail::reset_for_tests();
                rmp::detail::reset_reports_for_tests();
                rmp::detail::set_strict(true);
                const rmp::detail::StrictHandler previous =
                    rmp::detail::set_strict_handler([] { ++strictness.stops; });
                strictness.stops = 0;
                World world;
                rmp::Object &target = world.spawn({});
                const float bad = *rc::gen::element(
                    std::numeric_limits<float>::quiet_NaN(),
                    -std::numeric_limits<float>::quiet_NaN(), INF, -INF);
                const bool on_x = pick(2) == 0;
                const int field = pick(8);
                for (int again = 0; again < 3; ++again) {
                    switch (field) {
                        case 0:
                            (on_x ? world.camera.position.x : world.camera.position.y) =
                                bad;
                            break;
                        case 1:
                            world.camera.zoom = bad;
                            break;
                        case 2:
                            world.camera.rotation = bad;
                            break;
                        case 3:
                            switch (pick(4)) {
                                case 0:
                                    world.camera.limits.x = bad;
                                    break;
                                case 1:
                                    world.camera.limits.y = bad;
                                    break;
                                case 2:
                                    world.camera.limits.width = bad;
                                    break;
                                default:
                                    world.camera.limits.height = bad;
                                    break;
                            }
                            break;
                        case 4: // +inf is a snap, as documented
                            world.camera.smoothing = bad == INF ? -INF : bad;
                            break;
                        case 5:
                            world.camera.follow = target.handle();
                            (on_x ? target.position.x : target.position.y) = bad;
                            break;
                        case 6:
                            world.camera.shake(bad, 0.5f);
                            break;
                        default:
                            world.camera.shake(5, bad);
                            break;
                    }
                    world.camera.detail_settle(1.0f / 60);
                }
                const int said = rmp::detail::report_count();
                const int stops = strictness.stops;
                rmp::detail::set_strict(false);
                rmp::detail::set_strict_handler(previous);
                rmp::detail::reset_reports_for_tests();
                RC_ASSERT(said == 1);
                RC_ASSERT(stops == 1);
            });
    }
}
