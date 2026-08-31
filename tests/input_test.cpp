// ===========================================================================
// rmp::input.
//
// The easiest layer in the framework to test without a window, because input
// already has to come through a seam to be injectable at all: the test writes a
// DeviceState by hand and every function answers from it. There is no raylib
// underneath any of this and no device attached to the machine running it.
//
// The stance here is deliberately hostile. Key codes out of range, actions that
// were never defined, more bindings than the table holds, sticks pushed to the
// wrong half, a dead zone of 1.0 — none of these are things a careful user
// does, and all of them are things a config file, a refactor or a typo can
// produce. A check that only ever sees plausible input has not been tested.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/ui/internal.h"

#include <rmp/input.h>
#include <rmp/ui.h>

#include <cmath>

namespace {

// The device state the fake provider hands over. Tests write it directly, which
// is the whole seam: there is nothing between this and what rmp::input answers.
rmp::input::detail::DeviceState g_devices;

void fake_sample(rmp::input::detail::DeviceState *out) { *out = g_devices; }

// Every test starts from nothing: no actions, no history, the factory set
// reinstalled on first use, and the fake provider in place.
struct Fixture {
    Fixture() {
        rmp::input::detail::reset();
        g_devices = rmp::input::detail::DeviceState{};
        rmp::input::detail::set_sample_provider(fake_sample);
    }
    ~Fixture() {
        rmp::input::detail::reset();
        g_devices = rmp::input::detail::DeviceState{};
    }
};

// One turn of the loop. Everything about edges — just_pressed, just_released —
// is a difference between two of these, so a test that forgets to call it twice
// is testing the wrong thing.
void frame() { rmp::input::detail::begin_frame(); }

void hold(::KeyboardKey key, bool down = true) { g_devices.keys[key] = down; }
void hold(::MouseButton button, bool down = true) { g_devices.mouse[button] = down; }
void hold(::GamepadButton button, bool down = true) { g_devices.pad[button] = down; }
void push(::GamepadAxis axis, float value) { g_devices.axes[axis] = value; }

float length(Vector2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

} // namespace

TEST_SUITE("input actions") {
    TEST_CASE("an action answers to every binding it was given, and to nothing else") {
        Fixture fix;
        rmp::input::action("jump", KEY_SPACE, KEY_W, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);

        frame();
        CHECK(!rmp::input::pressed("jump"));

        for (int which = 0; which < 3; which++) {
            g_devices = rmp::input::detail::DeviceState{};
            if (which == 0) hold(KEY_SPACE);
            if (which == 1) hold(KEY_W);
            if (which == 2) hold(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
            frame();
            CHECK_MESSAGE(rmp::input::pressed("jump"), "binding ", which,
                          " did not register");
        }

        // And nothing else: a neighbouring key is not a near miss, it is a miss.
        g_devices = rmp::input::detail::DeviceState{};
        hold(KEY_A);
        hold(KEY_E);
        hold(MOUSE_BUTTON_LEFT);
        hold(GAMEPAD_BUTTON_RIGHT_FACE_UP);
        frame();
        CHECK(!rmp::input::pressed("jump"));
    }

    TEST_CASE("just_pressed is true for exactly one frame out of ten held") {
        Fixture fix;
        rmp::input::action("fire", KEY_F);

        frame(); // nothing held
        hold(KEY_F);

        int edges = 0;
        for (int i = 0; i < 10; i++) {
            frame();
            if (rmp::input::just_pressed("fire")) edges++;
            CHECK(rmp::input::pressed("fire")); // held, all ten
        }
        CHECK(edges == 1);
    }

    TEST_CASE("just_released is the other edge, and also exactly once") {
        Fixture fix;
        rmp::input::action("fire", KEY_F);

        hold(KEY_F);
        frame();
        frame();
        CHECK(!rmp::input::just_released("fire"));

        hold(KEY_F, false);
        int edges = 0;
        for (int i = 0; i < 5; i++) {
            frame();
            if (rmp::input::just_released("fire")) edges++;
        }
        CHECK(edges == 1);
    }

    TEST_CASE("the very first frame has no edges, because there is no frame before it") {
        Fixture fix;
        rmp::input::action("fire", KEY_F);
        hold(KEY_F);
        frame();
        // Held, yes. But "just pressed" would be a claim about a frame that never
        // happened, and a game that spawns with a key down should not fire.
        CHECK(rmp::input::pressed("fire"));
        CHECK(!rmp::input::just_pressed("fire"));
    }

    TEST_CASE("redefining an action replaces it and does not accumulate") {
        Fixture fix;
        rmp::input::action("fire", KEY_F);
        rmp::input::action("fire", KEY_G);

        hold(KEY_F);
        frame();
        CHECK(!rmp::input::pressed("fire"));

        g_devices = rmp::input::detail::DeviceState{};
        hold(KEY_G);
        frame();
        CHECK(rmp::input::pressed("fire"));
    }

    TEST_CASE("defining the same action twenty times is what a scene _ready does") {
        Fixture fix;
        for (int i = 0; i < 20; i++) rmp::input::action("fire", KEY_F);
        hold(KEY_F);
        frame();
        CHECK(rmp::input::pressed("fire"));
    }

    TEST_CASE("an action nobody defined reads as false rather than crashing") {
        Fixture fix;
        frame();
        CHECK(!rmp::input::pressed("nonexistent"));
        CHECK(!rmp::input::just_pressed("nonexistent"));
        CHECK(!rmp::input::just_released("nonexistent"));
        // And again, to exercise the "warn once" path rather than only its first
        // branch — sixty warnings a second is the same as no warning at all.
        CHECK(!rmp::input::pressed("nonexistent"));
    }

    TEST_CASE("more bindings than the table holds is truncated, not overflowed") {
        Fixture fix;
        rmp::input::action("many", KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H,
                           KEY_I, KEY_J, KEY_K, KEY_L);
        hold(KEY_A);
        frame();
        CHECK(rmp::input::pressed("many"));
        // The ones past the limit are gone, and the point of the test is that
        // asking about them is safe rather than a read past the end of the array.
        g_devices = rmp::input::detail::DeviceState{};
        hold(KEY_L);
        frame();
        CHECK(!rmp::input::pressed("many"));
    }

    TEST_CASE("an empty name is a name, and behaves like any other unknown one") {
        Fixture fix;
        frame();
        CHECK(!rmp::input::pressed(""));
        rmp::input::action("", KEY_Z);
        hold(KEY_Z);
        frame();
        CHECK(rmp::input::pressed(""));
    }

    TEST_CASE("reading by device is raw and never routed") {
        Fixture fix;
        hold(KEY_W);
        hold(MOUSE_BUTTON_LEFT);
        hold(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        frame();
        CHECK(rmp::input::pressed(KEY_W));
        CHECK(rmp::input::pressed(MOUSE_BUTTON_LEFT));
        CHECK(rmp::input::pressed(GAMEPAD_BUTTON_RIGHT_FACE_DOWN));
        CHECK(!rmp::input::pressed(KEY_S));
    }

} // TEST_SUITE

TEST_SUITE("input directions") {
    TEST_CASE("axis is exactly -1, 0 and +1, and both at once is 0") {
        Fixture fix;
        frame();
        CHECK(rmp::input::axis() == doctest::Approx(0.0f));

        hold(KEY_A);
        frame();
        CHECK(rmp::input::axis() == doctest::Approx(-1.0f));

        g_devices = rmp::input::detail::DeviceState{};
        hold(KEY_D);
        frame();
        CHECK(rmp::input::axis() == doctest::Approx(1.0f));

        // Both. Somebody mashing the keyboard should stand still, not drift.
        hold(KEY_A);
        frame();
        CHECK(rmp::input::axis() == doctest::Approx(0.0f));
    }

    TEST_CASE("a diagonal has length 1, not 1.41") {
        Fixture fix;
        hold(KEY_D);
        hold(KEY_S);
        frame();

        const Vector2 v = rmp::input::vector();
        // THE test of this phase. Without the normalisation, holding two keys moves
        // you 41 % faster than holding one, which is the single most common bug in
        // 2D movement code and is invisible until somebody notices the diagonal is
        // the fast way to travel.
        CHECK(length(v) == doctest::Approx(1.0f).epsilon(0.001));
        CHECK(v.x == doctest::Approx(v.y));
        CHECK(v.x > 0.0f);
    }

    TEST_CASE("all four diagonals, and all four straights") {
        Fixture fix;
        const struct {
            ::KeyboardKey a;
            ::KeyboardKey b;
        } pairs[] = {
            { KEY_W, KEY_A }, { KEY_W, KEY_D }, { KEY_S, KEY_A }, { KEY_S, KEY_D }
        };
        for (const auto &pair : pairs) {
            g_devices = rmp::input::detail::DeviceState{};
            hold(pair.a);
            hold(pair.b);
            frame();
            CHECK(length(rmp::input::vector()) == doctest::Approx(1.0f).epsilon(0.001));
        }
        for (::KeyboardKey key : { KEY_W, KEY_A, KEY_S, KEY_D }) {
            g_devices = rmp::input::detail::DeviceState{};
            hold(key);
            frame();
            CHECK(length(rmp::input::vector()) == doctest::Approx(1.0f).epsilon(0.001));
        }
    }

    TEST_CASE("up is negative Y, which is what raylib and Object::position agree on") {
        Fixture fix;
        hold(KEY_W);
        frame();
        CHECK(rmp::input::vector().y < 0.0f);

        g_devices = rmp::input::detail::DeviceState{};
        hold(KEY_S);
        frame();
        CHECK(rmp::input::vector().y > 0.0f);
    }

    TEST_CASE("no direction at all is exactly zero, not a tiny drift") {
        Fixture fix;
        frame();
        const Vector2 v = rmp::input::vector();
        CHECK(v.x == 0.0f);
        CHECK(v.y == 0.0f);
    }

    TEST_CASE("opposite keys cancel in both axes at once") {
        Fixture fix;
        hold(KEY_W);
        hold(KEY_S);
        hold(KEY_A);
        hold(KEY_D);
        frame();
        CHECK(length(rmp::input::vector()) == doctest::Approx(0.0f));
    }

    TEST_CASE("a stick pushed halfway stays halfway") {
        Fixture fix;
        rmp::input::set_deadzone(0.0f);
        push(GAMEPAD_AXIS_LEFT_X, 0.5f);
        frame();
        // Normalisation only shortens; it must not stretch a gentle push into a
        // full-speed run, which is what clamping the length to exactly 1 would do.
        CHECK(rmp::input::vector().x == doctest::Approx(0.5f).epsilon(0.01));
        CHECK(length(rmp::input::vector()) == doctest::Approx(0.5f).epsilon(0.01));
    }

    TEST_CASE("a stick pushed into a corner is still length 1") {
        Fixture fix;
        rmp::input::set_deadzone(0.0f);
        push(GAMEPAD_AXIS_LEFT_X, 1.0f);
        push(GAMEPAD_AXIS_LEFT_Y, 1.0f);
        frame();
        CHECK(length(rmp::input::vector()) == doctest::Approx(1.0f).epsilon(0.001));
    }

    TEST_CASE("axis takes two action names, and they can be any actions") {
        Fixture fix;
        rmp::input::action("brake", KEY_Q);
        rmp::input::action("boost", KEY_E);
        hold(KEY_E);
        frame();
        CHECK(rmp::input::axis("brake", "boost") == doctest::Approx(1.0f));
        hold(KEY_Q);
        frame();
        CHECK(rmp::input::axis("brake", "boost") == doctest::Approx(0.0f));
    }

    TEST_CASE("the factory actions exist before anybody defines anything") {
        Fixture fix;
        for (const char *name : { "move_left", "move_right", "move_up", "move_down",
                                  "ui_accept", "ui_cancel" }) {
            g_devices = rmp::input::detail::DeviceState{};
            frame();
            CHECK_MESSAGE(!rmp::input::pressed(name), name, " should be up");
        }
        hold(KEY_ENTER);
        frame();
        CHECK(rmp::input::pressed("ui_accept"));
        g_devices = rmp::input::detail::DeviceState{};
        hold(KEY_ESCAPE);
        frame();
        CHECK(rmp::input::pressed("ui_cancel"));
    }

    TEST_CASE("a factory action can be redefined like any other") {
        Fixture fix;
        rmp::input::action("move_left", KEY_H);
        hold(KEY_A); // the old binding
        frame();
        CHECK(!rmp::input::pressed("move_left"));
        g_devices = rmp::input::detail::DeviceState{};
        hold(KEY_H);
        frame();
        CHECK(rmp::input::pressed("move_left"));
    }

    TEST_CASE("keyboard, d-pad and stick all reach the same action") {
        Fixture fix;
        for (int which = 0; which < 3; which++) {
            g_devices = rmp::input::detail::DeviceState{};
            rmp::input::set_deadzone(0.2f);
            if (which == 0) hold(KEY_D);
            if (which == 1) hold(GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
            if (which == 2) push(GAMEPAD_AXIS_LEFT_X, 1.0f);
            frame();
            CHECK_MESSAGE(rmp::input::pressed("move_right"), "device ", which);
            CHECK(rmp::input::vector().x > 0.5f);
        }
    }

} // TEST_SUITE

TEST_SUITE("input dead zone") {
    TEST_CASE("it clips the stick") {
        Fixture fix;
        rmp::input::set_deadzone(0.5f);
        push(GAMEPAD_AXIS_LEFT_X, 0.4f);
        frame();
        CHECK(!rmp::input::pressed("move_right"));
        CHECK(rmp::input::vector().x == doctest::Approx(0.0f));

        push(GAMEPAD_AXIS_LEFT_X, 0.6f);
        frame();
        CHECK(rmp::input::pressed("move_right"));
    }

    TEST_CASE("and does NOT clip the keyboard") {
        Fixture fix;
        // A key has no travel to ignore. Applying the dead zone to it would make a
        // large one turn the keyboard off, which is a bug with a very confusing
        // symptom: the game works with a controller and not without one.
        rmp::input::set_deadzone(0.95f);
        hold(KEY_D);
        frame();
        CHECK(rmp::input::pressed("move_right"));
        CHECK(rmp::input::vector().x == doctest::Approx(1.0f));
    }

    TEST_CASE("just past the dead zone reads a little, not a jump") {
        Fixture fix;
        rmp::input::set_deadzone(0.2f);
        push(GAMEPAD_AXIS_LEFT_X, 0.21f);
        frame();
        const float x = rmp::input::vector().x;
        // Rescaled, not clipped. Without the rescale every character starts moving
        // at a fifth of full speed the instant the stick wakes up.
        CHECK(x > 0.0f);
        CHECK(x < 0.05f);
    }

    TEST_CASE("a fully pushed stick still reads 1 whatever the dead zone") {
        Fixture fix;
        for (float dz : { 0.0f, 0.2f, 0.5f, 0.9f }) {
            rmp::input::set_deadzone(dz);
            push(GAMEPAD_AXIS_LEFT_X, 1.0f);
            frame();
            CHECK_MESSAGE(rmp::input::vector().x == doctest::Approx(1.0f).epsilon(0.001),
                          "deadzone ", dz);
        }
    }

    TEST_CASE("it is symmetric") {
        Fixture fix;
        rmp::input::set_deadzone(0.3f);
        push(GAMEPAD_AXIS_LEFT_X, -1.0f);
        frame();
        CHECK(rmp::input::vector().x == doctest::Approx(-1.0f).epsilon(0.001));
        CHECK(rmp::input::pressed("move_left"));
        CHECK(!rmp::input::pressed("move_right"));
    }

    TEST_CASE("a stick on the wrong half does not trigger the other direction") {
        Fixture fix;
        rmp::input::set_deadzone(0.1f);
        push(GAMEPAD_AXIS_LEFT_X, -1.0f);
        frame();
        CHECK(!rmp::input::pressed("move_right"));
    }

    TEST_CASE("out-of-range dead zones are clamped rather than refused") {
        Fixture fix;
        rmp::input::set_deadzone(-5.0f);
        CHECK(rmp::input::deadzone() == doctest::Approx(0.0f));
        rmp::input::set_deadzone(50.0f);
        CHECK(rmp::input::deadzone() <= 0.95f);
        // A settings slider reaches this. One whose ends wedge the sticks off is
        // worse than one whose ends do nothing.
        push(GAMEPAD_AXIS_LEFT_X, 1.0f);
        frame();
        CHECK(rmp::input::vector().x > 0.0f);
    }

    TEST_CASE("stick() clamps its sign to one of two halves") {
        Fixture fix;
        rmp::input::set_deadzone(0.0f);
        rmp::input::action("weird", rmp::input::stick(GAMEPAD_AXIS_RIGHT_X, 0));
        rmp::input::action("weirder", rmp::input::stick(GAMEPAD_AXIS_RIGHT_X, -7));

        push(GAMEPAD_AXIS_RIGHT_X, 1.0f);
        frame();
        CHECK(rmp::input::pressed("weird")); // 0 became +1
        CHECK(!rmp::input::pressed("weirder")); // -7 became -1

        push(GAMEPAD_AXIS_RIGHT_X, -1.0f);
        frame();
        CHECK(!rmp::input::pressed("weird"));
        CHECK(rmp::input::pressed("weirder"));
    }

} // TEST_SUITE

TEST_SUITE("input consumption") {
    TEST_CASE("a layer input cannot reach hears nothing by name, but raw still works") {
        Fixture fix;
        rmp::input::action("fire", KEY_F, MOUSE_BUTTON_LEFT);
        hold(KEY_F);
        hold(MOUSE_BUTTON_LEFT);
        frame();
        CHECK(rmp::input::pressed("fire"));

        rmp::input::detail::set_layer_input(false);
        CHECK(!rmp::input::pressed("fire"));
        CHECK(!rmp::input::just_pressed("fire"));
        CHECK(rmp::input::consumed_pointer());
        CHECK(rmp::input::consumed_keyboard());

        // Raw is raw. "I want to know regardless of who else is listening" is a
        // real question and this is its answer.
        CHECK(rmp::input::pressed(KEY_F));
        CHECK(rmp::input::pressed(MOUSE_BUTTON_LEFT));

        rmp::input::detail::set_layer_input(true);
        CHECK(rmp::input::pressed("fire"));
    }

    TEST_CASE("the frame boundary puts the layer back") {
        Fixture fix;
        rmp::input::detail::set_layer_input(false);
        frame();
        // Otherwise one scene setting it would silence the whole next frame, which
        // is a bug that looks like "input stopped working" and has no obvious cause.
        CHECK(rmp::input::detail::layer_input());
    }

    TEST_CASE("axis and vector go quiet too, not just pressed()") {
        Fixture fix;
        hold(KEY_D);
        frame();
        CHECK(rmp::input::axis() == doctest::Approx(1.0f));
        rmp::input::detail::set_layer_input(false);
        CHECK(rmp::input::axis() == doctest::Approx(0.0f));
        CHECK(length(rmp::input::vector()) == doctest::Approx(0.0f));
    }

} // TEST_SUITE

TEST_SUITE("input pointer") {
    TEST_CASE("the delta is zero on the first frame") {
        Fixture fix;
        g_devices.pointer = Vector2{ 100, 100 };
        frame();
        // There is no previous position, and inventing one would report a jump from
        // the origin the moment the game starts.
        CHECK(rmp::input::pointer_delta().x == doctest::Approx(0.0f));
        CHECK(rmp::input::pointer_delta().y == doctest::Approx(0.0f));
    }

    TEST_CASE("and is the difference after that") {
        Fixture fix;
        g_devices.pointer = Vector2{ 100, 100 };
        frame();
        g_devices.pointer = Vector2{ 130, 90 };
        frame();
        CHECK(rmp::input::pointer_delta().x == doctest::Approx(30.0f));
        CHECK(rmp::input::pointer_delta().y == doctest::Approx(-10.0f));
        CHECK(rmp::input::pointer_screen().x == doctest::Approx(130.0f));
    }

    TEST_CASE("down, pressed and released are the three edges of one button") {
        Fixture fix;
        frame();
        CHECK(!rmp::input::pointer_down());

        hold(MOUSE_BUTTON_LEFT);
        frame();
        CHECK(rmp::input::pointer_down());
        CHECK(rmp::input::pointer_pressed());
        CHECK(!rmp::input::pointer_released());

        frame();
        CHECK(rmp::input::pointer_down());
        CHECK(!rmp::input::pointer_pressed()); // held, not pressed

        hold(MOUSE_BUTTON_LEFT, false);
        frame();
        CHECK(!rmp::input::pointer_down());
        CHECK(rmp::input::pointer_released());
    }

} // TEST_SUITE

TEST_SUITE("input consumption, against the real UI") {
    namespace {

    Clay_Dimensions measure_stub(Clay_StringSlice text, Clay_TextElementConfig *config,
                                 void * /*user*/) {
        const float size =
            config != nullptr ? static_cast<float>(config->fontSize) : 16.0f;
        return Clay_Dimensions{ static_cast<float>(text.length) * size * 0.5f, size };
    }

    // The UI reads the pointer through its own provider, and rmp::input through
    // its sample provider. They have to agree or the test is measuring nothing —
    // so both come from the same g_devices the test writes.
    void ui_pointer(Clay_Vector2 *position, bool *down) {
        *position = Clay_Vector2{ g_devices.pointer.x, g_devices.pointer.y };
        *down = g_devices.mouse[MOUSE_BUTTON_LEFT];
    }

    struct HeadlessUi {
        HeadlessUi() {
            rmp::ui::detail::set_measure_provider(measure_stub);
            rmp::ui::detail::set_pointer_provider(ui_pointer);
            rmp::ui::detail::set_test_viewport(800, 450);
        }
        ~HeadlessUi() {
            rmp::ui::detail::set_measure_provider(rmp::ui::detail::measure_with_raylib);
            rmp::ui::detail::set_pointer_provider(rmp::ui::detail::pointer_from_raylib);
            rmp::ui::detail::set_test_viewport(0, 0);
        }
    };

    // One whole frame the way rmp::app runs it: sample the devices, mark the UI
    // frame, describe a menu, close it. Two of these, because immediate-mode hit
    // testing answers for the layout of the previous frame.
    void frame_with_menu() {
        rmp::input::detail::begin_frame();
        rmp::ui::detail::begin_frame();
        rmp::ui::begin();
        rmp::ui::button("Play");
        rmp::ui::end();
        rmp::ui::detail::end_frame();
    }

    } // namespace

    TEST_CASE("a mouse action goes quiet under the pointer, and the key half does not") {
        Fixture fix;
        HeadlessUi headless;
        rmp::input::action("fire", KEY_F, MOUSE_BUTTON_LEFT);

        // Away from the menu: the pointer is the game's.
        g_devices.pointer = Vector2{ 5, 5 };
        hold(MOUSE_BUTTON_LEFT);
        hold(KEY_F);
        frame_with_menu();
        frame_with_menu();
        REQUIRE(!rmp::ui::wants_pointer());
        CHECK(rmp::input::pressed("fire"));

        // Over the button in the middle of the viewport. This is the case the whole
        // routing rule exists for: a finger that presses Play must not also fire.
        g_devices.pointer = Vector2{ 400, 225 };
        frame_with_menu();
        frame_with_menu();
        REQUIRE(rmp::ui::wants_pointer());
        CHECK(rmp::input::consumed_pointer());

        // The action is bound to BOTH a key and a mouse button. The key half is
        // untouched — the UI took the pointer, not the keyboard — so this still
        // reads true, and it is what stops "the UI ate my input" from meaning
        // "the game stopped responding".
        CHECK(rmp::input::pressed("fire"));

        hold(KEY_F, false);
        frame_with_menu();
        // Now only the mouse binding is left, and it is the one being consumed.
        CHECK(!rmp::input::pressed("fire"));

        // Raw, however, still sees it. Same button, same frame, different question.
        CHECK(rmp::input::pressed(MOUSE_BUTTON_LEFT));
    }

    TEST_CASE("a mouse-only action is silent over the UI and loud beside it") {
        Fixture fix;
        HeadlessUi headless;
        rmp::input::action("shoot", MOUSE_BUTTON_LEFT);
        hold(MOUSE_BUTTON_LEFT);

        g_devices.pointer = Vector2{ 400, 225 };
        frame_with_menu();
        frame_with_menu();
        CHECK(!rmp::input::pressed("shoot"));

        g_devices.pointer = Vector2{ 2, 2 };
        frame_with_menu();
        frame_with_menu();
        CHECK(rmp::input::pressed("shoot"));
    }

} // TEST_SUITE

TEST_SUITE("input, hostile values") {
    TEST_CASE("a key code past the end of the table does not read past it") {
        Fixture fix;
        frame();
        // Not something anyone types on purpose. It is something a cast, a saved
        // key-binding file or a newer raylib can produce, and the answer has to be
        // false rather than whatever byte happens to live there.
        //
        // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange): casting out
        // of range is the thing under test. The analyzer is right that ordinary
        // code must not do it — which is exactly why the framework has to hold
        // up when something does it anyway.
        CHECK(!rmp::input::pressed(static_cast<::KeyboardKey>(100000)));
        CHECK(!rmp::input::pressed(static_cast<::KeyboardKey>(-1)));
        CHECK(!rmp::input::pressed(static_cast<::MouseButton>(999)));
        CHECK(!rmp::input::pressed(static_cast<::GamepadButton>(-3)));
        CHECK(rmp::input::axis_value(static_cast<::GamepadAxis>(77)) ==
              doctest::Approx(0.0f));
        // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
    }

    TEST_CASE("an action bound to an out-of-range code is inert, not a crash") {
        Fixture fix;
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        rmp::input::action("bad", static_cast<::KeyboardKey>(50000));
        frame();
        CHECK(!rmp::input::pressed("bad"));
        CHECK(!rmp::input::just_pressed("bad"));
    }

    TEST_CASE("axis and vector survive names that do not exist") {
        Fixture fix;
        frame();
        CHECK(rmp::input::axis("nope", "also_nope") == doctest::Approx(0.0f));
        const Vector2 v = rmp::input::vector("a", "b", "c", "d");
        CHECK(v.x == doctest::Approx(0.0f));
        CHECK(v.y == doctest::Approx(0.0f));
    }

    TEST_CASE("a stick value past its own range is still bounded") {
        Fixture fix;
        rmp::input::set_deadzone(0.2f);
        // A driver reporting outside -1..1 is not hypothetical; some do.
        push(GAMEPAD_AXIS_LEFT_X, 4.0f);
        frame();
        CHECK(rmp::input::pressed("move_right"));
        CHECK(length(rmp::input::vector()) <= doctest::Approx(1.0f).epsilon(0.001));
    }

    TEST_CASE(
        "reading before any frame at all answers false rather than reading nothing") {
        Fixture fix;
        // No frame() call. Every accessor has to hold up against a state that has
        // never been sampled, because that is what the first line of a game's
        // _ready() sees.
        CHECK(!rmp::input::pressed("move_left"));
        CHECK(!rmp::input::pressed(KEY_A));
        CHECK(rmp::input::axis() == doctest::Approx(0.0f));
        CHECK(length(rmp::input::vector()) == doctest::Approx(0.0f));
        CHECK(!rmp::input::pointer_down());
        CHECK(rmp::input::pointer_delta().x == doctest::Approx(0.0f));
    }

} // TEST_SUITE
