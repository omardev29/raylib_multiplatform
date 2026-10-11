// ---------------------------------------------------------------------------
// What floats over the interface, headless: at(), tooltip(), notify().
//
// The camera at() projects through is the scene's, handed over by the scene
// stack (set_pass_camera()); a tooltip is about the control declared last
// (take_last_control()); a notice and a report on screen only queue, so
// neither may start the UI by itself. And whatever each of them draws, the
// frame around it still balances.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/internal.h"
#include "../src/rmp/scene_internal.h"
#include "../src/rmp/ui/floating.h"
#include "../src/rmp/ui/internal.h"

#include <rmp/object.h> // rmp::Callback, for the report sink
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using rmp::ui::Align;

static_assert(std::is_default_constructible_v<rmp::ui::detail::AtCloser> &&
              !std::is_copy_constructible_v<rmp::ui::detail::AtCloser> &&
              !std::is_copy_assignable_v<rmp::ui::detail::AtCloser> &&
              !std::is_move_constructible_v<rmp::ui::detail::AtCloser> &&
              !std::is_move_assignable_v<rmp::ui::detail::AtCloser>);

constexpr float VIEW_W = 1280;
constexpr float VIEW_H = 720;

// Clay's MeasureFn: its last parameter is a C pointer Clay hands back untouched.
// NOLINTBEGIN(custom-rmp-raw-pointer): Clay's signature, not ours to choose
Clay_Dimensions measure_stub(Clay_StringSlice text, Clay_TextElementConfig *config,
                             void * /*user*/) {
    // NOLINTEND(custom-rmp-raw-pointer)
    const float size = config != nullptr ? static_cast<float>(config->fontSize) : 16.0f;
    return Clay_Dimensions{ static_cast<float>(text.length) * size * 0.5f, size };
}

// The same, and a report on the way: what ui_font() says the first time a
// [ui] font cannot be loaded -- which can be while the notices are measured,
// when nothing else has measured any text yet.
// NOLINTBEGIN(custom-rmp-raw-pointer): Clay's signature, not ours to choose
Clay_Dimensions measure_reporting(Clay_StringSlice text, Clay_TextElementConfig *config,
                                  void *user) {
    // NOLINTEND(custom-rmp-raw-pointer)
    RMP_REPORT_ONCE("test: a report while the notices are measured");
    return measure_stub(text, config, user);
}

// More reports than the ring holds, all while one notice is measured: the
// newest of them are what is kept, as in the ring itself.
struct {
    int said = 0;
} flood;

// NOLINTBEGIN(custom-rmp-raw-pointer): Clay's signature, not ours to choose
Clay_Dimensions measure_flooding(Clay_StringSlice text, Clay_TextElementConfig *config,
                                 void *user) {
    // NOLINTEND(custom-rmp-raw-pointer)
    for (; flood.said < rmp::ui::detail::NOTICE_RING + 2; ++flood.said) {
        rmp::ui::detail::show_report("flood " + std::to_string(flood.said));
    }
    return measure_stub(text, config, user);
}

// Where the test puts the pointer, and whether it is held down. Away from
// everything until a test says otherwise.
struct {
    Clay_Vector2 at{ -1.0f, -1.0f };
    bool down = false;
} finger;

void pointer_scripted(Clay_Vector2 &position, bool &down) {
    position = finger.at;
    down = finger.down;
}

// What the keyboard says: nothing, unless a test pushes sideways -- which
// shows the focus without moving it.
struct {
    int x = 0;
} keys;

void nav_scripted(rmp::ui::detail::NavState &out) {
    out = rmp::ui::detail::NavState{};
    out.x = keys.x;
}

Camera2D identity_camera() {
    return Camera2D{
        .offset = Vector2{ 0, 0 }, .target = Vector2{ 0, 0 }, .rotation = 0, .zoom = 1
    };
}

struct HeadlessUi {
    HeadlessUi() {
        rmp::scenes::detail::shutdown();
        rmp::ui::detail::set_measure_provider(measure_stub);
        finger.at = Clay_Vector2{ -1.0f, -1.0f };
        finger.down = false;
        keys.x = 0;
        rmp::ui::detail::set_pointer_provider(pointer_scripted);
        rmp::ui::detail::set_nav_provider(nav_scripted);
        rmp::ui::detail::set_test_viewport(VIEW_W, VIEW_H);
        rmp::ui::detail::forget_notices();
        rmp::ui::detail::set_pass_camera(identity_camera());
        // A focus some other test showed would show this one's tooltips.
        rmp::ui::focus("");
        rmp::ui::detail::set_focus_visible(false);
        // And keys some other test's game read -- a Platformer's jump reads
        // Space, which the interface navigates with -- would make this frame
        // a game's HUD, whose focus shows no tooltip.
        (void)rmp::ui::detail::take_game_keys();
    }
    HeadlessUi(const HeadlessUi &) = delete;
    HeadlessUi &operator=(const HeadlessUi &) = delete;
    ~HeadlessUi() {
        rmp::scenes::detail::shutdown();
        rmp::ui::detail::forget_notices();
        rmp::ui::detail::set_test_frame_time(0);
        rmp::ui::detail::set_pass_camera(identity_camera());
        rmp::ui::set_scale(0);
        rmp::ui::set_navigation_enabled(true);
        rmp::ui::focus("");
        rmp::ui::detail::set_focus_visible(false);
        rmp::detail::set_report_sink({});
        rmp::ui::detail::set_measure_provider(rmp::ui::detail::measure_with_raylib);
        rmp::ui::detail::set_pointer_provider(rmp::ui::detail::pointer_from_raylib);
        rmp::ui::detail::set_nav_provider(rmp::ui::detail::nav_from_raylib);
        rmp::ui::detail::set_test_viewport(0, 0);
    }
};

bool same(const Camera2D &a, const Camera2D &b) {
    return a.offset.x == b.offset.x && a.offset.y == b.offset.y &&
        a.target.x == b.target.x && a.target.y == b.target.y &&
        a.rotation == b.rotation && a.zoom == b.zoom;
}

bool identity(const Camera2D &c) { return same(c, identity_camera()); }

// A scene with a camera of its own, writing down the camera its _draw() is
// handed.
struct {
    Camera2D seen{};
    Camera2D expected{};
} drawn;

class Zoomed : public rmp::Scene {
public:
    Zoomed() {
        camera.position = Vector2{ 1000, 300 };
        camera.zoom = 2;
    }
    void _draw() override {
        drawn.seen = rmp::ui::detail::pass_camera();
        drawn.expected = camera.raylib();
    }
};

void run_frame() {
    rmp::ui::detail::begin_frame();
    rmp::scenes::detail::update(1.0f / 60.0f);
    rmp::scenes::detail::draw();
    rmp::ui::detail::end_frame();
    rmp::scenes::detail::apply_pending();
}

// rmp::app::detail::frame(), as far as the UI is concerned: the notices
// after every scene.
void run_app_frame() {
    rmp::ui::detail::begin_frame();
    rmp::scenes::detail::update(1.0f / 60.0f);
    rmp::scenes::detail::draw();
    rmp::ui::detail::draw_notices();
    rmp::ui::detail::end_frame();
    rmp::scenes::detail::apply_pending();
}

bool has_box(std::string_view label) {
    Clay_BoundingBox box{};
    return rmp::ui::detail::bounds_of(label, 0, 0, &box) && box.width > 0 &&
        box.height > 0;
}

Clay_BoundingBox box_of(std::string_view label) {
    Clay_BoundingBox box{};
    REQUIRE(rmp::ui::detail::bounds_of(label, 0, 0, &box));
    return box;
}

// The point of `b` that `a` names.
Vector2 anchor_of(const Clay_BoundingBox &b, Align a) {
    float x = b.x;
    float y = b.y;
    switch (a) {
        case Align::TOP_LEFT:
            break;
        case Align::TOP_CENTER:
            x += b.width / 2;
            break;
        case Align::TOP_RIGHT:
            x += b.width;
            break;
        case Align::CENTER_LEFT:
            y += b.height / 2;
            break;
        case Align::CENTER:
            x += b.width / 2;
            y += b.height / 2;
            break;
        case Align::CENTER_RIGHT:
            x += b.width;
            y += b.height / 2;
            break;
        case Align::BOTTOM_LEFT:
            y += b.height;
            break;
        case Align::BOTTOM_CENTER:
            x += b.width / 2;
            y += b.height;
            break;
        case Align::BOTTOM_RIGHT:
            x += b.width;
            y += b.height;
            break;
    }
    return Vector2{ x, y };
}

constexpr std::array ALL_ALIGNS{ Align::TOP_LEFT,    Align::TOP_CENTER,
                                 Align::TOP_RIGHT,   Align::CENTER_LEFT,
                                 Align::CENTER,      Align::CENTER_RIGHT,
                                 Align::BOTTOM_LEFT, Align::BOTTOM_CENTER,
                                 Align::BOTTOM_RIGHT };

// One frame of a box 80 x 20 design units at `world`, named "box".
void frame_with_box(Vector2 world, const rmp::ui::AtOptions &o) {
    rmp::ui::begin();
    rmp::ui::at(world, o, [] { rmp::ui::column({ .width = 80, .height = 20 }, [] {}); });
    rmp::ui::end();
}

// A Clay render command with nothing but a z: what world_passthrough() reads.
Clay_RenderCommand command_at_z(int16_t z) {
    Clay_RenderCommand c{};
    c.commandType = CLAY_RENDER_COMMAND_TYPE_RECTANGLE;
    c.zIndex = z;
    return c;
}

Clay_ElementId tooltip_id(std::string_view control) {
    return rmp::ui::detail::peek_sub_id(rmp::ui::detail::peek_element_id(control, 0, 0),
                                        rmp::ui::detail::TOOLTIP_SUB);
}

// The tooltip of `control`, if the pass Clay laid out last has it.
std::optional<Clay_BoundingBox> tooltip_box(std::string_view control) {
    const Clay_ElementData data = Clay_GetElementData(tooltip_id(control));
    if (!data.found) return std::nullopt;
    return data.boundingBox;
}

bool tooltip_shown(std::string_view control) { return tooltip_box(control).has_value(); }

void sell_frame(const rmp::ui::TooltipOptions &o) {
    rmp::ui::begin();
    rmp::ui::button("Sell");
    rmp::ui::tooltip("Gives you half", o);
    rmp::ui::end();
}

Clay_Vector2 centre_of(const Clay_BoundingBox &b) {
    return Clay_Vector2{ b.x + b.width / 2, b.y + b.height / 2 };
}

bool inside(Clay_Vector2 p, const Clay_BoundingBox &b) {
    return p.x >= b.x && p.x <= b.x + b.width && p.y >= b.y && p.y <= b.y + b.height;
}

bool near(float a, float b) { return std::fabs(a - b) <= 0.5f; }

bool same_color(Color a, Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

// A scene whose _update() says something once.
class Saving : public rmp::Scene {
public:
    void _update(float /*delta*/) override {
        if (said) return;
        rmp::ui::notify("Saved");
        said = true;
    }
    void _draw() override {
        rmp::ui::begin();
        rmp::ui::button("Game");
        rmp::ui::end();
    }
    bool said = false;
};

// at() inside at(), `depth` deep, a button in the innermost.
void nest(int depth) {
    rmp::ui::at(Vector2{ 10.0f * static_cast<float>(depth), 10 }, [depth] {
        if (depth > 1) {
            nest(depth - 1);
        } else {
            rmp::ui::button("Deep");
        }
    });
}

} // namespace

TEST_SUITE("ui: floating") {
    TEST_CASE_FIXTURE(HeadlessUi,
                      "a scene's _draw() is handed the camera that drew its world") {
        rmp::scenes::detail::start(std::make_unique<Zoomed>());
        run_frame();
        CHECK(same(drawn.seen, drawn.expected));
        CHECK(drawn.seen.zoom == 2);
        // Outside the stack the world is the screen again.
        CHECK(identity(rmp::ui::detail::pass_camera()));
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "at() runs its body once, and what is in it is laid out") {
        const int errors = rmp::ui::detail::clay_error_count();
        int ran = 0;
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::begin();
            rmp::ui::at(Vector2{ 100, 200 }, [&] {
                ++ran;
                rmp::ui::button("Above");
            });
            rmp::ui::at(Vector2{ 300, 200 },
                        { .anchor = rmp::ui::Align::TOP_LEFT,
                          .offset = Vector2{ 0, -8 },
                          .id = "named" },
                        [] { rmp::ui::text("hp"); });
            rmp::ui::end();
        }
        CHECK(ran == 2);
        CHECK(has_box("Above"));
        CHECK(rmp::ui::detail::clay_error_count() == errors);
    }

    // -- at() ----------------------------------------------------------------

    TEST_CASE_FIXTURE(HeadlessUi,
                      "at() puts the point its anchor names where the camera shows the "
                      "world point") {
        const int errors = rmp::ui::detail::clay_error_count();
        struct Case {
            Camera2D camera;
            Vector2 world;
        };
        const std::array cases{
            Case{ identity_camera(), Vector2{ 300, 200 } },
            // The scene's camera of tests/camera_test.cpp: zoomed in on a point.
            Case{ Camera2D{ .offset = Vector2{ 640, 360 },
                            .target = Vector2{ 1000, 300 },
                            .rotation = 0,
                            .zoom = 2 },
                  Vector2{ 1010, 290 } },
            // Zoomed out, and moved.
            Case{ Camera2D{ .offset = Vector2{ 100, 50 },
                            .target = Vector2{ -200, 40 },
                            .rotation = 0,
                            .zoom = 0.5f },
                  Vector2{ 0, 100 } },
        };
        for (const Case &c : cases) {
            rmp::ui::detail::set_pass_camera(c.camera);
            const Vector2 expected = GetWorldToScreen2D(c.world, c.camera);
            CHECK(rmp::ui::detail::world_to_screen(c.world, c.camera).x == expected.x);
            CHECK(rmp::ui::detail::world_to_screen(c.world, c.camera).y == expected.y);
            for (const Align a : ALL_ALIGNS) {
                CAPTURE(static_cast<int>(a));
                CAPTURE(c.camera.zoom);
                frame_with_box(c.world, { .anchor = a, .id = "box" });
                const Clay_BoundingBox b = box_of("box");
                const Vector2 p = anchor_of(b, a);
                CHECK(near(p.x, expected.x));
                CHECK(near(p.y, expected.y));
                // Its size is its content's, scaled like everything else.
                CHECK(near(b.width, rmp::ui::detail::px(80)));
                CHECK(near(b.height, rmp::ui::detail::px(20)));
            }
        }
        // The default anchor is the bottom centre: the box sits above the point.
        rmp::ui::detail::set_pass_camera(identity_camera());
        frame_with_box(Vector2{ 640, 400 }, { .id = "box" });
        const Clay_BoundingBox b = box_of("box");
        CHECK(near(b.x + b.width / 2, 640));
        CHECK(near(b.y + b.height, 400));
        CHECK(rmp::ui::detail::clay_error_count() == errors);
    }

    TEST_CASE_FIXTURE(HeadlessUi, "at()'s offset is in design units, after projecting") {
        rmp::ui::set_scale(2);
        const Camera2D zoomed{
            .offset = Vector2{ 0, 0 }, .target = Vector2{ 0, 0 }, .rotation = 0, .zoom = 3
        };
        rmp::ui::detail::set_pass_camera(zoomed);
        frame_with_box(Vector2{ 100, 100 }, { .anchor = Align::TOP_LEFT, .id = "box" });
        const Clay_BoundingBox still = box_of("box");
        frame_with_box(
            Vector2{ 100, 100 },
            { .anchor = Align::TOP_LEFT, .offset = Vector2{ 10, -8 }, .id = "box" });
        const Clay_BoundingBox moved = box_of("box");
        // Projected first -- 300, 300 through a zoom of 3 -- then moved by the
        // offset at the interface's scale, not the camera's.
        CHECK(near(still.x, 300));
        CHECK(near(still.y, 300));
        CHECK(near(moved.x - still.x, 20));
        CHECK(near(moved.y - still.y, -16));
    }

    TEST_CASE_FIXTURE(
        HeadlessUi, "a box with only a growing bar in it is as wide as a touch target") {
        // The example in rmp/ui.h: a progress() fills the space available, and
        // a box that only fitted its content would have none to give it.
        rmp::ui::begin();
        rmp::ui::at(Vector2{ 400, 300 }, { .id = "bar" },
                    [] { rmp::ui::progress(0.5f, { .id = "hp" }); });
        rmp::ui::end();
        const Clay_BoundingBox hp = box_of("hp");
        CHECK(
            near(hp.width, rmp::ui::detail::px(rmp::ui::current_theme().min_touch_size)));
        CHECK(hp.height > 0);
        CHECK(near(hp.x + hp.width / 2, 400));
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a point no camera shows, or no point at all, breaks nothing") {
        const int errors = rmp::ui::detail::clay_error_count();
        const float inf = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::array far_points{
            Vector2{ 1.0e30f, -1.0e30f }, Vector2{ nan, 10 }, Vector2{ 10, nan },
            Vector2{ inf, inf },          Vector2{ -inf, 5 }, Vector2{ -5000, 300 }
        };
        for (const Vector2 &p : far_points) {
            CAPTURE(p.x);
            CAPTURE(p.y);
            const Vector2 s = rmp::ui::detail::world_to_screen(p, identity_camera());
            CHECK(std::isfinite(s.x));
            CHECK(std::isfinite(s.y));
            bool pressed = false;
            for (int frame = 0; frame < 2; ++frame) {
                rmp::ui::begin();
                rmp::ui::at(p, { .id = "far" },
                            [&] { pressed = rmp::ui::button("Far away") || pressed; });
                rmp::ui::end();
            }
            CHECK_FALSE(pressed);
            CHECK_FALSE(rmp::ui::detail::frame_open());
            // Laid out, if at all, where nothing on the screen is.
            Clay_BoundingBox b{};
            if (rmp::ui::detail::bounds_of("far", 0, 0, &b)) {
                const bool off = b.x + b.width < 0 || b.x > VIEW_W ||
                    b.y + b.height < 0 || b.y > VIEW_H;
                CHECK(off);
            }
        }
        // A camera that sees nothing at all.
        const Camera2D flat{
            .offset = Vector2{ 0, 0 }, .target = Vector2{ 0, 0 }, .rotation = 0, .zoom = 0
        };
        const Vector2 s = rmp::ui::detail::world_to_screen(Vector2{ 50, 50 }, flat);
        CHECK(std::isfinite(s.x));
        CHECK(std::isfinite(s.y));
        CHECK(rmp::ui::detail::clay_error_count() == errors);
    }

    TEST_CASE_FIXTURE(HeadlessUi, "an offset that is no number breaks nothing either") {
        const int errors = rmp::ui::detail::clay_error_count();
        const float inf = std::numeric_limits<float>::infinity();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const std::array offsets{ Vector2{ nan, 0 }, Vector2{ 0, inf },
                                  Vector2{ -inf, nan }, Vector2{ 1.0e30f, -1.0e30f } };
        for (const Vector2 &off : offsets) {
            CAPTURE(off.x);
            CAPTURE(off.y);
            for (int frame = 0; frame < 2; ++frame) {
                rmp::ui::begin();
                rmp::ui::at(Vector2{ 100, 100 }, { .offset = off, .id = "far" },
                            [] { rmp::ui::button("Far away"); });
                rmp::ui::end();
            }
            // A box at a number -- Clay culls a NaN one nowhere and draws it --
            // and where nothing on the screen is.
            Clay_BoundingBox b{};
            REQUIRE(rmp::ui::detail::bounds_of("far", 0, 0, &b));
            CHECK(std::isfinite(b.x));
            CHECK(std::isfinite(b.y));
            const bool off_screen =
                b.x + b.width < 0 || b.x > VIEW_W || b.y + b.height < 0 || b.y > VIEW_H;
            CHECK(off_screen);
        }
        CHECK(rmp::ui::detail::clay_error_count() == errors);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "what at() paints does not take the pointer, and a control in it "
                      "does") {
        auto frame = [](bool &clicked) {
            rmp::ui::begin();
            rmp::ui::at(Vector2{ 300, 300 }, { .id = "bar" }, [] {
                rmp::ui::progress(0.5f, { .width = 100, .height = 20, .id = "hp" });
            });
            rmp::ui::at(Vector2{ 900, 300 }, [&] {
                if (rmp::ui::button("Use")) clicked = true;
            });
            rmp::ui::end();
        };
        bool clicked = false;
        frame(clicked); // laid out: from here the boxes are known
        const Clay_BoundingBox hp = box_of("hp");
        finger.at = centre_of(hp);
        frame(clicked);
        frame(clicked);
        CHECK_FALSE(rmp::ui::wants_pointer());

        // Over the button in the other box: that is the interface's.
        rmp::ui::begin();
        rmp::ui::at(Vector2{ 900, 300 }, [&] { rmp::ui::button("Use"); });
        rmp::ui::end();
        finger.at = centre_of(box_of("Use"));
        frame(clicked);
        frame(clicked);
        CHECK(rmp::ui::wants_pointer());
        // ...and it is pressed where it is drawn.
        finger.down = true;
        frame(clicked);
        CHECK_FALSE(clicked);
        finger.down = false;
        frame(clicked);
        CHECK(clicked);

        // The same bar outside at() is a wall like any panel: what proves the
        // first answer was at()'s doing.
        auto plain = [] {
            rmp::ui::begin({ .placement = Align::TOP_LEFT });
            rmp::ui::progress(0.5f, { .width = 100, .height = 20, .id = "plain" });
            rmp::ui::end();
        };
        plain();
        finger.at = centre_of(box_of("plain"));
        plain();
        plain();
        CHECK(rmp::ui::wants_pointer());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a control over the world is clicked where it is drawn, even when "
                      "it is described inside a scroll area") {
        bool clicked = false;
        auto frame = [&] {
            rmp::ui::begin({ .placement = Align::TOP_LEFT });
            rmp::ui::scroll({ .width = 200, .height = 100, .id = "list" }, [&] {
                rmp::ui::text("row");
                // Far from the list: the box is not clipped by it.
                rmp::ui::at(Vector2{ 900, 500 }, [&] {
                    if (rmp::ui::button("Far")) clicked = true;
                });
                // Rows enough to put the next one below what the list shows.
                for (int row = 0; row < 8; ++row) rmp::ui::text("row");
                rmp::ui::button("Scrolled away");
            });
            rmp::ui::end();
        };
        frame();
        const Clay_BoundingBox far = box_of("Far");
        const Clay_BoundingBox list = box_of("list");
        REQUIRE_FALSE(inside(centre_of(far), list));
        finger.at = centre_of(far);
        frame();
        frame();
        CHECK(rmp::ui::wants_pointer());
        finger.down = true;
        frame();
        finger.down = false;
        frame();
        CHECK(clicked);
        // And what comes after the at() in the list is clipped by it again:
        // a row below what the list shows is not there to be hovered.
        const Clay_BoundingBox away = box_of("Scrolled away");
        REQUIRE(away.y > list.y + list.height);
        finger.at = centre_of(away);
        frame();
        frame();
        CHECK_FALSE(rmp::ui::wants_pointer());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a control over the world takes the focus like any other") {
        // Nothing else in the pass: it is the first control, so the focus is
        // its, and the keyboard shows it.
        keys.x = 1;
        for (int frame = 0; frame < 3; ++frame) {
            rmp::ui::begin();
            rmp::ui::at(Vector2{ 500, 500 }, [] { rmp::ui::button("Talk"); });
            rmp::ui::end();
        }
        CHECK(rmp::ui::focused() == "Talk");
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a box over the world is drawn under the rest of its pass, and "
                      "nothing in it takes the pointer") {
        rmp::ui::begin();
        rmp::ui::panel([] { rmp::ui::text("HUD"); });
        int16_t inner = 0;
        rmp::ui::at(Vector2{ 100, 100 }, [&] {
            // What a stack() inside it would be handed.
            inner = rmp::ui::detail::next_layer_z();
        });
        const int16_t root = rmp::ui::detail::last_at_z();
        rmp::ui::at(Vector2{ 200, 100 }, [] {});
        const int16_t second = rmp::ui::detail::last_at_z();
        const int16_t after = rmp::ui::detail::next_layer_z();
        // Under the pass's own tree, which is at 0, and the second over the
        // first: the z Clay was handed, read back.
        CHECK(root < 0);
        CHECK(second > root);
        CHECK(second < 0);
        // Everything in at() lets the pointer through; nothing else does.
        CHECK(rmp::ui::detail::world_passthrough(command_at_z(root)));
        CHECK(rmp::ui::detail::world_passthrough(command_at_z(second)));
        CHECK(rmp::ui::detail::world_passthrough(command_at_z(inner)));
        CHECK_FALSE(rmp::ui::detail::world_passthrough(command_at_z(0)));
        CHECK_FALSE(rmp::ui::detail::world_passthrough(command_at_z(after)));
        CHECK_FALSE(rmp::ui::detail::world_passthrough(command_at_z(1000)));
        rmp::ui::end();
        // A new pass starts with none.
        CHECK_FALSE(rmp::ui::detail::world_passthrough(command_at_z(root)));
        CHECK_FALSE(rmp::ui::detail::world_passthrough(command_at_z(inner)));
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "at() nested, or outside a frame, keeps the frame whole") {
        const int errors = rmp::ui::detail::clay_error_count();
        int ran = 0;
        rmp::ui::at(Vector2{ 1, 1 }, [&] { ++ran; });
        CHECK(ran == 1); // the body runs; there is nothing to put it in
        CHECK_FALSE(rmp::ui::detail::frame_open());
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::begin();
            rmp::ui::at(Vector2{ 100, 100 }, [] {
                rmp::ui::at(Vector2{ 300, 300 }, [] { rmp::ui::button("Inner"); });
                rmp::ui::text("outer");
            });
            rmp::ui::button("After");
            rmp::ui::end();
        }
        CHECK(has_box("Inner"));
        CHECK(has_box("After"));
        CHECK_FALSE(rmp::ui::detail::frame_open());
        // A frame opened inside an at() that had none to go into: the at()
        // closes nothing of it, and end() closes all of it.
        rmp::ui::at(Vector2{ 1, 1 }, [] {
            rmp::ui::begin();
            rmp::ui::button("Inside");
        });
        rmp::ui::end();
        CHECK(has_box("Inside"));
        CHECK_FALSE(rmp::ui::detail::frame_open());
        CHECK(rmp::ui::detail::clay_error_count() == errors);
    }

    TEST_CASE_FIXTURE(HeadlessUi, "at() nested past eight deep is said once, and holds") {
        rmp::detail::reset_reports_for_tests();
        const int errors = rmp::ui::detail::clay_error_count();
        for (int frame = 0; frame < 3; ++frame) {
            rmp::ui::begin();
            nest(10);
            rmp::ui::button("After");
            rmp::ui::end();
        }
        CHECK(rmp::detail::report_count() == 1);
        CHECK(has_box("Deep"));
        CHECK(has_box("After"));
        CHECK_FALSE(rmp::ui::detail::frame_open());
        CHECK(rmp::ui::detail::clay_error_count() == errors);
        // Eight deep is no mistake.
        rmp::detail::reset_reports_for_tests();
        rmp::ui::begin();
        nest(8);
        rmp::ui::end();
        CHECK(rmp::detail::report_count() == 0);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "what floats inside at() lets the pointer through, however many "
                      "layers come between") {
        rmp::ui::begin();
        std::vector<int16_t> inside;
        for (int i = 0; i < 20; ++i) {
            rmp::ui::at(Vector2{ 30.0f * static_cast<float>(i), 50 },
                        [&] { inside.push_back(rmp::ui::detail::next_layer_z()); });
            // A layer of the pass's own between two boxes: the z inside them
            // stop following on, and are kept apart.
            rmp::ui::stack([] { rmp::ui::layer([] {}); });
        }
        const int16_t after = rmp::ui::detail::next_layer_z();
        for (const int16_t z : inside) {
            CAPTURE(z);
            CHECK(rmp::ui::detail::world_passthrough(command_at_z(z)));
        }
        // The first layers of the pass's own are its own.
        CHECK_FALSE(rmp::ui::detail::world_passthrough(command_at_z(inside[0] + 2)));
        CHECK_FALSE(rmp::ui::detail::world_passthrough(command_at_z(after)));
        rmp::ui::end();
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a pass that never reached its end() leaves the next pass's at() "
                      "nothing of it") {
        // Begun, and dropped without an end(): pass_ended() -- where at()
        // forgets what it handed out -- never ran for it.
        const int errors = rmp::ui::detail::clay_error_count();
        rmp::ui::begin();
        std::vector<int16_t> stale;
        for (int i = 0; i < 3; ++i) {
            rmp::ui::at(Vector2{ 100, 100 },
                        [&] { stale.push_back(rmp::ui::detail::next_layer_z()); });
            rmp::ui::stack([] { rmp::ui::layer([] {}); });
        }
        rmp::ui::detail::set_frame_open(false);
        // The next pass. Its first at() starts from nothing: the z the last
        // pass handed out inside its boxes are anybody's again, but for what
        // this box takes itself -- 1 and 2, the first z of any pass.
        rmp::ui::begin();
        rmp::ui::at(Vector2{ 100, 100 }, [] {});
        REQUIRE(stale.size() == 3);
        for (const int16_t z : stale) {
            CAPTURE(z);
            if (z > 2) CHECK_FALSE(rmp::ui::detail::world_passthrough(command_at_z(z)));
        }
        rmp::ui::end();
        CHECK_FALSE(rmp::ui::detail::frame_open());
        CHECK(rmp::ui::detail::clay_error_count() == errors);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a pass closed without being drawn leaves the next pass nothing "
                      "of its at()s") {
        // What the runner does to a frame the game left open, after
        // EndDrawing(): laid out, and not drawn. What at() handed out goes
        // with it all the same -- the next pass's first layer is not one of
        // its at()'s, so nothing else would forget them.
        const int errors = rmp::ui::detail::clay_error_count();
        rmp::ui::begin();
        for (int i = 0; i < 3; ++i) {
            rmp::ui::at(Vector2{ 100, 100 },
                        [] { (void)rmp::ui::detail::next_layer_z(); });
        }
        rmp::ui::detail::close_frame(false);
        CHECK_FALSE(rmp::ui::detail::frame_open());
        rmp::ui::begin();
        std::array<int16_t, 6> own{};
        for (int16_t &z : own) z = rmp::ui::detail::next_layer_z();
        for (const int16_t z : own) {
            CAPTURE(z);
            CHECK_FALSE(rmp::ui::detail::world_passthrough(command_at_z(z)));
        }
        rmp::ui::end();
        CHECK(rmp::ui::detail::clay_error_count() == errors);
    }

    // -- tooltip() -----------------------------------------------------------

    TEST_CASE_FIXTURE(HeadlessUi, "the control declared last is the one taken, once") {
        rmp::ui::begin();
        CHECK(rmp::ui::detail::take_last_control() == 0);
        rmp::ui::text("not a control");
        CHECK(rmp::ui::detail::take_last_control() == 0);
        rmp::ui::button("One");
        bool on = false;
        rmp::ui::checkbox("Two", on);
        const uint32_t two = rmp::ui::detail::take_last_control();
        CHECK(two != 0);
        CHECK(two == rmp::ui::detail::peek_element_id("Two").id);
        CHECK(rmp::ui::detail::take_last_control() == 0);
        rmp::ui::button("Three");
        rmp::ui::end();
        // A new pass starts with none.
        rmp::ui::begin();
        CHECK(rmp::ui::detail::take_last_control() == 0);
        rmp::ui::end();
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "tooltips and notices keep the frame they are in whole") {
        const int errors = rmp::ui::detail::clay_error_count();
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::begin();
            rmp::ui::button("Sell");
            rmp::ui::tooltip("Gives you half");
            rmp::ui::tooltip("Twice, for no control",
                             { .side = rmp::ui::Align::BOTTOM_CENTER, .delay = 0 });
            rmp::ui::notify("Sold");
            rmp::ui::notify("Loud",
                            { .seconds = 1, .color = rmp::ui::ColorRole::DANGER });
            rmp::ui::button("After");
            rmp::ui::end();
        }
        CHECK(has_box("After"));
        CHECK_FALSE(rmp::ui::detail::frame_open());
        CHECK(rmp::ui::detail::clay_error_count() == errors);
        // Outside a frame they go nowhere and break nothing.
        rmp::ui::tooltip("outside");
        CHECK_FALSE(rmp::ui::detail::frame_open());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a tooltip waits for the pointer to rest on its control") {
        rmp::ui::detail::set_test_frame_time(0.125f); // exact in binary
        sell_frame({}); // laid out
        finger.at = centre_of(box_of("Sell"));
        // The pointer arrives on the second frame; half a second later -- four
        // frames of 0.125 -- the tooltip shows, and not a frame sooner.
        int shown_at = 0;
        for (int frame = 2; frame <= 8 && shown_at == 0; ++frame) {
            sell_frame({});
            if (tooltip_shown("Sell")) shown_at = frame;
        }
        CHECK(shown_at == 6);

        // Above the control, centred, clear of it.
        sell_frame({});
        const std::optional<Clay_BoundingBox> shown = tooltip_box("Sell");
        REQUIRE(shown.has_value());
        const Clay_BoundingBox tip = shown.value_or(Clay_BoundingBox{});
        const Clay_BoundingBox sell = box_of("Sell");
        CHECK(near(tip.x + tip.width / 2, sell.x + sell.width / 2));
        CHECK(tip.y + tip.height < sell.y);
        CHECK(tip.width > 0);

        // The pointer leaves: gone at once. Back: the wait starts again.
        finger.at = Clay_Vector2{ 2, 2 };
        sell_frame({});
        CHECK_FALSE(tooltip_shown("Sell"));
        finger.at = centre_of(sell);
        sell_frame({});
        CHECK_FALSE(tooltip_shown("Sell"));

        // With no delay, on the first frame the pointer is over it.
        sell_frame({ .delay = 0 });
        CHECK(tooltip_shown("Sell"));
        // A delay that is not a number is no delay, rather than never.
        finger.at = Clay_Vector2{ 2, 2 };
        sell_frame({});
        finger.at = centre_of(sell);
        sell_frame({ .delay = std::numeric_limits<float>::quiet_NaN() });
        CHECK(tooltip_shown("Sell"));
    }

    TEST_CASE_FIXTURE(
        HeadlessUi,
        "a frame without the tooltip, or without its control, starts the wait "
        "again") {
        rmp::ui::detail::set_test_frame_time(0.125f);
        sell_frame({});
        finger.at = centre_of(box_of("Sell"));
        for (int frame = 0; frame < 3; ++frame) sell_frame({}); // 0.25 s of the 0.5
        // The control, and no tooltip asked for: the pointer stayed where it
        // was, but nothing was waiting on it.
        rmp::ui::begin();
        rmp::ui::button("Sell");
        rmp::ui::end();
        sell_frame({});
        sell_frame({});
        CHECK_FALSE(tooltip_shown("Sell")); // 0.125 s since it was asked again, not 0.5
        for (int frame = 0; frame < 3; ++frame) sell_frame({});
        CHECK(tooltip_shown("Sell")); // 0.5 s: the whole wait, from the start

        // A frame without the control at all.
        finger.at = Clay_Vector2{ 2, 2 };
        sell_frame({});
        finger.at = centre_of(box_of("Sell"));
        for (int frame = 0; frame < 3; ++frame) sell_frame({});
        rmp::ui::begin();
        rmp::ui::button("Elsewhere", { .id = "elsewhere" });
        rmp::ui::end();
        sell_frame({});
        sell_frame({});
        CHECK_FALSE(tooltip_shown("Sell"));
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a tooltip shows at once on a control the keyboard has reached") {
        keys.x = 1; // a key touched: the focus is shown, and does not move
        sell_frame({});
        sell_frame({});
        CHECK(rmp::ui::focused() == "Sell");
        CHECK(tooltip_shown("Sell"));
        // The pointer is nowhere near it, and that is the point.
        CHECK(finger.at.x < 0);

        // A focus nobody can see is not reason enough.
        keys.x = 0;
        rmp::ui::detail::set_focus_visible(false);
        sell_frame({});
        CHECK_FALSE(tooltip_shown("Sell"));

        // Nor one the game turned navigation off for: the ring is not drawn.
        keys.x = 1;
        sell_frame({});
        sell_frame({});
        REQUIRE(tooltip_shown("Sell"));
        rmp::ui::set_navigation_enabled(false);
        sell_frame({});
        rmp::ui::set_navigation_enabled(true);
        CHECK_FALSE(tooltip_shown("Sell"));
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a tooltip that waits forever shows by the focus alone, and an "
                      "empty one not at all") {
        rmp::ui::detail::set_test_frame_time(0.125f);
        const float forever = std::numeric_limits<float>::infinity();
        sell_frame({ .delay = forever });
        finger.at = centre_of(box_of("Sell"));
        for (int frame = 0; frame < 40; ++frame) sell_frame({ .delay = forever }); // 5 s
        CHECK_FALSE(tooltip_shown("Sell"));
        keys.x = 1; // a key touched: the focus shows
        sell_frame({ .delay = forever });
        sell_frame({ .delay = forever });
        CHECK(tooltip_shown("Sell"));

        // tooltip(item.description) for an item that has none: nothing shown,
        // by the pointer or by the focus, and no mistake.
        rmp::detail::reset_reports_for_tests();
        auto empty_frame = [] {
            rmp::ui::begin();
            rmp::ui::button("Sell");
            rmp::ui::tooltip("", { .delay = 0 });
            rmp::ui::end();
        };
        empty_frame();
        empty_frame();
        CHECK_FALSE(tooltip_shown("Sell"));
        CHECK(rmp::detail::report_count() == 0);
        // It was about the button all the same: one after it is about nothing.
        rmp::ui::begin();
        rmp::ui::button("Sell");
        rmp::ui::tooltip("");
        rmp::ui::tooltip("Twice");
        rmp::ui::end();
        CHECK(rmp::detail::report_count() == 1);
    }

    TEST_CASE_FIXTURE(
        HeadlessUi, "a tooltip does not show by the focus in a pass input cannot reach") {
        keys.x = 1; // a key touched: the focus is shown
        auto frame = [](bool reachable) {
            rmp::ui::detail::begin_frame();
            rmp::ui::detail::set_pass_input(reachable);
            rmp::ui::begin();
            rmp::ui::button("Sell");
            rmp::ui::tooltip("Gives you half");
            rmp::ui::end();
            rmp::ui::detail::set_pass_input(true);
            rmp::ui::detail::end_frame();
        };
        frame(true);
        frame(true);
        REQUIRE(rmp::ui::focused() == "Sell");
        REQUIRE(tooltip_shown("Sell"));
        // Under an overlay now, or a scene the app says input does not reach:
        // no ring is drawn there, and no line about it either -- from the
        // first frame, while the focus is still where it was.
        frame(false);
        CHECK(rmp::ui::focused() == "Sell");
        CHECK_FALSE(tooltip_shown("Sell"));
    }

    TEST_CASE_FIXTURE(HeadlessUi, "a long tooltip wraps at its width") {
        std::string line;
        for (int i = 0; i < 40; ++i) line += "word ";
        auto frame = [&] {
            rmp::ui::begin();
            rmp::ui::button("Sell");
            rmp::ui::tooltip(line, { .delay = 0 });
            rmp::ui::end();
        };
        frame();
        finger.at = centre_of(box_of("Sell"));
        frame();
        const std::optional<Clay_BoundingBox> shown = tooltip_box("Sell");
        REQUIRE(shown.has_value());
        const Clay_BoundingBox tip = shown.value_or(Clay_BoundingBox{});
        CHECK(tip.width <= rmp::ui::detail::px(320) + 0.5f);
        // Several lines of it, then.
        CHECK(tip.height >
              2 * rmp::ui::detail::px(rmp::ui::current_theme().font_size_small));
    }

    TEST_CASE_FIXTURE(HeadlessUi, "a tooltip keeps a copy of its text") {
        sell_frame({});
        finger.at = centre_of(box_of("Sell"));
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::begin();
            rmp::ui::button("Sell");
            {
                // Gone before end() lays it out: under ASan, a read of it
                // would stop the test here.
                std::string line = "Gives you half";
                rmp::ui::tooltip(line, { .delay = 0 });
                line.assign(line.size(), '#');
            }
            rmp::ui::end();
        }
        const std::optional<Clay_BoundingBox> shown = tooltip_box("Sell");
        REQUIRE(shown.has_value());
        CHECK(shown.value_or(Clay_BoundingBox{}).width > 0);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a tooltip with no control before it is reported once") {
        rmp::detail::reset_reports_for_tests();
        for (int frame = 0; frame < 60; ++frame) {
            rmp::ui::begin();
            rmp::ui::text("Not a control");
            rmp::ui::tooltip("About nothing");
            rmp::ui::end();
        }
        CHECK(rmp::detail::report_count() == 1);
        CHECK_FALSE(rmp::ui::detail::frame_open());
        // One control, one tooltip: the second has nothing left to be about.
        rmp::detail::reset_reports_for_tests();
        rmp::ui::begin();
        rmp::ui::button("Sell");
        rmp::ui::tooltip("One");
        rmp::ui::tooltip("Two");
        rmp::ui::end();
        CHECK(rmp::detail::report_count() == 1);
        // A tooltip right after a control is no mistake.
        rmp::detail::reset_reports_for_tests();
        sell_frame({});
        CHECK(rmp::detail::report_count() == 0);
        // A line that changes every frame is the same mistake every frame:
        // one report, not one per line.
        rmp::detail::reset_reports_for_tests();
        for (int frame = 0; frame < 60; ++frame) {
            rmp::ui::begin();
            rmp::ui::text("Not a control");
            rmp::ui::tooltip("Ready in " + std::to_string(60 - frame) + " frames");
            rmp::ui::end();
        }
        CHECK(rmp::detail::report_count() == 1);
    }

    TEST_CASE_FIXTURE(
        HeadlessUi, "a tooltip after a control that declared nothing is about nothing") {
        // A slider with no range and a dropdown with no items are no control:
        // the tooltip after each is said, and not worn by the button before
        // it -- which has the focus, shown, so a tooltip of its would show.
        float volume = 0.5f;
        int quality = 0;
        const std::vector<std::string_view> none;
        keys.x = 1; // a key touched: the focus is shown, and does not move
        for (int which = 0; which < 2; ++which) {
            CAPTURE(which);
            rmp::detail::reset_reports_for_tests();
            for (int frame = 0; frame < 2; ++frame) {
                rmp::ui::begin();
                rmp::ui::button("Sell");
                if (which == 0) {
                    CHECK_FALSE(rmp::ui::slider("Volume", volume, 1, 1));
                } else {
                    CHECK_FALSE(rmp::ui::dropdown("Quality", quality, none));
                }
                rmp::ui::tooltip("About the control that is not there");
                rmp::ui::end();
            }
            CHECK(rmp::ui::focused() == "Sell");
            CHECK_FALSE(tooltip_shown("Sell"));
            CHECK(rmp::detail::report_count() == 1);
        }
    }

    TEST_CASE_FIXTURE(HeadlessUi, "a tooltip goes on the side it is asked for") {
        // Each side, where Clay put it, against where place_tooltip() says.
        rmp::ui::detail::set_test_frame_time(0.125f);
        for (const Align side : ALL_ALIGNS) {
            CAPTURE(static_cast<int>(side));
            sell_frame({ .side = side, .delay = 0 });
            // After a frame: the scale is this viewport's, not the last test's.
            const float gap = rmp::ui::detail::px(rmp::ui::current_theme().gap / 2);
            finger.at = centre_of(box_of("Sell"));
            sell_frame({ .side = side, .delay = 0 });
            const std::optional<Clay_BoundingBox> shown = tooltip_box("Sell");
            REQUIRE(shown.has_value());
            const Clay_BoundingBox tip = shown.value_or(Clay_BoundingBox{});
            const Clay_BoundingBox c = box_of("Sell");
            switch (side) {
                case Align::TOP_LEFT:
                    CHECK(near(tip.x, c.x));
                    CHECK(near(tip.y + tip.height, c.y - gap));
                    break;
                case Align::TOP_CENTER:
                    CHECK(near(tip.x + tip.width / 2, c.x + c.width / 2));
                    CHECK(near(tip.y + tip.height, c.y - gap));
                    break;
                case Align::TOP_RIGHT:
                    CHECK(near(tip.x + tip.width, c.x + c.width));
                    CHECK(near(tip.y + tip.height, c.y - gap));
                    break;
                case Align::CENTER_LEFT:
                    CHECK(near(tip.x + tip.width, c.x - gap));
                    CHECK(near(tip.y + tip.height / 2, c.y + c.height / 2));
                    break;
                case Align::CENTER:
                    CHECK(near(tip.x + tip.width / 2, c.x + c.width / 2));
                    CHECK(near(tip.y + tip.height / 2, c.y + c.height / 2));
                    break;
                case Align::CENTER_RIGHT:
                    CHECK(near(tip.x, c.x + c.width + gap));
                    CHECK(near(tip.y + tip.height / 2, c.y + c.height / 2));
                    break;
                case Align::BOTTOM_LEFT:
                    CHECK(near(tip.x, c.x));
                    CHECK(near(tip.y, c.y + c.height + gap));
                    break;
                case Align::BOTTOM_CENTER:
                    CHECK(near(tip.x + tip.width / 2, c.x + c.width / 2));
                    CHECK(near(tip.y, c.y + c.height + gap));
                    break;
                case Align::BOTTOM_RIGHT:
                    CHECK(near(tip.x + tip.width, c.x + c.width));
                    CHECK(near(tip.y, c.y + c.height + gap));
                    break;
            }
            finger.at = Clay_Vector2{ -1, -1 };
        }
    }

    TEST_CASE_FIXTURE(HeadlessUi, "a tooltip at the edge of the screen stays on it") {
        // A button against the top edge, its tooltip asked for above it.
        auto top_frame = [] {
            rmp::ui::begin({ .placement = Align::TOP_LEFT, .padding = 0 });
            rmp::ui::button("Sell");
            rmp::ui::tooltip("A rather long line about selling things",
                             { .side = Align::TOP_CENTER, .delay = 0 });
            rmp::ui::end();
        };
        top_frame();
        finger.at = centre_of(box_of("Sell"));
        top_frame(); // shown where it was asked for, its size unknown
        top_frame(); // its size known: put where it fits
        const std::optional<Clay_BoundingBox> shown = tooltip_box("Sell");
        REQUIRE(shown.has_value());
        const Clay_BoundingBox tip = shown.value_or(Clay_BoundingBox{});
        const Clay_BoundingBox sell = box_of("Sell");
        CHECK(tip.y >= sell.y + sell.height); // below, the side that fits
        CHECK(tip.x >= -0.5f); // and not past the left edge either
        CHECK(tip.x + tip.width <= VIEW_W + 0.5f);
    }

    TEST_CASE("place_tooltip(): the side asked for when it fits, the other when not") {
        using rmp::ui::detail::place_tooltip;
        const Vector2 screen{ 1000, 500 };
        const Vector2 size{ 200, 40 };
        const float gap = 6;
        const Rectangle middle{ 400, 200, 100, 50 };
        for (const Align side : ALL_ALIGNS) {
            CAPTURE(static_cast<int>(side));
            const rmp::ui::detail::TipPlace p =
                place_tooltip(middle, size, side, screen, gap);
            CHECK(p.side == side);
            CHECK(p.shift.x == 0);
            CHECK(p.shift.y == 0);
        }
        // Against the top: above does not fit, below does.
        const Rectangle top{ 400, 10, 100, 50 };
        CHECK(place_tooltip(top, size, Align::TOP_CENTER, screen, gap).side ==
              Align::BOTTOM_CENTER);
        CHECK(place_tooltip(top, size, Align::TOP_LEFT, screen, gap).side ==
              Align::BOTTOM_LEFT);
        CHECK(place_tooltip(top, size, Align::TOP_RIGHT, screen, gap).side ==
              Align::BOTTOM_RIGHT);
        // Against the bottom, the other way.
        const Rectangle bottom{ 400, 460, 100, 30 };
        CHECK(place_tooltip(bottom, size, Align::BOTTOM_CENTER, screen, gap).side ==
              Align::TOP_CENTER);
        CHECK(place_tooltip(bottom, size, Align::BOTTOM_LEFT, screen, gap).side ==
              Align::TOP_LEFT);
        CHECK(place_tooltip(bottom, size, Align::BOTTOM_RIGHT, screen, gap).side ==
              Align::TOP_RIGHT);
        // Beside: against the left edge it goes right, and the other way.
        const Rectangle left{ 5, 200, 100, 50 };
        CHECK(place_tooltip(left, size, Align::CENTER_LEFT, screen, gap).side ==
              Align::CENTER_RIGHT);
        const Rectangle right{ 900, 200, 95, 50 };
        CHECK(place_tooltip(right, size, Align::CENTER_RIGHT, screen, gap).side ==
              Align::CENTER_LEFT);
        // Neither side fits: the one asked for.
        const Vector2 tall{ 200, 300 };
        CHECK(place_tooltip(middle, tall, Align::TOP_CENTER, screen, gap).side ==
              Align::TOP_CENTER);

        // Along the side: moved just enough to be on the screen.
        const Rectangle corner{ 0, 200, 40, 40 };
        const rmp::ui::detail::TipPlace in =
            place_tooltip(corner, size, Align::TOP_CENTER, screen, gap);
        // Centred on the control it would start at 20 - 100 = -80.
        CHECK(in.shift.x == 80);
        CHECK(in.shift.y == 0);
        const Rectangle far_right{ 980, 200, 20, 40 };
        CHECK(place_tooltip(far_right, size, Align::BOTTOM_LEFT, screen, gap).shift.x ==
              -180);
        const Rectangle low{ 400, 480, 100, 20 };
        // Beside a control at the very bottom: up, by what hangs below.
        CHECK(place_tooltip(low, size, Align::CENTER_RIGHT, screen, gap).shift.y == -10);
        // Wider than the screen: against its left edge.
        const Vector2 wide{ 1200, 20 };
        CHECK(place_tooltip(middle, wide, Align::TOP_CENTER, screen, gap).shift.x == 150);
        // On top of the control, both ways.
        const rmp::ui::detail::TipPlace over =
            place_tooltip(Rectangle{ 0, 0, 10, 10 }, size, Align::CENTER, screen, gap);
        CHECK(over.side == Align::CENTER);
        CHECK(over.shift.x == 95);
        CHECK(over.shift.y == 15);
    }

    // -- notify() ------------------------------------------------------------

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a notice, or a report on screen, does not start the UI") {
        rmp::ui::shutdown();
        REQUIRE_FALSE(rmp::ui::detail::started());
        rmp::ui::notify("Saved");
        rmp::ui::detail::show_report("UI: a mistake");
        CHECK_FALSE(rmp::ui::detail::started());
        CHECK(rmp::ui::detail::notice_count() == 2);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "an empty queue starts nothing; a queued notice is drawn") {
        rmp::ui::shutdown();
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::draw_notices();
        rmp::ui::detail::end_frame();
        CHECK_FALSE(rmp::ui::detail::started());

        rmp::ui::notify("Saved");
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::draw_notices();
        rmp::ui::detail::end_frame();
        CHECK(rmp::ui::detail::started());
        REQUIRE(rmp::ui::detail::notice_count() == 1);
        CHECK(rmp::ui::detail::notice_at(0).box.width > 0);
        CHECK(rmp::ui::detail::notice_layout_errors() == 0);
    }

    TEST_CASE_FIXTURE(HeadlessUi, "drawing the notices leaves no frame open") {
        rmp::ui::detail::begin_frame();
        rmp::ui::begin();
        rmp::ui::button("Game");
        rmp::ui::end();
        rmp::ui::detail::draw_notices();
        CHECK_FALSE(rmp::ui::detail::frame_open());
        rmp::ui::detail::end_frame();
    }

    TEST_CASE_FIXTURE(
        HeadlessUi,
        "notices queued from _update() are drawn at the bottom centre, after "
        "every scene") {
        rmp::scenes::detail::start(std::make_unique<Saving>());
        // The scene's own passes do not draw them: with a scene stack, the
        // app does, after the last scene.
        run_frame();
        REQUIRE(rmp::ui::detail::notice_count() == 1);
        CHECK(rmp::ui::detail::notice_at(0).box.width == 0);
        run_app_frame();
        const rmp::ui::detail::NoticeView n = rmp::ui::detail::notice_at(0);
        CHECK(n.text == "Saved");
        CHECK(n.color == rmp::ui::ColorRole::TEXT);
        CHECK(n.alpha == 1);
        REQUIRE(n.box.width > 0);
        CHECK(near(n.box.x + n.box.width / 2, VIEW_W / 2));
        const float inset = rmp::ui::detail::px(rmp::ui::current_theme().panel_padding) +
            rmp::ui::detail::safe_area_inset();
        CHECK(near(n.box.y + n.box.height, VIEW_H - inset));
    }

    TEST_CASE_FIXTURE(HeadlessUi, "the notices leave the game's interface alone") {
        rmp::ui::notify("Saved");
        rmp::ui::detail::begin_frame();
        rmp::ui::begin();
        rmp::ui::button("Game");
        rmp::ui::end();
        const Clay_Context *const before = Clay_GetCurrentContext();
        const int pass = rmp::ui::detail::current_pass();
        rmp::ui::detail::draw_notices();
        // No pass of the game's taken, and Clay still holds the game's layout:
        // the notices have one of their own. A pass after the game's would
        // have told Clay the pointer was nowhere, and the wheel would not
        // scroll the game's lists for as long as a notice showed.
        CHECK(rmp::ui::detail::current_pass() == pass);
        CHECK(Clay_GetCurrentContext() == before);
        CHECK(has_box("Game"));
        rmp::ui::detail::end_frame();
        CHECK(rmp::ui::detail::notice_at(0).box.width > 0);
    }

    TEST_CASE_FIXTURE(HeadlessUi, "a notice is in the colour it was given") {
        const rmp::ui::Theme &t = rmp::ui::current_theme();
        struct Paint {
            rmp::ui::ColorRole role;
            Color expected;
        };
        const std::array paints{ Paint{ rmp::ui::ColorRole::TEXT, t.text },
                                 Paint{ rmp::ui::ColorRole::MUTED, t.text_muted },
                                 Paint{ rmp::ui::ColorRole::PRIMARY, t.primary },
                                 Paint{ rmp::ui::ColorRole::DANGER, t.danger } };
        // Four colours a test can tell apart, or it proves nothing.
        for (size_t a = 0; a < paints.size(); ++a) {
            for (size_t b = a + 1; b < paints.size(); ++b) {
                REQUIRE_FALSE(same_color(paints[a].expected, paints[b].expected));
            }
        }
        for (const Paint &p : paints) rmp::ui::notify("x", { .color = p.role });
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::draw_notices();
        rmp::ui::detail::end_frame();
        int i = 0;
        for (const Paint &p : paints) {
            CAPTURE(i);
            const rmp::ui::detail::NoticeView n = rmp::ui::detail::notice_at(i);
            CHECK(n.color == p.role);
            CHECK(n.box.width > 0);
            // Painted in the theme's colour for it, on the theme's panel.
            CHECK(same_color(n.ink, p.expected));
            CHECK(same_color(n.fill, t.panel));
            ++i;
        }
        // Past either end of the queue there is nothing.
        CHECK(rmp::ui::detail::notice_at(-1).text.empty());
        CHECK(rmp::ui::detail::notice_at(4).text.empty());
        CHECK(rmp::ui::detail::notice_at(4).alpha == 0);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "the newest eight are kept, oldest first, newest lowest") {
        // Each one longer than the one before, so its box says which it is.
        auto line = [](int i) { return std::string(static_cast<size_t>(i + 1), 'a'); };
        for (int i = 0; i < 10; ++i) rmp::ui::notify(line(i));
        REQUIRE(rmp::ui::detail::notice_count() == rmp::ui::detail::NOTICE_RING);
        for (int i = 0; i < rmp::ui::detail::NOTICE_RING; ++i) {
            CHECK(rmp::ui::detail::notice_at(i).text == line(i + 2));
        }
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::draw_notices();
        rmp::ui::detail::end_frame();
        for (int i = 1; i < rmp::ui::detail::NOTICE_RING; ++i) {
            CAPTURE(i);
            const Rectangle above = rmp::ui::detail::notice_at(i - 1).box;
            const Rectangle below = rmp::ui::detail::notice_at(i).box;
            CHECK(above.y + above.height <= below.y);
            // The longer, newer line in the lower, wider box.
            CHECK(above.width < below.width);
        }
        CHECK(rmp::ui::detail::notice_layout_errors() == 0);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a notice stays its seconds, fading over the last of them") {
        rmp::ui::detail::set_test_frame_time(0.125f);
        rmp::ui::notify("Bye", { .seconds = 1 });
        const rmp::ui::Theme &t = rmp::ui::current_theme();
        // It ages by the frames drawn: after each, what the next one shows.
        const std::array<float, 7> alpha{ 1, 1, 1, 1, 0.75f, 0.5f, 0.25f };
        // And what each one painted: the first at once, unfaded, and every
        // one of its 1 s of frames -- eight of 0.125 -- the last at a quarter.
        const std::array<float, 8> painted{ 1, 1, 1, 1, 1, 0.75f, 0.5f, 0.25f };
        auto ink_alpha = [&](float a) {
            return static_cast<unsigned char>(std::lround(t.text.a * a));
        };
        auto fill_alpha = [&](float a) {
            return static_cast<unsigned char>(std::lround(t.panel.a * a));
        };
        for (size_t frame = 0; frame < painted.size(); ++frame) {
            CAPTURE(frame);
            REQUIRE(rmp::ui::detail::notice_count() == 1);
            rmp::ui::detail::begin_frame();
            rmp::ui::detail::draw_notices();
            rmp::ui::detail::end_frame();
            if (frame < alpha.size()) {
                REQUIRE(rmp::ui::detail::notice_count() == 1);
                const rmp::ui::detail::NoticeView n = rmp::ui::detail::notice_at(0);
                CHECK(n.alpha == doctest::Approx(alpha[frame]));
                CHECK(n.ink.a == ink_alpha(painted[frame]));
                CHECK(n.fill.a == fill_alpha(painted[frame]));
            }
        }
        CHECK(rmp::ui::detail::notice_count() == 0);

        // A short one fades over its second half: 0.25 s of its 0.5.
        rmp::ui::notify("Blink", { .seconds = 0.5f });
        for (const float expected : { 1.0f, 1.0f, 0.5f }) {
            rmp::ui::detail::begin_frame();
            rmp::ui::detail::draw_notices();
            rmp::ui::detail::end_frame();
            REQUIRE(rmp::ui::detail::notice_count() == 1);
            CHECK(rmp::ui::detail::notice_at(0).alpha == doctest::Approx(expected));
        }
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "the notices are drawn at the window's scale with no interface of "
                      "the game's to set it") {
        // A game that never calls begin(): its warnings on screen in a debug
        // build are all the interface it has, and the first of them starts
        // the UI after the frame began -- too late for the frame to have set
        // the scale. Once in a window of the design size, once in one twice
        // as big.
        auto first_notice_height = [](float width, float height) {
            rmp::ui::shutdown();
            rmp::ui::detail::forget_notices();
            rmp::ui::detail::set_test_viewport(width, height);
            rmp::ui::notify("Saved");
            rmp::ui::detail::begin_frame();
            REQUIRE_FALSE(rmp::ui::detail::started());
            rmp::ui::detail::draw_notices();
            rmp::ui::detail::end_frame();
            return rmp::ui::detail::notice_at(0).box.height;
        };
        const float design = first_notice_height(VIEW_W, VIEW_H);
        const float twice = first_notice_height(2 * VIEW_W, 2 * VIEW_H);
        REQUIRE(design > 0);
        CHECK(twice > 1.5f * design);
    }

    TEST_CASE_FIXTURE(HeadlessUi, "a notice takes neither the pointer nor the focus") {
        rmp::ui::notify("Saved");
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::draw_notices();
        rmp::ui::detail::end_frame();
        const Rectangle b = rmp::ui::detail::notice_at(0).box;
        REQUIRE(b.width > 0);
        finger.at = Clay_Vector2{ b.x + b.width / 2, b.y + b.height / 2 };
        for (int frame = 0; frame < 3; ++frame) {
            rmp::ui::detail::begin_frame();
            rmp::ui::detail::draw_notices();
            rmp::ui::detail::end_frame();
        }
        CHECK_FALSE(rmp::ui::wants_pointer());
        CHECK_FALSE(rmp::ui::wants_keyboard());
        CHECK(rmp::ui::focused().empty());
    }

    TEST_CASE_FIXTURE(
        HeadlessUi,
        "without scenes, the notices are drawn once a frame, after its first "
        "pass") {
        rmp::ui::detail::set_test_frame_time(0.125f);
        rmp::ui::notify("Once", { .seconds = 1 });
        // Two passes a frame, as a screen and its overlay(): aged once.
        auto two_passes = [] {
            rmp::ui::detail::begin_frame();
            rmp::ui::begin();
            rmp::ui::button("Screen");
            rmp::ui::end();
            rmp::ui::begin();
            rmp::ui::button("Over it");
            rmp::ui::end();
            rmp::ui::detail::end_frame();
        };
        for (int frame = 0; frame < 7; ++frame) two_passes();
        REQUIRE(rmp::ui::detail::notice_count() == 1);
        CHECK(rmp::ui::detail::notice_at(0).box.width > 0);
        two_passes();
        CHECK(rmp::ui::detail::notice_count() == 0);

        // And a plain raylib loop, where every begin() is a frame of its own.
        rmp::ui::notify("Plain", { .seconds = 1 });
        for (int frame = 0; frame < 7; ++frame) {
            rmp::ui::begin();
            rmp::ui::end();
        }
        CHECK(rmp::ui::detail::notice_count() == 1);
        rmp::ui::begin();
        rmp::ui::end();
        CHECK(rmp::ui::detail::notice_count() == 0);
    }

    TEST_CASE_FIXTURE(HeadlessUi, "a notice keeps a copy, and not too long a one") {
        {
            std::string temporary = "Achievement unlocked";
            rmp::ui::notify(temporary);
            temporary.assign(temporary.size(), 'x');
        }
        CHECK(rmp::ui::detail::notice_at(0).text == "Achievement unlocked");
        // A kilobyte, as a report line can be: cut, and never in the middle of
        // a character.
        std::string long_line;
        for (int i = 0; i < 400; ++i) long_line += "\xC3\xA9"; // e acute, two bytes
        rmp::ui::notify(long_line);
        const std::string_view kept = rmp::ui::detail::notice_at(1).text;
        CHECK(rmp::ui::detail::utf8_length(kept) == rmp::ui::detail::NOTICE_MAX_CHARS);
        CHECK(kept.size() == 2 * rmp::ui::detail::NOTICE_MAX_CHARS);
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::draw_notices();
        rmp::ui::detail::end_frame();
        CHECK(rmp::ui::detail::notice_layout_errors() == 0);
    }

    TEST_CASE_FIXTURE(HeadlessUi, "a full ring of the longest notices still lays out") {
        // The most words the ring can hold: every one a letter and a space.
        std::string words;
        while (rmp::ui::detail::utf8_length(words) < rmp::ui::detail::NOTICE_MAX_CHARS) {
            words += "a ";
        }
        for (int i = 0; i < rmp::ui::detail::NOTICE_RING; ++i) {
            // Each its own: Clay measures a string it has seen once.
            rmp::ui::notify(std::to_string(i) + " " + words);
        }
        for (int frame = 0; frame < 3; ++frame) {
            rmp::ui::detail::begin_frame();
            rmp::ui::detail::draw_notices();
            rmp::ui::detail::end_frame();
        }
        CHECK(rmp::ui::detail::notice_layout_errors() == 0);
        // Each one wrapped to the width a notice has, and all of them on the
        // screen.
        for (int i = 0; i < rmp::ui::detail::NOTICE_RING; ++i) {
            const Rectangle b = rmp::ui::detail::notice_at(i).box;
            CHECK(b.width > 0);
            CHECK(b.width <= rmp::ui::detail::px(560) + 0.5f);
        }
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a thousand different notices, one after another, "
                      "are each measured and drawn") {
        // What a long session says: a line, gone before the next. Clay lets
        // go of a line it measured only when another lands in the same one of
        // its buckets, and the notices' context visits a bucket or two a
        // frame -- it was full after about 150 lines, and from then on, every
        // so often, a new one came out as nothing: a box with no text in it
        // (67 of these 1000).
        rmp::ui::detail::set_test_frame_time(1.0f / 60.0f);
        float one_line = -1;
        int unmeasured = 0;
        for (int i = 0; i < 1000; ++i) {
            rmp::ui::notify("Score " + std::to_string(i * 7919), { .seconds = 0.05f });
            rmp::ui::detail::begin_frame();
            rmp::ui::detail::draw_notices();
            rmp::ui::detail::end_frame();
            const int newest = rmp::ui::detail::notice_count() - 1;
            REQUIRE(newest >= 0);
            // Every one of them a line of text: the first one's height.
            const float height = rmp::ui::detail::notice_at(newest).box.height;
            if (one_line < 0) one_line = height;
            if (height != one_line) ++unmeasured;
        }
        CHECK(one_line > rmp::ui::detail::px(rmp::ui::current_theme().padding_y));
        CHECK(unmeasured == 0);
        CHECK(rmp::ui::detail::notice_layout_errors() == 0);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a notice nobody could read is reported, and queues nothing") {
        rmp::detail::reset_reports_for_tests();
        rmp::ui::notify("");
        rmp::ui::notify("Never", { .seconds = 0 });
        rmp::ui::notify("Never", { .seconds = -1 });
        rmp::ui::notify("Never", { .seconds = std::numeric_limits<float>::quiet_NaN() });
        CHECK(rmp::ui::detail::notice_count() == 0);
        CHECK(rmp::detail::report_count() == 2); // no text; no time to read it in
        // Forever is a long time, but it can be read.
        rmp::ui::notify("Pinned", { .seconds = std::numeric_limits<float>::infinity() });
        CHECK(rmp::ui::detail::notice_count() == 1);
        CHECK(rmp::detail::report_count() == 2);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a report while the notices are laid out waits for them to be "
                      "drawn") {
        rmp::detail::reset_reports_for_tests();
        rmp::detail::set_report_sink(rmp::ui::detail::show_report);
        // A full ring, of lines too long for a string to keep inline: the one
        // a report pushed out would be freed while Clay was reading it.
        auto line = [](int i) {
            return "A line long enough to live on the heap, number " + std::to_string(i);
        };
        constexpr int RING = rmp::ui::detail::NOTICE_RING;
        for (int i = 0; i < RING; ++i) rmp::ui::notify(line(i));
        rmp::ui::detail::set_measure_provider(measure_reporting);
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::draw_notices();
        rmp::ui::detail::end_frame();
        rmp::ui::detail::set_measure_provider(measure_stub);
        CHECK(rmp::detail::report_count() == 1);
        // The ring as it was drawn, the oldest making room for the report,
        // which comes after it: not drawn yet, and not a frame older.
        REQUIRE(rmp::ui::detail::notice_count() == RING);
        for (int i = 0; i + 1 < RING; ++i) {
            CAPTURE(i);
            CHECK(rmp::ui::detail::notice_at(i).text == line(i + 1));
            CHECK(rmp::ui::detail::notice_at(i).box.width > 0);
        }
        const rmp::ui::detail::NoticeView report = rmp::ui::detail::notice_at(RING - 1);
        CHECK(report.text == "test: a report while the notices are measured");
        CHECK(report.color == rmp::ui::ColorRole::DANGER);
        CHECK(report.box.width == 0);
        CHECK(report.alpha == 1);
        // The next frame draws it.
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::draw_notices();
        rmp::ui::detail::end_frame();
        CHECK(rmp::ui::detail::notice_at(RING - 1).box.width > 0);
        CHECK(rmp::ui::detail::notice_layout_errors() == 0);

        // More of them than the ring holds, during one draw: the newest.
        rmp::ui::detail::forget_notices();
        // A line no other test shows: Clay measures a string once, and the
        // notices' context outlives a test.
        rmp::ui::notify("Flooded with reports");
        flood.said = 0;
        rmp::ui::detail::set_measure_provider(measure_flooding);
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::draw_notices();
        rmp::ui::detail::end_frame();
        rmp::ui::detail::set_measure_provider(measure_stub);
        REQUIRE(rmp::ui::detail::notice_count() == RING);
        for (int i = 0; i < RING; ++i) {
            CAPTURE(i);
            CHECK(rmp::ui::detail::notice_at(i).text == "flood " + std::to_string(i + 2));
        }
    }

    TEST_CASE_FIXTURE(HeadlessUi, "a report, in a debug build, becomes a notice") {
        rmp::detail::reset_reports_for_tests();
        rmp::detail::set_report_sink(rmp::ui::detail::show_report);
        RMP_REPORT_ONCE("test: %s", "a mistake, on screen");
        rmp::detail::set_report_sink({});
        REQUIRE(rmp::ui::detail::notice_count() == 1);
        CHECK(rmp::ui::detail::notice_at(0).text == "test: a mistake, on screen");
        CHECK(rmp::ui::detail::notice_at(0).color == rmp::ui::ColorRole::DANGER);
        // Showing one is never a report of its own, or the sink would hear
        // itself: not for an empty line either.
        const int before = rmp::detail::report_count();
        rmp::ui::detail::show_report("");
        CHECK(rmp::detail::report_count() == before);
        CHECK(rmp::ui::detail::notice_count() == 1);
    }
}
