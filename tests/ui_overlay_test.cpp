// ---------------------------------------------------------------------------
// overlay(), headless: the frame of its own, what is behind it, and the
// backdrop doing nothing where there is no window.
//
// The blur itself needs a GPU, and is tests/ui_render_test.cpp's. What is
// here is everything a frame with no window can say: the overlay's content is
// laid out in a frame that closes where it was meant to, inside an open frame
// too; its veil takes the pointer over the whole viewport, dim 0 included;
// the passes described before it take no input from the next frame, so the
// focus goes to its first control and Enter presses it; the mistakes -- an
// overlay inside a frame, a blur in a frame of the game's own, a dim or a
// blur that is not one -- are each said once; the pure half of the backdrop
// (the dialect of GLSL for a context, the shader in each, the passes a blur
// is cut into) is what the GPU test builds on; and the backdrop's hooks and
// the GPU objects it would make are empty and harmless with no window.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/internal.h"
#include "../src/rmp/scene_internal.h"
#include "../src/rmp/ui/backdrop.h"
#include "../src/rmp/ui/internal.h"

#include <raylib.h>
#include <rlgl.h> // the RL_OPENGL_* a context is named by
#include <rmp/assets.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

// The closer is a guard, as every container's is: one overlay, closed once.
static_assert(std::is_default_constructible_v<rmp::ui::detail::OverlayCloser> &&
              !std::is_copy_constructible_v<rmp::ui::detail::OverlayCloser> &&
              !std::is_copy_assignable_v<rmp::ui::detail::OverlayCloser> &&
              !std::is_move_constructible_v<rmp::ui::detail::OverlayCloser> &&
              !std::is_move_assignable_v<rmp::ui::detail::OverlayCloser>);

// Clay's MeasureFn: its last parameter is a C pointer Clay hands back untouched.
// NOLINTBEGIN(custom-rmp-raw-pointer): Clay's signature, not ours to choose
Clay_Dimensions measure_stub(Clay_StringSlice text, Clay_TextElementConfig *config,
                             void * /*user*/) {
    // NOLINTEND(custom-rmp-raw-pointer)
    const float size = config != nullptr ? static_cast<float>(config->fontSize) : 16.0f;
    return Clay_Dimensions{ static_cast<float>(text.length) * size * 0.5f, size };
}

// The pointer, where a test puts it: up, and in the top-left corner of the
// viewport by default -- over nothing a menu in the middle paints.
struct {
    Clay_Vector2 position{ 5.0f, 5.0f };
    bool down = false;
} fake_pointer;

void pointer_stub(Clay_Vector2 &position, bool &down) {
    position = fake_pointer.position;
    down = fake_pointer.down;
}

// The keys, where a test puts them: nothing pressed by default.
struct {
    bool enter = false;
} fake_keys;

void scripted_keys(rmp::ui::detail::NavState &out) {
    out = rmp::ui::detail::NavState{};
    out.activate = fake_keys.enter;
    out.submit = fake_keys.enter;
}

struct HeadlessUi {
    HeadlessUi() {
        fake_pointer.position = Clay_Vector2{ 5.0f, 5.0f };
        fake_pointer.down = false;
        fake_keys.enter = false;
        rmp::detail::reset_reports_for_tests();
        rmp::ui::detail::set_measure_provider(measure_stub);
        rmp::ui::detail::set_pointer_provider(pointer_stub);
        rmp::ui::detail::set_nav_provider(scripted_keys);
        rmp::ui::detail::set_test_viewport(1280, 720);
        rmp::ui::focus("");
    }
    HeadlessUi(const HeadlessUi &) = delete;
    HeadlessUi &operator=(const HeadlessUi &) = delete;
    ~HeadlessUi() {
        // A frame with nothing in front, so no modality reaches the next test.
        rmp::ui::detail::begin_frame();
        rmp::ui::detail::end_frame();
        rmp::ui::focus("");
        rmp::ui::detail::set_measure_provider(rmp::ui::detail::measure_with_raylib);
        rmp::ui::detail::set_pointer_provider(rmp::ui::detail::pointer_from_raylib);
        rmp::ui::detail::set_nav_provider(rmp::ui::detail::nav_from_raylib);
        rmp::ui::detail::set_test_viewport(0, 0);
        rmp::detail::reset_reports_for_tests();
    }
};

bool has_box(std::string_view label, int pass) {
    Clay_BoundingBox box{};
    return rmp::ui::detail::bounds_of(label, 0, pass, &box) && box.width > 0 &&
        box.height > 0;
}

// The middle of where `label` was drawn last frame, in `pass`.
Clay_Vector2 middle_of(std::string_view label, int pass) {
    Clay_BoundingBox box{};
    rmp::ui::detail::bounds_of(label, 0, pass, &box);
    return Clay_Vector2{ box.x + box.width / 2, box.y + box.height / 2 };
}

// The four corners of the 1280 x 720 viewport, two pixels in, and the middle
// of each edge: wherever a veil over all of it must take the pointer.
constexpr std::array<Clay_Vector2, 8> ALL_OVER{
    Clay_Vector2{ 2, 2 },      Clay_Vector2{ 1278, 2 },  Clay_Vector2{ 2, 718 },
    Clay_Vector2{ 1278, 718 }, Clay_Vector2{ 640, 2 },   Clay_Vector2{ 640, 718 },
    Clay_Vector2{ 2, 360 },    Clay_Vector2{ 1278, 360 }
};

bool takes_the_pointer_all_over() {
    bool all = true;
    for (const Clay_Vector2 &p : ALL_OVER) {
        fake_pointer.position = p;
        if (!rmp::ui::wants_pointer()) all = false;
    }
    fake_pointer.position = Clay_Vector2{ 5.0f, 5.0f };
    return all;
}

// A frame drawn with `draw`, the pointer pressed on `at` and released there:
// three frames -- where it lands, the press, the release.
template <class Draw> void click(Clay_Vector2 at, Draw &&draw) {
    fake_pointer.position = at;
    fake_pointer.down = false;
    draw();
    fake_pointer.down = true;
    draw();
    fake_pointer.down = false;
    draw();
}

// Every frame of an app around `draw`: the backdrop's hooks, as
// rmp::app::detail::frame() calls them.
template <class Draw> void app_frame(Draw &&draw) {
    rmp::ui::detail::begin_frame();
    rmp::ui::detail::begin_backdrop();
    draw();
    rmp::ui::detail::finish_backdrop();
    rmp::ui::detail::end_frame();
}

// Where a box sits in the 1280 x 720 viewport, by thirds: -1, 0 or +1 across
// and down.
int third(float at, float size) {
    if (at < size / 3) return -1;
    if (at > 2 * size / 3) return 1;
    return 0;
}

struct Placed {
    rmp::ui::Align align;
    int x;
    int y;
};
constexpr std::array<Placed, 9> PLACEMENTS{
    Placed{ rmp::ui::Align::TOP_LEFT, -1, -1 },
    Placed{ rmp::ui::Align::TOP_CENTER, 0, -1 },
    Placed{ rmp::ui::Align::TOP_RIGHT, 1, -1 },
    Placed{ rmp::ui::Align::CENTER_LEFT, -1, 0 },
    Placed{ rmp::ui::Align::CENTER, 0, 0 },
    Placed{ rmp::ui::Align::CENTER_RIGHT, 1, 0 },
    Placed{ rmp::ui::Align::BOTTOM_LEFT, -1, 1 },
    Placed{ rmp::ui::Align::BOTTOM_CENTER, 0, 1 },
    Placed{ rmp::ui::Align::BOTTOM_RIGHT, 1, 1 },
};

// Two passes in one frame -- a HUD, then a pass in front of it -- recording
// whether each could be reached by input.
struct Reached {
    bool under = false;
    bool front = false;
};

Reached two_passes(bool front_is_modal) {
    Reached r;
    rmp::ui::detail::begin_frame();
    rmp::ui::begin();
    r.under = rmp::ui::detail::pass_input();
    rmp::ui::button("Under");
    rmp::ui::end();
    rmp::ui::begin();
    if (front_is_modal) rmp::ui::detail::set_pass_modal();
    r.front = rmp::ui::detail::pass_input();
    rmp::ui::button("Front");
    rmp::ui::end();
    rmp::ui::detail::end_frame();
    return r;
}

} // namespace

TEST_SUITE("ui: overlay") {
    TEST_CASE_FIXTURE(HeadlessUi,
                      "an overlay is a frame of its own, with its content in it") {
        const int errors = rmp::ui::detail::clay_error_count();
        int ran = 0;
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::overlay([&] {
                ++ran;
                rmp::ui::button("Resume");
            });
            CHECK_FALSE(rmp::ui::detail::frame_open());
        }
        CHECK(ran == 2);
        CHECK(has_box("Resume", 0));
        CHECK(rmp::ui::detail::clay_error_count() == errors);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "inside an open frame, that frame still closes where it should") {
        const int errors = rmp::ui::detail::clay_error_count();
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::begin();
            rmp::ui::overlay({ .blur = 8, .dim = 0.5f },
                             [] { rmp::ui::button("Inside"); });
            CHECK(rmp::ui::detail::frame_open());
            rmp::ui::button("After");
            rmp::ui::end();
        }
        CHECK(has_box("Inside", 0));
        CHECK(has_box("After", 0));
        CHECK_FALSE(rmp::ui::detail::frame_open());
        CHECK(rmp::ui::detail::clay_error_count() == errors);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a pass in front takes the input of the passes behind it") {
        // Nothing in front last frame: both reachable.
        Reached r = two_passes(false);
        CHECK(r.under);
        CHECK(r.front);
        // The frame it says so in is already described up to it.
        r = two_passes(true);
        CHECK(r.under);
        CHECK(r.front);
        // From the next frame, for as long as it keeps saying so.
        r = two_passes(true);
        CHECK_FALSE(r.under);
        CHECK(r.front);
        // And one frame after it stops.
        r = two_passes(false);
        CHECK_FALSE(r.under);
        r = two_passes(false);
        CHECK(r.under);
        CHECK(r.front);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a frame painted all over takes the pointer everywhere") {
        // The pointer is in a corner, nowhere near a centred menu.
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::begin();
            rmp::ui::button("Middle");
            rmp::ui::end();
        }
        CHECK_FALSE(rmp::ui::wants_pointer());
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::detail::open_frame(rmp::ui::FrameOptions{}, Color{ 0, 0, 0, 120 });
            rmp::ui::button("Middle");
            rmp::ui::end();
        }
        CHECK(rmp::ui::wants_pointer());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "the veil takes the pointer all over the viewport, dim 0 too") {
        // A menu with no veil does not: the corners are the game's.
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::begin();
            rmp::ui::button("Resume");
            rmp::ui::end();
        }
        fake_pointer.position = Clay_Vector2{ 2, 2 };
        CHECK_FALSE(rmp::ui::wants_pointer());
        for (const float dim : { 0.45f, 1.0f, 0.0f }) {
            CAPTURE(dim);
            for (int frame = 0; frame < 2; ++frame) {
                rmp::ui::overlay({ .dim = dim }, [] { rmp::ui::button("Resume"); });
            }
            CHECK(takes_the_pointer_all_over());
        }
        // A veil the colour of nothing is dim 0: it still takes the pointer.
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::overlay({ .veil = Color{ 0, 0, 0, 0 } },
                             [] { rmp::ui::button("Resume"); });
        }
        CHECK(takes_the_pointer_all_over());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a scroll area in it scrolls under a finger, dim 0 too") {
        // What takes the pointer from the game must not take it from the
        // overlay's own content: Clay decides which scroll area a drag
        // belongs to, and a layer over the viewport that captures the pointer
        // -- what a veil painting nothing is drawn with -- hides every area
        // under it from Clay.
        for (const float dim : { 0.45f, 0.0f }) {
            CAPTURE(dim);
            const std::string area = dim > 0 ? "veiled list" : "clear list";
            const std::string first = area + " 0";
            const auto draw = [&] {
                rmp::ui::overlay(
                    { .placement = rmp::ui::Align::TOP_LEFT, .dim = dim }, [&] {
                        rmp::ui::scroll({ .height = 80, .id = area }, [&] {
                            for (int i = 0; i < 8; ++i) {
                                rmp::ui::button(area + " " + std::to_string(i));
                            }
                        });
                    });
            };
            fake_pointer.position = Clay_Vector2{ -1, -1 };
            fake_pointer.down = false;
            draw();
            draw();
            Clay_BoundingBox list{};
            Clay_BoundingBox before{};
            REQUIRE(rmp::ui::detail::bounds_of(area, 0, 0, &list));
            REQUIRE(rmp::ui::detail::bounds_of(first, 0, 0, &before));
            // Pressed in the area, and dragged up a few pixels a frame.
            const Clay_Vector2 at{ list.x + list.width / 2, list.y + list.height / 2 };
            fake_pointer.position = at;
            fake_pointer.down = true;
            draw();
            for (int step = 1; step <= 8; ++step) {
                fake_pointer.position =
                    Clay_Vector2{ at.x, at.y - 6.0f * static_cast<float>(step) };
                draw();
            }
            fake_pointer.down = false;
            draw();
            draw();
            Clay_BoundingBox after{};
            REQUIRE(rmp::ui::detail::bounds_of(first, 0, 0, &after));
            CHECK(after.y < before.y - 20);
        }
    }

    TEST_CASE_FIXTURE(
        HeadlessUi, "the passes described before it take no input from the next frame") {
        int under = 0;
        int resume = 0;
        bool reachable = true;
        auto draw = [&](bool with_overlay) {
            return [&, with_overlay] {
                rmp::ui::detail::begin_frame();
                rmp::ui::begin();
                reachable = rmp::ui::detail::pass_input();
                if (rmp::ui::button("Under")) ++under;
                rmp::ui::end();
                if (with_overlay) {
                    rmp::ui::overlay({ .placement = rmp::ui::Align::TOP_LEFT }, [&] {
                        if (rmp::ui::button("Resume")) ++resume;
                    });
                }
                rmp::ui::detail::end_frame();
            };
        };
        // With nothing in front, Under clicks.
        draw(false)();
        click(middle_of("Under", 0), draw(false));
        CHECK(under == 1);
        // The frame the overlay appears in, Under could still be reached: the
        // frame was described up to it before it said so.
        draw(true)();
        CHECK(reachable);
        // From the next, Under is behind it: no input, no click -- the veil
        // is over it.
        click(middle_of("Under", 0), draw(true));
        CHECK_FALSE(reachable);
        CHECK(under == 1);
        // And the overlay's own control does.
        click(middle_of("Resume", 1), draw(true));
        CHECK(resume == 1);
        // Gone, and a frame later Under takes input again.
        draw(false)();
        draw(false)();
        CHECK(reachable);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "its first control takes the focus, and Enter presses it") {
        int play = 0;
        int resume = 0;
        auto draw = [&](bool paused) {
            rmp::ui::detail::begin_frame();
            rmp::ui::begin();
            if (rmp::ui::button("Play")) ++play;
            rmp::ui::end();
            if (paused) {
                rmp::ui::overlay([&] {
                    if (rmp::ui::button("Resume")) ++resume;
                    rmp::ui::button("Quit");
                });
            }
            rmp::ui::detail::end_frame();
        };
        draw(false);
        draw(false);
        CHECK(rmp::ui::focused() == "Play");
        // The menu had the focus; the overlay takes it, within the frames
        // the menu takes to go behind it.
        for (int frame = 0; frame < 3; ++frame) draw(true);
        CHECK(rmp::ui::focused() == "Resume");
        fake_keys.enter = true;
        draw(true);
        fake_keys.enter = false;
        draw(true);
        CHECK(resume == 1);
        CHECK(play == 0);
    }

    TEST_CASE_FIXTURE(
        HeadlessUi,
        "inside an open frame: said once, and a layer in front of that frame") {
        const int reports = rmp::detail::report_count();
        int after = 0;
        int inside = 0;
        auto draw = [&] {
            rmp::ui::begin();
            if (rmp::ui::button("After")) ++after;
            rmp::ui::overlay({ .placement = rmp::ui::Align::TOP_LEFT, .blur = 8 }, [&] {
                if (rmp::ui::button("Inside")) ++inside;
            });
            rmp::ui::end();
        };
        for (int frame = 0; frame < 60; ++frame) draw();
        CHECK(rmp::detail::report_count() == reports + 1);
        CHECK_FALSE(rmp::ui::detail::frame_open());
        // The layer is over the whole viewport and takes the pointer there...
        CHECK(takes_the_pointer_all_over());
        // ...the frame's own control under it cannot be clicked through it...
        const Clay_Vector2 under = middle_of("After", 0);
        const Clay_Vector2 over = middle_of("Inside", 0);
        CHECK(under.x > 400); // centred, nowhere near the top-left corner
        CHECK(over.x < 400);
        click(under, draw);
        CHECK(after == 0);
        // ...and what is in the layer can.
        click(over, draw);
        CHECK(inside == 1);
        CHECK(rmp::detail::report_count() == reports + 1);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "its content goes where placement says, in a frame or a layer") {
        for (const Placed &p : PLACEMENTS) {
            CAPTURE(static_cast<int>(p.align));
            for (int frame = 0; frame < 2; ++frame) {
                rmp::ui::overlay({ .placement = p.align },
                                 [] { rmp::ui::button("Here"); });
            }
            Clay_Vector2 at = middle_of("Here", 0);
            CHECK(third(at.x, 1280) == p.x);
            CHECK(third(at.y, 720) == p.y);
            for (int frame = 0; frame < 2; ++frame) {
                rmp::ui::begin();
                rmp::ui::overlay({ .placement = p.align },
                                 [] { rmp::ui::button("Here"); });
                rmp::ui::end();
            }
            at = middle_of("Here", 0);
            CHECK(third(at.x, 1280) == p.x);
            CHECK(third(at.y, 720) == p.y);
        }
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a blur needs the app's frame, and says so once without one") {
        const int reports = rmp::detail::report_count();
        // The game's own frame (RMP_ENTRY_POINT): nothing to blur.
        for (int frame = 0; frame < 60; ++frame) {
            rmp::ui::overlay({ .blur = 6 }, [] { rmp::ui::button("Resume"); });
        }
        CHECK(rmp::detail::report_count() == reports + 1);
        // No blur asked: nothing to say.
        rmp::detail::reset_reports_for_tests();
        for (int frame = 0; frame < 5; ++frame) {
            rmp::ui::overlay({ .blur = 0 }, [] { rmp::ui::button("Resume"); });
        }
        CHECK(rmp::detail::report_count() == 0);
        // The app's frame, which can be redirected: not a mistake, and with no
        // window nothing is redirected all the same.
        for (int frame = 0; frame < 60; ++frame) {
            app_frame([] {
                rmp::ui::overlay({ .blur = 6 }, [] { rmp::ui::button("Resume"); });
            });
            CHECK_FALSE(rmp::ui::detail::backdrop_redirected());
            CHECK_FALSE(rmp::ui::detail::backdrop_blurred());
        }
        CHECK(rmp::detail::report_count() == 0);
        // And an app frame with no overlay in it after them.
        app_frame([] {});
        CHECK_FALSE(rmp::ui::detail::backdrop_redirected());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a scene's blur outside the app's frame is not called its own") {
        // A pause scene, drawn by the stack with no app frame around it -- a
        // test driving the stack, or a scene describing its overlay where it
        // should not, which is begin()'s to say. Not RMP_ENTRY_POINT's frame:
        // nothing about that here.
        class Paused : public rmp::Scene {
        public:
            void _draw() override {
                rmp::ui::overlay({ .blur = 6 }, [] { rmp::ui::button("Resume"); });
            }
        };
        rmp::scenes::detail::shutdown();
        rmp::scenes::detail::start(std::make_unique<Paused>());
        REQUIRE(rmp::scenes::detail::running());
        const int reports = rmp::detail::report_count();
        for (int frame = 0; frame < 3; ++frame) {
            rmp::ui::detail::begin_frame();
            rmp::scenes::detail::draw();
            rmp::ui::detail::end_frame();
        }
        CHECK(has_box("Resume", 0));
        CHECK(rmp::detail::report_count() == reports);
        rmp::scenes::detail::shutdown();
        CHECK_FALSE(rmp::scenes::detail::running());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a dim or a blur that is not one is said once, and made one") {
        constexpr float NOT_A_NUMBER = std::numeric_limits<float>::quiet_NaN();
        constexpr float ENDLESS = std::numeric_limits<float>::infinity();
        auto show = [](const rmp::ui::OverlayOptions &o) {
            for (int frame = 0; frame < 3; ++frame) {
                rmp::ui::overlay(o, [] { rmp::ui::button("Resume"); });
            }
        };
        // What is fine says nothing.
        for (const float dim : { 0.0f, 0.45f, 1.0f }) show({ .blur = 0, .dim = dim });
        for (const float blur : { 0.0f, 0.5f, 6.0f, 400.0f }) {
            app_frame([&] { show({ .blur = blur }); });
        }
        CHECK(rmp::detail::report_count() == 0);
        // A dim out of 0..1, however far out, is one mistake.
        for (const float dim : { -0.5f, 2.0f, NOT_A_NUMBER, -ENDLESS, ENDLESS }) {
            CAPTURE(dim);
            show({ .dim = dim });
            CHECK(rmp::detail::report_count() == 1);
            // Still a veil, so still the pointer's, whatever it was made into.
            CHECK(takes_the_pointer_all_over());
        }
        // A blur that is not a distance is another.
        for (const float blur : { -1.0f, NOT_A_NUMBER, ENDLESS, -ENDLESS }) {
            CAPTURE(blur);
            app_frame([&] { show({ .blur = blur }); });
            CHECK(rmp::detail::report_count() == 2);
        }
    }

    TEST_CASE_FIXTURE(HeadlessUi, "deeper than its stack, every overlay still closes") {
        const int errors = rmp::ui::detail::clay_error_count();
        const int reports = rmp::detail::report_count();
        // Ten deep inside a frame: past the eight it remembers.
        struct Nest {
            static void in(int depth) {
                if (depth == 0) {
                    rmp::ui::button("Deepest");
                    return;
                }
                rmp::ui::overlay([depth] { in(depth - 1); });
            }
        };
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::begin();
            Nest::in(10);
            CHECK(rmp::ui::detail::frame_open());
            rmp::ui::button("After");
            rmp::ui::end();
            CHECK_FALSE(rmp::ui::detail::frame_open());
        }
        CHECK(has_box("Deepest", 0));
        CHECK(has_box("After", 0));
        CHECK(rmp::ui::detail::clay_error_count() == errors);
        CHECK(rmp::detail::report_count() == reports + 1);
        // And a frame of its own after them: the stack is empty again.
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::overlay([] { rmp::ui::button("Alone"); });
            CHECK_FALSE(rmp::ui::detail::frame_open());
        }
        CHECK(has_box("Alone", 0));
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a body that ends the frame itself is not ended twice") {
        const int errors = rmp::ui::detail::clay_error_count();
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::overlay([] {
                rmp::ui::button("Early");
                rmp::ui::end();
            });
            CHECK_FALSE(rmp::ui::detail::frame_open());
        }
        // A close with nothing open closes nothing: not the frame open now.
        rmp::ui::begin();
        rmp::ui::detail::close_overlay();
        CHECK(rmp::ui::detail::frame_open());
        rmp::ui::button("Still");
        rmp::ui::end();
        CHECK(has_box("Still", 0));
        CHECK(rmp::ui::detail::clay_error_count() == errors);
    }

    TEST_CASE("with no window the backdrop does nothing and makes nothing") {
        rmp::detail::reset_reports_for_tests();
        // Two app frames, the first asking for a blur: the second would be
        // redirected, and with no window -- no test viewport either, so the
        // window's size is asked for, and is 0 -- it is not.
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::detail::begin_backdrop();
            CHECK_FALSE(rmp::ui::detail::backdrop_redirected());
            rmp::ui::detail::blur_backdrop(8);
            rmp::ui::detail::finish_backdrop();
            CHECK_FALSE(rmp::ui::detail::backdrop_redirected());
            CHECK_FALSE(rmp::ui::detail::backdrop_blurred());
        }
        CHECK(rmp::detail::report_count() == 0);
        // A test's shader, and back: what was built is forgotten with it.
        rmp::ui::detail::set_test_blur_shader(
            rmp::ui::detail::blur_shader_source(rmp::ui::detail::Glsl::ES_100));
        rmp::ui::detail::set_test_blur_shader(std::nullopt);
        CHECK_FALSE(rmp::ui::detail::backdrop_redirected());
        rmp::ui::detail::release_backdrop();
        CHECK_FALSE(rmp::assets::detail::make_render_texture(0, 0).valid());
        CHECK_FALSE(rmp::assets::detail::make_render_texture(-1, 100).valid());
        CHECK_FALSE(rmp::assets::detail::make_render_texture(100, -1).valid());
        CHECK_FALSE(rmp::assets::detail::make_shader("", "").valid());
        CHECK_FALSE(rmp::assets::detail::make_shader("void main() {}", "").valid());
        rmp::detail::reset_reports_for_tests();
    }
}

// The half of the backdrop that needs no GPU: what tests/ui_render_test.cpp
// builds and draws, checked here for what it is.
TEST_SUITE("ui: backdrop") {
    using rmp::ui::detail::Glsl;

    TEST_CASE(
        "each context raylib names gets the GLSL it takes, and none where none runs") {
        CHECK(rmp::ui::detail::glsl_for(RL_OPENGL_ES_20) == Glsl::ES_100);
        CHECK(rmp::ui::detail::glsl_for(RL_OPENGL_ES_30) == Glsl::ES_300);
        CHECK(rmp::ui::detail::glsl_for(RL_OPENGL_33) == Glsl::CORE_330);
        CHECK(rmp::ui::detail::glsl_for(RL_OPENGL_43) == Glsl::CORE_330);
        CHECK(rmp::ui::detail::glsl_for(RL_OPENGL_SOFTWARE) == Glsl::NONE);
        CHECK(rmp::ui::detail::glsl_for(RL_OPENGL_11) == Glsl::NONE);
        CHECK(rmp::ui::detail::glsl_for(RL_OPENGL_21) == Glsl::NONE);
        for (const int nonsense : { -1, 7, 99, 1000 }) {
            CHECK(rmp::ui::detail::glsl_for(nonsense) == Glsl::NONE);
        }
    }

    TEST_CASE(
        "the shader in each dialect: its own prologue, both halves, raylib's names") {
        const auto has = [](const std::string &text, std::string_view what) {
            return text.find(what) != std::string::npos;
        };
        const auto starts = [](const std::string &text, std::string_view what) {
            return text.starts_with(what);
        };
        const auto es100 = rmp::ui::detail::blur_shader_source(Glsl::ES_100);
        const auto es300 = rmp::ui::detail::blur_shader_source(Glsl::ES_300);
        const auto core = rmp::ui::detail::blur_shader_source(Glsl::CORE_330);
        // One version per program: the vertex half in the fragment's.
        CHECK(starts(es100.vertex, "#version 100\n"));
        CHECK(starts(es100.fragment, "#version 100\n"));
        CHECK(starts(es300.vertex, "#version 300 es\n"));
        CHECK(starts(es300.fragment, "#version 300 es\n"));
        CHECK(starts(core.vertex, "#version 330\n"));
        CHECK(starts(core.fragment, "#version 330\n"));
        // GLSL ES 1.00 speaks attribute, varying, texture2D and gl_FragColor,
        // and must say how precise a float is.
        CHECK(has(es100.vertex, "attribute vec3 vertexPosition;"));
        CHECK(has(es100.vertex, "varying vec2 fragTexCoord;"));
        CHECK(has(es100.fragment, "precision mediump float;"));
        CHECK(has(es100.fragment, "precision highp float;"));
        CHECK(has(es100.fragment, "texture2D(texture0"));
        CHECK(has(es100.fragment, "gl_FragColor"));
        CHECK_FALSE(has(es100.fragment, "out vec4"));
        // 3.00 ES and 330 speak in, out and texture(), and have none of it.
        const auto in_and_out = [&](const rmp::ui::detail::ShaderSource &later) {
            CHECK(has(later.vertex, "in vec3 vertexPosition;"));
            CHECK(has(later.vertex, "out vec2 fragTexCoord;"));
            CHECK(has(later.fragment, "in vec2 fragTexCoord;"));
            CHECK(has(later.fragment, "out vec4 finalColor;"));
            CHECK(has(later.fragment, "texture(texture0"));
            for (const std::string_view old :
                 { "attribute", "varying", "texture2D", "gl_FragColor" }) {
                CHECK_FALSE(has(later.vertex, old));
                CHECK_FALSE(has(later.fragment, old));
            }
        };
        in_and_out(es300);
        in_and_out(core);
        // ES 3.00 must say how precise a float is; desktop 330 need not.
        CHECK(has(es300.fragment, "precision highp float;"));
        CHECK_FALSE(has(core.fragment, "precision"));
        // In every dialect: the names raylib binds and sets, the two uniforms
        // the backdrop sets, and the tap count it plans with.
        const auto common = [&](const rmp::ui::detail::ShaderSource &source) {
            for (const std::string_view name : { "vertexPosition", "vertexTexCoord",
                                                 "vertexColor", "uniform mat4 mvp;" }) {
                CHECK(has(source.vertex, name));
            }
            for (const std::string_view name :
                 { "uniform sampler2D texture0;", "uniform vec2 direction;",
                   "uniform float sigma;" }) {
                CHECK(has(source.fragment, name));
            }
            CHECK(has(source.fragment,
                      "const int TAPS = " + std::to_string(rmp::ui::detail::BLUR_TAPS) +
                          ";"));
        };
        common(es100);
        common(es300);
        common(core);
        // Where no shader runs there is none.
        const auto none = rmp::ui::detail::blur_shader_source(Glsl::NONE);
        CHECK(none.vertex.empty());
        CHECK(none.fragment.empty());
    }

    TEST_CASE("a blur is cut into passes that reach three sigmas a texel at a time") {
        using rmp::ui::detail::BLUR_MAX_DIVISOR;
        using rmp::ui::detail::BLUR_TAPS;
        constexpr int SCREEN = 1920;
        const auto plan_blur = [](float px, int size = SCREEN) {
            return rmp::ui::detail::plan_blur(px, size);
        };
        // Nothing to blur: not a distance, or under half a pixel.
        for (const float none :
             { std::numeric_limits<float>::quiet_NaN(),
               std::numeric_limits<float>::infinity(),
               -std::numeric_limits<float>::infinity(), -1.0f, 0.0f, 0.49f }) {
            CAPTURE(none);
            CHECK(plan_blur(none).divisor == 0);
        }
        // Small: at half the resolution.
        CHECK(plan_blur(0.5f).divisor == 2);
        CHECK(plan_blur(2.9f).divisor == 2);
        CHECK(plan_blur(2.0f).sigma == doctest::Approx(1.0f));
        // From three pixels: at a quarter.
        CHECK(plan_blur(3.0f).divisor == 4);
        CHECK(plan_blur(4.0f).sigma == doctest::Approx(1.0f));
        CHECK(plan_blur(10.0f).divisor == 4);
        // Wider: a smaller picture, the taps still a texel apart.
        CHECK(plan_blur(11.0f).divisor == 8);
        const auto wide = plan_blur(40.0f);
        CHECK(wide.divisor == 16);
        CHECK(wide.sigma == doctest::Approx(2.5f));
        // However wide, no smaller than one texel across the frame...
        CHECK(plan_blur(1.0e30f).divisor == 2048);
        CHECK(plan_blur(std::numeric_limits<float>::max(), 320).divisor == 512);
        CHECK(plan_blur(1.0e30f, 512).divisor == 512);
        CHECK(plan_blur(1.0e30f, 1).divisor == 4);
        CHECK(plan_blur(1.0f, 1).divisor == 2);
        CHECK(plan_blur(1.0e30f, 0).divisor == 4);
        // ...and never past the most it is drawn with.
        CHECK(plan_blur(1.0e30f, 1 << 20).divisor == BLUR_MAX_DIVISOR);
        CHECK(plan_blur(1.0e30f, 1 << 20).sigma == doctest::Approx(1.0e30f / 4096.0f));
        // Over every blur from half a pixel to two thousand: the plan is the
        // blur asked for, at a power of two, the kernel reaches three sigmas,
        // and the picture is no smaller than that takes.
        for (int step = 0; step < 125; ++step) {
            const float px = 0.5f * std::pow(1.07f, static_cast<float>(step)); // to 2200
            CAPTURE(px);
            const auto plan = plan_blur(px, 1 << 16);
            CHECK(plan.divisor >= 2);
            CHECK((plan.divisor & (plan.divisor - 1)) == 0);
            CHECK((plan.divisor == 2) == (px < 3.0f));
            CHECK(plan.sigma * static_cast<float>(plan.divisor) == doctest::Approx(px));
            CHECK(3.0f * plan.sigma <= static_cast<float>(BLUR_TAPS) + 1e-4f);
            CHECK((plan.divisor <= 4 ||
                   3.0f * px > static_cast<float>(BLUR_TAPS * (plan.divisor / 2))));
        }
    }
}
