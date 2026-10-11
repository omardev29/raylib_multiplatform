// ---------------------------------------------------------------------------
// overlay(), headless: the frame of its own, what is behind it, and the
// backdrop doing nothing where there is no window.
//
// The blur itself needs a GPU, and is tests/ui_render_test.cpp's. What is
// here is everything a frame with no window can say: the overlay's content is
// laid out in a frame that closes where it was meant to, inside an open frame
// too; a pass in front of the others takes their input from the next frame
// (set_pass_modal()); a frame painted over the whole viewport takes the
// pointer everywhere (open_frame()); and the backdrop's hooks and the GPU
// objects it would make are empty and harmless with no window.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/internal.h"
#include "../src/rmp/ui/backdrop.h"
#include "../src/rmp/ui/internal.h"

#include <rmp/assets.h>
#include <rmp/ui.h>

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

void no_keys(rmp::ui::detail::NavState &out) { out = rmp::ui::detail::NavState{}; }

struct HeadlessUi {
    HeadlessUi() {
        fake_pointer.position = Clay_Vector2{ 5.0f, 5.0f };
        fake_pointer.down = false;
        rmp::ui::detail::set_measure_provider(measure_stub);
        rmp::ui::detail::set_pointer_provider(pointer_stub);
        rmp::ui::detail::set_nav_provider(no_keys);
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
    }
};

bool has_box(std::string_view label, int pass) {
    Clay_BoundingBox box{};
    return rmp::ui::detail::bounds_of(label, 0, pass, &box) && box.width > 0 &&
        box.height > 0;
}

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

    TEST_CASE("with no window the backdrop does nothing and makes nothing") {
        rmp::ui::detail::begin_backdrop();
        CHECK_FALSE(rmp::ui::detail::backdrop_redirected());
        rmp::ui::detail::blur_backdrop(8);
        rmp::ui::detail::finish_backdrop();
        CHECK_FALSE(rmp::ui::detail::backdrop_redirected());
        rmp::ui::detail::release_backdrop();
        CHECK_FALSE(rmp::assets::detail::make_render_texture(0, 0).valid());
        CHECK_FALSE(rmp::assets::detail::make_render_texture(-1, 100).valid());
        CHECK_FALSE(rmp::assets::detail::make_shader("", "").valid());
    }
}
