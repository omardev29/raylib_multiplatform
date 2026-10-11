// ---------------------------------------------------------------------------
// HUD focus: the interface over a game being played does not take Space.
//
// A scene that reads the keys the interface navigates with (UI_KEYS,
// src/rmp/ui_internal.h) through rmp::input is being played, and the passes
// it describes are told so (set_pass_playing()). Here: that the list is the
// list navigation reads, key by key, so the two cannot drift apart; that Tab
// is a press of its own; that the scene stack hands each scene's passes what
// that scene read; and that a frame which asks for its first control to be
// focused gets it.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/internal.h"
#include "../src/rmp/scene_internal.h"
#include "../src/rmp/ui/internal.h"

#include <rmp/input.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <algorithm>
#include <memory>
#include <set>
#include <string>

namespace {

using rmp::ui::detail::NavState;

bool moves(const NavState &s) {
    return s.x != 0 || s.y != 0 || s.activate || s.submit || s.cancel || s.tab;
}

// Navigation with exactly one key held and pressed, and nothing on a gamepad.
NavState with_key(int key) {
    return rmp::ui::detail::nav_from_keys([key](int k) { return k == key; },
                                          [key](int k) { return k == key; },
                                          [](int &, int &, bool &, bool &) {});
}

// No key, and a gamepad with exactly one button held and pressed, or one axis
// pushed all the way to `push`.
NavState with_pad(int button, int axis, float push) {
    return rmp::ui::detail::nav_from_keys(
        [](int) { return false; }, [](int) { return false; },
        [&](int &x, int &y, bool &submit, bool &cancel) {
            rmp::ui::detail::nav_from_pad(
                [button](int b) { return b == button; },
                [button](int b) { return b == button; },
                [axis, push](int a) { return a == axis ? push : 0.0f; }, x, y, submit,
                cancel);
        });
}

template <class List> std::set<int> as_set(const List &list) {
    std::set<int> out;
    for (const auto item : list) out.insert(static_cast<int>(item));
    return out;
}

// --- the headless UI, as tests/scene_test.cpp has it -------------------------
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

void no_keys(NavState &out) { out = NavState{}; }

struct HeadlessUi {
    HeadlessUi() {
        rmp::scenes::detail::shutdown();
        rmp::ui::detail::set_measure_provider(measure_stub);
        rmp::ui::detail::set_pointer_provider(pointer_away);
        rmp::ui::detail::set_nav_provider(no_keys);
        rmp::ui::detail::set_test_viewport(1280, 720);
        rmp::ui::focus("");
        (void)rmp::ui::detail::take_game_keys();
    }
    HeadlessUi(const HeadlessUi &) = delete;
    HeadlessUi &operator=(const HeadlessUi &) = delete;
    ~HeadlessUi() {
        rmp::scenes::detail::shutdown();
        rmp::ui::focus("");
        (void)rmp::ui::detail::take_game_keys();
        rmp::ui::detail::set_measure_provider(rmp::ui::detail::measure_with_raylib);
        rmp::ui::detail::set_pointer_provider(rmp::ui::detail::pointer_from_raylib);
        rmp::ui::detail::set_nav_provider(rmp::ui::detail::nav_from_raylib);
        rmp::ui::detail::set_test_viewport(0, 0);
    }
};

// What each kind of scene's _draw() was told, the last time it drew.
struct {
    int played = -1;
    int watched = -1;
} told;

// Reads a key the interface navigates with, as a game's player does.
class Played : public rmp::Scene {
public:
    void _update(float /*delta*/) override { rmp::ui::detail::note_game_keys(); }
    void _draw() override { told.played = rmp::ui::detail::pass_playing() ? 1 : 0; }
};

// Reads nothing: a menu, a title screen, a game-over card.
class Watched : public rmp::Scene {
public:
    Watched() {
        updates_below = true;
        draws_below = true;
    }
    void _draw() override { told.watched = rmp::ui::detail::pass_playing() ? 1 : 0; }
};

void run_frame() {
    rmp::ui::detail::begin_frame();
    rmp::scenes::detail::update(1.0f / 60.0f);
    rmp::scenes::detail::draw();
    rmp::ui::detail::end_frame();
    rmp::scenes::detail::apply_pending();
}

} // namespace

TEST_SUITE("ui: HUD focus") {
    TEST_CASE("the interface's keys are exactly the keys navigation reads") {
        std::set<int> moved;
        for (int key = 0; key < rmp::input::detail::DeviceState::KEYS; ++key) {
            if (moves(with_key(key))) moved.insert(key);
        }
        // Escape cancels and is left out on purpose: a pause bound to it is
        // not a game being played.
        std::set<int> expected = as_set(rmp::ui::detail::UI_KEYS);
        expected.insert(KEY_ESCAPE);
        CHECK(moved == expected);
    }

    TEST_CASE("the gamepad's buttons and axes, the same") {
        std::set<int> buttons;
        for (int b = 0; b < rmp::input::detail::DeviceState::PAD_BUTTONS; ++b) {
            if (moves(with_pad(b, -1, 0))) buttons.insert(b);
        }
        std::set<int> expected = as_set(rmp::ui::detail::UI_PAD_BUTTONS);
        expected.insert(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT); // B cancels, as Escape does
        CHECK(buttons == expected);

        std::set<int> axes;
        for (int a = 0; a < rmp::input::detail::DeviceState::AXES; ++a) {
            if (moves(with_pad(-1, a, 1.0f)) || moves(with_pad(-1, a, -1.0f)))
                axes.insert(a);
        }
        CHECK(axes == as_set(rmp::ui::detail::UI_PAD_AXES));
    }

    TEST_CASE("Tab is a press of its own, with Shift too, and held is not pressed") {
        CHECK(with_key(KEY_TAB).tab);
        CHECK(with_key(KEY_TAB).y == 1);
        const NavState back = rmp::ui::detail::nav_from_keys(
            [](int k) { return k == KEY_TAB || k == KEY_LEFT_SHIFT; },
            [](int k) { return k == KEY_TAB; }, [](int &, int &, bool &, bool &) {});
        CHECK(back.tab);
        CHECK(back.y == -1);
        const NavState held = rmp::ui::detail::nav_from_keys(
            [](int k) { return k == KEY_TAB; }, [](int) { return false; },
            [](int &, int &, bool &, bool &) {});
        CHECK_FALSE(held.tab);
        CHECK(held.y == 1);
        for (const int key : { KEY_DOWN, KEY_ENTER, KEY_SPACE, KEY_ESCAPE }) {
            CHECK_FALSE(with_key(key).tab);
        }
    }

    TEST_CASE("a note of the keys is taken once") {
        (void)rmp::ui::detail::take_game_keys();
        CHECK_FALSE(rmp::ui::detail::take_game_keys());
        rmp::ui::detail::note_game_keys();
        rmp::ui::detail::note_game_keys();
        CHECK(rmp::ui::detail::take_game_keys());
        CHECK_FALSE(rmp::ui::detail::take_game_keys());
    }

    TEST_CASE_FIXTURE(HeadlessUi, "each scene's passes are told what that scene read") {
        rmp::scenes::detail::start(std::make_unique<Played>());
        rmp::Scene::push<Watched>();
        rmp::scenes::detail::apply_pending();
        told.played = told.watched = -1;
        run_frame();
        CHECK(told.played == 1);
        CHECK(told.watched == 0);
        // And outside the stack nobody is being played.
        CHECK_FALSE(rmp::ui::detail::pass_playing());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a frame that asks for its first control focused gets it") {
        const int errors = rmp::ui::detail::clay_error_count();
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::begin({ .focus_first = true });
            rmp::ui::button("First");
            rmp::ui::button("Second");
            rmp::ui::end();
        }
        CHECK(rmp::ui::focused() == "First");
        CHECK(rmp::ui::detail::clay_error_count() == errors);
    }
}
