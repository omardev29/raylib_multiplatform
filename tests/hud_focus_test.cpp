// ---------------------------------------------------------------------------
// HUD focus: the interface over a game being played does not take Space.
//
// A scene that reads the keys the interface navigates with (UI_KEYS,
// src/rmp/ui_internal.h) through rmp::input is being played, and the passes
// it describes are told so (set_pass_playing()). Here: that the list is the
// list navigation reads, key by key, so the two cannot drift apart; that Tab
// is a press of its own; that the scene stack hands each scene's passes what
// that scene read; and then what the focus makes of it -- a HUD over a game
// being played takes no focus and no key until Tab, gives the keys back on
// Escape or a click elsewhere, and a pause menu over it is a menu as before.
// FrameOptions::focus_first says it either way when the guess is wrong.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/internal.h"
#include "../src/rmp/scene_internal.h"
#include "../src/rmp/ui/internal.h"

#include <rmp/input.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

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

// What the player does, as both halves of the framework read it: the UI's
// navigation and pointer through its providers, the game's keys through
// rmp::input's. A test writes it; a frame reads it. The dot says "file state".
struct {
    NavState nav{};
    Clay_Vector2 pointer{ -1.0f, -1.0f };
    bool down = false;
    rmp::input::detail::DeviceState devices{};
} feed;

void nav_feed(NavState &out) { out = feed.nav; }
void pointer_feed(Clay_Vector2 &position, bool &down) {
    position = feed.pointer;
    down = feed.down;
}
void devices_feed(rmp::input::detail::DeviceState &out) { out = feed.devices; }

void describe_nothing() {
    rmp::ui::begin();
    rmp::ui::end();
}

struct HeadlessUi {
    HeadlessUi() {
        rmp::scenes::detail::shutdown();
        feed.nav = NavState{};
        feed.pointer = Clay_Vector2{ -1.0f, -1.0f };
        feed.down = false;
        feed.devices = rmp::input::detail::DeviceState{};
        rmp::input::detail::reset();
        rmp::input::detail::set_sample_provider(devices_feed);
        rmp::input::action("jump", KEY_SPACE, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        rmp::input::action("pause", KEY_ESCAPE);
        rmp::ui::detail::set_measure_provider(measure_stub);
        rmp::ui::detail::set_pointer_provider(pointer_feed);
        rmp::ui::detail::set_nav_provider(nav_feed);
        rmp::ui::detail::set_test_viewport(1280, 720);
        rmp::ui::focus("");
        rmp::ui::detail::set_focus_visible(false);
        // A frame with nothing focusable in it: whatever an earlier test left
        // on screen is not on screen any more.
        describe_nothing();
        (void)rmp::ui::detail::take_game_keys();
    }
    HeadlessUi(const HeadlessUi &) = delete;
    HeadlessUi &operator=(const HeadlessUi &) = delete;
    ~HeadlessUi() {
        rmp::scenes::detail::shutdown();
        feed.nav = NavState{};
        feed.down = false;
        rmp::ui::focus("");
        describe_nothing();
        (void)rmp::ui::detail::take_game_keys();
        rmp::input::detail::reset();
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

// One turn of the loop, in src/rmp/app.cpp's order: the devices sampled, the
// UI's frame marked, the scenes updated and drawn, the frame closed.
void run_frame() {
    rmp::input::detail::begin_frame();
    rmp::ui::detail::begin_frame();
    rmp::scenes::detail::update(1.0f / 60.0f);
    rmp::scenes::detail::draw();
    rmp::ui::detail::end_frame();
    rmp::scenes::detail::apply_pending();
}

void run_frames(int n) {
    for (int i = 0; i < n; ++i) run_frame();
}

// --- a game with a HUD ---------------------------------------------------------

// What the game scene reads in its _update(), through rmp::input.
enum class Read { NOTHING, JUMP, PAUSE, RAW_SPACE, MOVE };

// How the game is played, and what came of it. The dot says "file state".
struct {
    Read read = Read::JUMP;
    std::optional<bool> focus_first{};
    int map = 0; // presses of the HUD's two buttons
    int bag = 0;
    int resumed = 0; // and of the pause menu's
    int jumps = 0; // what the game heard, routed
    int pauses = 0;
    bool playing = false; // what its pass was told, last time it drew
    bool chat = false; // the HUD has a text field too
    std::string said;
} game;

void read_as_told() {
    switch (game.read) {
        case Read::NOTHING:
            break;
        case Read::JUMP:
            if (rmp::input::just_pressed("jump")) ++game.jumps;
            break;
        case Read::PAUSE:
            if (rmp::input::just_pressed("pause")) ++game.pauses;
            break;
        case Read::RAW_SPACE:
            (void)rmp::input::pressed(KEY_SPACE);
            break;
        case Read::MOVE:
            (void)rmp::input::vector();
            break;
    }
    // Every game here may pause, and Escape is not one of the keys: reading it
    // does not make a scene a game being played.
    if (game.read != Read::PAUSE && rmp::input::just_pressed("pause")) ++game.pauses;
}

// The game being played, and its HUD in the top right corner.
class Field : public rmp::Scene {
public:
    void _update(float /*delta*/) override { read_as_told(); }
    void _draw() override {
        game.playing = rmp::ui::detail::pass_playing();
        rmp::ui::begin(
            { .placement = rmp::ui::Align::TOP_RIGHT, .focus_first = game.focus_first });
        if (rmp::ui::button("Map")) ++game.map;
        if (rmp::ui::button("Bag")) ++game.bag;
        if (game.chat) rmp::ui::text_input("Say", game.said);
        rmp::ui::end();
    }
};

// A pause menu over it, which reads only Escape: a menu, not a game.
class Pause : public rmp::Scene {
public:
    void _update(float /*delta*/) override { (void)rmp::input::just_pressed("pause"); }
    void _draw() override {
        rmp::ui::begin();
        if (rmp::ui::button("Resume")) {
            ++game.resumed;
            rmp::Scene::pop();
        }
        rmp::ui::button("Quit");
        rmp::ui::end();
    }
};

// A menu the game goes on under, and that input reaches through: a bag
// opened over a game that does not stop for it. It reads nothing itself.
class Inventory : public rmp::Scene {
public:
    Inventory() {
        updates_below = true;
        input_below = true;
    }
    void _draw() override {
        rmp::ui::begin();
        rmp::ui::button("Use");
        rmp::ui::button("Drop");
        rmp::ui::end();
    }
};

// The game, started on its own, played for two frames: the first draws the HUD,
// the second is the first that can know where it is.
void play(Read read, std::optional<bool> focus_first = std::nullopt) {
    game = {};
    game.read = read;
    game.focus_first = focus_first;
    rmp::scenes::detail::start(std::make_unique<Field>());
    rmp::scenes::detail::apply_pending();
    run_frames(2);
}

// One frame with these pressed; the next starts with nothing pressed.
void press(const NavState &nav) {
    feed.nav = nav;
    run_frame();
    feed.nav = NavState{};
}

constexpr NavState TAB{ .y = 1, .tab = true };
constexpr NavState DOWN{ .y = 1 };
constexpr NavState ENTER{ .activate = true, .submit = true }; // Enter, and the A button
constexpr NavState SPACE{ .activate = true };
constexpr NavState ESCAPE{ .cancel = true }; // Escape, and the B button

// Where the first pass of the last frame put `label`, in the middle.
Clay_Vector2 centre_of(std::string_view label) {
    Clay_BoundingBox box{};
    REQUIRE(rmp::ui::detail::bounds_of(label, 0, 0, &box));
    return Clay_Vector2{ box.x + box.width / 2, box.y + box.height / 2 };
}

// One frame with the pointer pressed at `where`, one with it up again.
void click(Clay_Vector2 where) {
    feed.pointer = where;
    feed.down = true;
    run_frame();
    feed.down = false;
    run_frame();
    feed.pointer = Clay_Vector2{ -1.0f, -1.0f };
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
                      "what the scene read through rmp::input, by name or "
                      "by key, is what decides") {
        // Jump on Space and moving are a game; a pause on Escape and reading
        // nothing are not; Space read by key, never routed, is a game too.
        const auto cases =
            std::to_array<std::pair<Read, bool>>({ { Read::JUMP, true },
                                                   { Read::MOVE, true },
                                                   { Read::RAW_SPACE, true },
                                                   { Read::PAUSE, false },
                                                   { Read::NOTHING, false } });
        for (const auto &[read, playing] : cases) {
            play(read);
            CHECK_MESSAGE(game.playing == playing, "read ", static_cast<int>(read));
            rmp::scenes::detail::shutdown();
        }
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

    TEST_CASE_FIXTURE(HeadlessUi, "a HUD over a game being played takes no focus") {
        play(Read::JUMP);
        CHECK(game.playing);
        CHECK(rmp::ui::focused().empty());
        CHECK_FALSE(rmp::ui::wants_keyboard());
        rmp::scenes::detail::shutdown();
        // The same screen over a game that reads nothing is a menu: its first
        // control takes the focus, as every screen's always has.
        play(Read::NOTHING);
        CHECK_FALSE(game.playing);
        CHECK(rmp::ui::focused() == "Map");
    }

    TEST_CASE_FIXTURE(HeadlessUi, "Enter, Space and the A button press nothing in it") {
        play(Read::JUMP);
        feed.devices.keys[KEY_SPACE] = true;
        press(SPACE);
        feed.devices.keys[KEY_SPACE] = false;
        press(ENTER);
        CHECK(game.map == 0);
        CHECK(game.bag == 0);
        // Space was the game's: the jump was heard.
        CHECK(game.jumps == 1);
        rmp::scenes::detail::shutdown();
        // And in a menu Enter presses the first button, so the test above was
        // able to fail.
        play(Read::NOTHING);
        press(ENTER);
        CHECK(game.map == 1);
    }

    TEST_CASE_FIXTURE(HeadlessUi, "the arrows and the d-pad do not walk into it") {
        play(Read::MOVE);
        press(DOWN);
        CHECK(rmp::ui::focused().empty());
        press(NavState{ .y = -1 });
        CHECK(rmp::ui::focused().empty());
        press(NavState{ .x = 1 });
        CHECK(rmp::ui::focused().empty());
        CHECK_FALSE(rmp::ui::wants_keyboard());
    }

    TEST_CASE_FIXTURE(HeadlessUi, "Tab walks in, shows the ring and takes the keys") {
        play(Read::JUMP);
        CHECK_FALSE(rmp::ui::detail::focus_visible());
        press(TAB);
        CHECK(rmp::ui::focused() == "Map");
        CHECK(rmp::ui::detail::focus_visible());
        CHECK(rmp::ui::wants_keyboard());
        CHECK(rmp::input::consumed_keyboard());
        // Space is the HUD's now: it presses Map, and the game does not jump --
        // by name it hears nothing; by key it still can.
        feed.devices.keys[KEY_SPACE] = true;
        press(SPACE);
        CHECK(game.map == 1);
        CHECK(game.jumps == 0);
        CHECK(rmp::input::pressed(KEY_SPACE));
        feed.devices.keys[KEY_SPACE] = false;
        run_frame();
        // Inside, the arrows walk it as they walk a menu.
        press(DOWN);
        CHECK(rmp::ui::focused() == "Bag");
        CHECK(rmp::ui::wants_keyboard());
        // And Shift+Tab walks back.
        press(NavState{ .y = -1, .tab = true });
        CHECK(rmp::ui::focused() == "Map");
        // The game is still being played the whole time: what it asks is
        // noted whether or not it is answered.
        CHECK(game.playing);
    }

    TEST_CASE_FIXTURE(HeadlessUi, "Escape and the B button give the keys back") {
        play(Read::JUMP);
        press(TAB);
        REQUIRE(rmp::ui::wants_keyboard());
        // Escape leaves the HUD before it is the game's pause.
        feed.devices.keys[KEY_ESCAPE] = true;
        press(ESCAPE);
        feed.devices.keys[KEY_ESCAPE] = false;
        CHECK(rmp::ui::focused().empty());
        CHECK_FALSE(rmp::ui::wants_keyboard());
        CHECK(game.pauses == 0);
        run_frame();
        feed.devices.keys[KEY_ESCAPE] = true;
        run_frame();
        feed.devices.keys[KEY_ESCAPE] = false;
        CHECK(game.pauses == 1);
        // And once out, Space is a jump again and presses nothing.
        feed.devices.keys[KEY_SPACE] = true;
        press(SPACE);
        feed.devices.keys[KEY_SPACE] = false;
        CHECK(game.jumps == 1);
        CHECK(game.map == 0);
    }

    TEST_CASE_FIXTURE(
        HeadlessUi, "a click elsewhere gives the keys back, and a click on it does not") {
        play(Read::JUMP);
        press(TAB);
        REQUIRE(rmp::ui::focused() == "Map");
        click(Clay_Vector2{ 20, 700 });
        CHECK(rmp::ui::focused().empty());
        CHECK_FALSE(rmp::ui::wants_keyboard());

        press(TAB);
        REQUIRE(rmp::ui::focused() == "Map");
        click(centre_of("Map"));
        CHECK(game.map == 1); // the click pressed it
        CHECK(rmp::ui::focused() == "Map");
        CHECK(rmp::ui::wants_keyboard());
        // And the ring stays: a click hides it, but these keys are still the
        // HUD's, and a player whose W moves nothing has to see where they went.
        CHECK(rmp::ui::detail::focus_visible());

        // A click on it keeps the keys for that click only: the next one
        // elsewhere still gives them back.
        click(Clay_Vector2{ 20, 700 });
        CHECK(rmp::ui::focused().empty());
        CHECK_FALSE(rmp::ui::wants_keyboard());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a click on a HUD's button presses it, and the keys stay "
                      "the game's") {
        // A touch Pause in a HUD: a finger presses it without walking into the
        // HUD, so Space after it is still a jump and presses nothing.
        play(Read::JUMP);
        click(centre_of("Map"));
        CHECK(game.map == 1);
        CHECK_FALSE(rmp::ui::wants_keyboard());
        CHECK_FALSE(rmp::input::consumed_keyboard());
        feed.devices.keys[KEY_SPACE] = true;
        press(SPACE);
        feed.devices.keys[KEY_SPACE] = false;
        CHECK(game.jumps == 1);
        CHECK(game.map == 1);
        CHECK(rmp::ui::focused().empty());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a click into a HUD's text field types, and Enter gives "
                      "the keys back") {
        // Typing is having the keyboard, not the focus: the field types with
        // no focus resting in the HUD, and Space is a character, not a jump.
        play(Read::JUMP);
        game.chat = true;
        run_frames(2);
        REQUIRE(rmp::ui::focused().empty());
        click(centre_of("Say"));
        CHECK(rmp::ui::wants_keyboard());
        feed.devices.keys[KEY_SPACE] = true;
        press(SPACE);
        feed.devices.keys[KEY_SPACE] = false;
        CHECK(game.jumps == 0);
        CHECK(game.map == 0);
        CHECK(rmp::ui::wants_keyboard());
        press(ENTER);
        CHECK_FALSE(rmp::ui::wants_keyboard());
        CHECK(rmp::ui::focused().empty());
        run_frame();
        feed.devices.keys[KEY_SPACE] = true;
        press(SPACE);
        feed.devices.keys[KEY_SPACE] = false;
        CHECK(game.jumps == 1);
        CHECK(game.map == 0);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a pause menu over it focuses Resume, and popping it "
                      "hands the HUD nothing") {
        play(Read::JUMP);
        rmp::Scene::push<Pause>();
        rmp::scenes::detail::apply_pending();
        run_frames(2);
        CHECK(rmp::ui::focused() == "Resume");
        CHECK_FALSE(rmp::ui::wants_keyboard());
        press(ENTER);
        CHECK(game.resumed == 1);
        REQUIRE(rmp::Scene::depth() == 1);
        run_frame();
        CHECK(rmp::ui::focused().empty());
        CHECK_FALSE(rmp::ui::wants_keyboard());
        press(ENTER);
        CHECK(game.map == 0);
        CHECK(game.bag == 0);
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a pause menu pushed while the HUD has the keys takes "
                      "them") {
        play(Read::JUMP);
        press(TAB);
        REQUIRE(rmp::ui::wants_keyboard());
        rmp::Scene::push<Pause>();
        rmp::scenes::detail::apply_pending();
        run_frames(2);
        CHECK(rmp::ui::focused() == "Resume");
        // A menu's focus: the keys are not the interface's, so the pause menu's
        // own Escape is heard.
        CHECK_FALSE(rmp::ui::wants_keyboard());
        CHECK_FALSE(rmp::input::consumed_keyboard());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a menu over a game that goes on under it: the arrows stay in "
                      "the menu, and Tab walks into the HUD and on out of it") {
        play(Read::JUMP);
        rmp::Scene::push<Inventory>();
        rmp::scenes::detail::apply_pending();
        run_frames(2);
        // Still played underneath, so its HUD takes nothing, though input
        // reaches it; the menu is a menu, and its first control has the focus.
        CHECK(game.playing);
        CHECK(rmp::ui::focused() == "Use");
        // Each a press of its own: a frame with it down, and one with it up.
        const auto tap = [](const NavState &nav) {
            press(nav);
            run_frame();
        };
        // The arrows wrap inside the menu, either way, past the HUD.
        tap(DOWN);
        CHECK(rmp::ui::focused() == "Drop");
        tap(DOWN);
        CHECK(rmp::ui::focused() == "Use");
        tap(NavState{ .y = -1 });
        CHECK(rmp::ui::focused() == "Drop");
        CHECK_FALSE(rmp::ui::wants_keyboard());
        // Tab is the way in, and in there the keys are the HUD's...
        tap(TAB);
        CHECK(rmp::ui::focused() == "Map");
        CHECK(rmp::ui::wants_keyboard());
        tap(DOWN);
        CHECK(rmp::ui::focused() == "Bag");
        CHECK(rmp::ui::wants_keyboard());
        // ...until the arrows walk on out of it, into the menu.
        tap(DOWN);
        CHECK(rmp::ui::focused() == "Use");
        CHECK_FALSE(rmp::ui::wants_keyboard());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "turning navigation off gives the keys back, for good") {
        play(Read::JUMP);
        press(TAB);
        REQUIRE(rmp::ui::wants_keyboard());
        rmp::ui::set_navigation_enabled(false);
        CHECK_FALSE(rmp::ui::wants_keyboard());
        run_frame();
        CHECK_FALSE(rmp::ui::wants_keyboard());
        // On again, the HUD is as it was before Tab: nobody walked back into
        // it, so it does not take the keys back by itself, and Space is a jump.
        rmp::ui::set_navigation_enabled(true);
        CHECK_FALSE(rmp::ui::wants_keyboard());
        run_frame();
        CHECK_FALSE(rmp::ui::wants_keyboard());
        CHECK(rmp::ui::focused().empty());
        feed.devices.keys[KEY_SPACE] = true;
        press(SPACE);
        feed.devices.keys[KEY_SPACE] = false;
        CHECK(game.jumps == 1);
        CHECK(game.map == 0);
        rmp::scenes::detail::shutdown();

        // A menu's focus is not the keys, and it is kept as it always was --
        // on its second control, where a menu's own default would not put it.
        play(Read::NOTHING);
        press(DOWN);
        REQUIRE(rmp::ui::focused() == "Bag");
        rmp::ui::set_navigation_enabled(false);
        run_frame();
        rmp::ui::set_navigation_enabled(true);
        run_frame();
        CHECK(rmp::ui::focused() == "Bag");
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "a focus it had before the game was played is dropped when it is") {
        play(Read::NOTHING);
        REQUIRE(rmp::ui::focused() == "Map");
        // The first frame the scene reads Space is a frame Space must not press
        // the button that had the focus a frame ago.
        game.read = Read::JUMP;
        feed.devices.keys[KEY_SPACE] = true;
        press(SPACE);
        feed.devices.keys[KEY_SPACE] = false;
        CHECK(game.map == 0);
        CHECK(game.jumps == 1);
        CHECK(rmp::ui::focused().empty());
        CHECK_FALSE(rmp::ui::wants_keyboard());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "focus_first = false makes a menu a HUD, and true "
                      "makes a HUD a menu") {
        // A game that reads raylib's keys itself is never noticed: false says
        // what the guess could not.
        play(Read::NOTHING, false);
        CHECK(rmp::ui::focused().empty());
        press(DOWN);
        CHECK(rmp::ui::focused().empty());
        press(ENTER);
        CHECK(game.map == 0);
        press(TAB);
        CHECK(rmp::ui::focused() == "Map");
        CHECK(rmp::ui::wants_keyboard());
        rmp::scenes::detail::shutdown();

        play(Read::JUMP, true);
        CHECK(rmp::ui::focused() == "Map");
        press(DOWN);
        CHECK(rmp::ui::focused() == "Bag");
        press(ENTER);
        CHECK(game.bag == 1);
        // Asked for, so it is a menu: it does not take the game's keys.
        CHECK_FALSE(rmp::ui::wants_keyboard());
    }

    TEST_CASE_FIXTURE(HeadlessUi, "focus() on a HUD's control is the game choosing it") {
        play(Read::JUMP);
        rmp::ui::focus("Bag");
        run_frame();
        CHECK(rmp::ui::focused() == "Bag");
        CHECK(rmp::ui::wants_keyboard());
        press(ESCAPE);
        CHECK(rmp::ui::focused().empty());
        // And focus() on a menu's control does not take the game's keys.
        rmp::scenes::detail::shutdown();
        play(Read::NOTHING);
        rmp::ui::focus("Bag");
        run_frame();
        CHECK(rmp::ui::focused() == "Bag");
        CHECK_FALSE(rmp::ui::wants_keyboard());
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "without scenes, what the frame read decides, and only "
                      "that frame") {
        const auto frame = [](bool read) {
            rmp::input::detail::begin_frame();
            rmp::ui::detail::begin_frame();
            if (read) (void)rmp::input::pressed("jump");
            rmp::ui::begin({ .placement = rmp::ui::Align::TOP_RIGHT });
            if (rmp::ui::button("Map")) ++game.map;
            rmp::ui::button("Bag");
            rmp::ui::end();
            rmp::ui::detail::end_frame();
        };
        game = {};
        frame(true);
        frame(true);
        CHECK(rmp::ui::focused().empty());
        feed.nav = SPACE;
        frame(true);
        feed.nav = NavState{};
        CHECK(game.map == 0);
        // A frame that reads nothing is a menu's.
        frame(false);
        frame(false);
        CHECK(rmp::ui::focused() == "Map");
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "without scenes, every pass of the frame is judged on what "
                      "the frame read") {
        // Two begin()/end() pairs in one frame the runner marked: the second
        // HUD is as much over the game as the first.
        const auto frame = [](bool read) {
            rmp::input::detail::begin_frame();
            rmp::ui::detail::begin_frame();
            if (read) (void)rmp::input::pressed("jump");
            rmp::ui::begin({ .placement = rmp::ui::Align::TOP_RIGHT });
            rmp::ui::button("Map");
            rmp::ui::end();
            rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
            rmp::ui::button("Bag");
            rmp::ui::end();
            rmp::ui::detail::end_frame();
        };
        frame(true);
        frame(true);
        CHECK(rmp::ui::focused().empty());
        frame(false);
        frame(false);
        CHECK(rmp::ui::focused() == "Map");
    }

    TEST_CASE_FIXTURE(HeadlessUi,
                      "without rmp::app, the note is forgotten at the end of "
                      "the frame") {
        // A plain loop: no devices sampled, begin() marks the frame and end()
        // closes it. A read in one frame is not the next one's.
        const auto frame = [](bool read) {
            if (read) rmp::ui::detail::note_game_keys();
            rmp::ui::begin();
            rmp::ui::button("Map");
            rmp::ui::end();
        };
        frame(true);
        frame(true);
        CHECK(rmp::ui::focused().empty());
        frame(false);
        frame(false);
        CHECK(rmp::ui::focused() == "Map");
    }

    TEST_CASE_FIXTURE(HeadlessUi, "after shutdown() the focus remembers nothing") {
        // Each thing the focus keeps, made true, the UI shut down and started
        // again: none of it may come back.
        //
        // A HUD walked into with Tab has the keys.
        play(Read::JUMP);
        press(TAB);
        REQUIRE(rmp::ui::wants_keyboard());
        rmp::scenes::detail::shutdown();
        rmp::ui::shutdown();
        CHECK(rmp::ui::focused().empty());
        CHECK_FALSE(rmp::ui::wants_keyboard());

        // A text field typing.
        std::string text;
        const auto field = [&text] {
            rmp::ui::begin();
            rmp::ui::text_input("Name", text);
            rmp::ui::end();
        };
        field();
        field();
        REQUIRE(rmp::ui::focused() == "Name");
        feed.nav = ENTER;
        field();
        feed.nav = NavState{};
        REQUIRE(rmp::ui::wants_keyboard());
        rmp::ui::shutdown();
        CHECK_FALSE(rmp::ui::wants_keyboard());
        rmp::ui::begin({ .focus_first = false });
        rmp::ui::text_input("Name", text);
        rmp::ui::end();
        CHECK_FALSE(rmp::ui::wants_keyboard());
        CHECK(rmp::ui::focused().empty());

        // A name asked for between frames, and the controls the last frame had:
        // a step on the first frame after walks this frame's, of which there are
        // none yet, and the name is not taken up.
        for (int frame = 0; frame < 2; ++frame) {
            rmp::ui::begin();
            rmp::ui::button("A");
            rmp::ui::button("B");
            rmp::ui::end();
        }
        rmp::ui::focus("Other");
        rmp::ui::shutdown();
        CHECK(rmp::ui::focused().empty());
        feed.nav = DOWN;
        rmp::ui::begin({ .focus_first = false });
        rmp::ui::button("Other");
        const std::string during(rmp::ui::focused());
        rmp::ui::end();
        feed.nav = NavState{};
        CHECK(during.empty());

        // A dropdown left open, walked to its second item.
        int size = 0;
        const auto sizes = [&size] {
            rmp::ui::begin();
            rmp::ui::dropdown("Size", size, { "S", "M", "L" });
            rmp::ui::end();
        };
        sizes();
        sizes();
        REQUIRE(rmp::ui::focused() == "Size");
        feed.nav = ENTER;
        sizes(); // opens it
        feed.nav = DOWN;
        sizes(); // walks to "M"
        feed.nav = NavState{};
        rmp::ui::shutdown();
        sizes();
        feed.nav = ENTER;
        sizes(); // opens it again, from what it shows; it does not pick "M"
        feed.nav = NavState{};
        CHECK(size == 0);
        feed.nav = ENTER;
        sizes(); // and picks what it shows
        feed.nav = NavState{};
        CHECK(size == 0);
    }
}
