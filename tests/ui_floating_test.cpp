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

#include <rmp/scene.h>
#include <rmp/ui.h>

#include <memory>
#include <type_traits>

namespace {

static_assert(std::is_default_constructible_v<rmp::ui::detail::AtCloser> &&
              !std::is_copy_constructible_v<rmp::ui::detail::AtCloser> &&
              !std::is_copy_assignable_v<rmp::ui::detail::AtCloser> &&
              !std::is_move_constructible_v<rmp::ui::detail::AtCloser> &&
              !std::is_move_assignable_v<rmp::ui::detail::AtCloser>);

// Clay's MeasureFn: its last parameter is a C pointer Clay hands back untouched.
// NOLINTBEGIN(custom-rmp-raw-pointer): Clay's signature, not ours to choose
Clay_Dimensions measure_stub(Clay_StringSlice text, Clay_TextElementConfig *config,
                             void * /*user*/) {
    // NOLINTEND(custom-rmp-raw-pointer)
    const float size = config != nullptr ? static_cast<float>(config->fontSize) : 16.0f;
    return Clay_Dimensions{ static_cast<float>(text.length) * size * 0.5f, size };
}

void pointer_away(Clay_Vector2 &position, bool &down) {
    position = Clay_Vector2{ -1.0f, -1.0f };
    down = false;
}

struct HeadlessUi {
    HeadlessUi() {
        rmp::scenes::detail::shutdown();
        rmp::ui::detail::set_measure_provider(measure_stub);
        rmp::ui::detail::set_pointer_provider(pointer_away);
        rmp::ui::detail::set_test_viewport(1280, 720);
    }
    HeadlessUi(const HeadlessUi &) = delete;
    HeadlessUi &operator=(const HeadlessUi &) = delete;
    ~HeadlessUi() {
        rmp::scenes::detail::shutdown();
        rmp::ui::detail::set_measure_provider(rmp::ui::detail::measure_with_raylib);
        rmp::ui::detail::set_pointer_provider(rmp::ui::detail::pointer_from_raylib);
        rmp::ui::detail::set_test_viewport(0, 0);
    }
};

bool same(const Camera2D &a, const Camera2D &b) {
    return a.offset.x == b.offset.x && a.offset.y == b.offset.y &&
        a.target.x == b.target.x && a.target.y == b.target.y &&
        a.rotation == b.rotation && a.zoom == b.zoom;
}

bool identity(const Camera2D &c) {
    return same(c,
                Camera2D{ .offset = Vector2{ 0, 0 },
                          .target = Vector2{ 0, 0 },
                          .rotation = 0,
                          .zoom = 1 });
}

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

bool has_box(std::string_view label) {
    Clay_BoundingBox box{};
    return rmp::ui::detail::bounds_of(label, 0, 0, &box) && box.width > 0 &&
        box.height > 0;
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
                      "a notice, or a report on screen, does not start the UI") {
        rmp::ui::shutdown();
        REQUIRE_FALSE(rmp::ui::detail::started());
        rmp::ui::notify("Saved");
        rmp::ui::detail::show_report("UI: a mistake");
        CHECK_FALSE(rmp::ui::detail::started());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "the notices are drawn as a pass that leaves no frame open") {
        rmp::ui::detail::begin_frame();
        rmp::ui::begin();
        rmp::ui::button("Game");
        rmp::ui::end();
        rmp::ui::detail::draw_notices();
        CHECK_FALSE(rmp::ui::detail::frame_open());
        rmp::ui::detail::end_frame();
    }
}
