// ---------------------------------------------------------------------------
// rmp::input, held to what rmp/input.h says of it for any device state a
// driver, a test or a broken gamepad can hand the seam -- NaN, the infinities
// and values far past -1..1 on a stick, a pointer that is not a number -- and
// any dead zone a game can set:
//
//   - vector() is never longer than 1 and never NaN; axis() and axis_value()
//     stay in -1..1, and a stick's two halves read alike;
//   - a bigger dead zone never makes a stick read more;
//   - set_deadzone() keeps a number in 0..0.95, and refuses one that is not a
//     number, saying so once;
//   - an action is a boolean: pressed() is any of its bindings down, and its
//     edges are the edges of that -- against a model of the frames, with up
//     to nine bindings of every kind (the ninth ignored and said once), and a
//     layer that cannot reach the input hearing none of it;
//   - reading by device is the device state, edges included, and never routed;
//   - the pointer is a finite number of pixels, its delta the difference, and
//     through a scene's camera in world units;
//   - an action nobody defined reads false and 0, and is said once per name.
//
// A counterexample one of these finds becomes a named case in
// tests/input_test.cpp; the commit that added them says what each was seen
// red on.
//
// covers: rmp::input::action [template]
// covers: rmp::input::axis()
// covers: rmp::input::axis(std::string_view, std::string_view)
// covers: rmp::input::axis_value(::GamepadAxis)
// covers: rmp::input::consumed_keyboard()
// covers: rmp::input::consumed_pointer()
// covers: rmp::input::deadzone()
// covers: rmp::input::just_pressed(::GamepadButton)
// covers: rmp::input::just_pressed(::KeyboardKey)
// covers: rmp::input::just_pressed(::MouseButton)
// covers: rmp::input::just_pressed(std::string_view)
// covers: rmp::input::just_released(::GamepadButton)
// covers: rmp::input::just_released(::KeyboardKey)
// covers: rmp::input::just_released(::MouseButton)
// covers: rmp::input::just_released(std::string_view)
// covers: rmp::input::pointer()
// covers: rmp::input::pointer_delta()
// covers: rmp::input::pointer_down()
// covers: rmp::input::pointer_pressed()
// covers: rmp::input::pointer_released()
// covers: rmp::input::pointer_screen()
// covers: rmp::input::pressed(::GamepadButton)
// covers: rmp::input::pressed(::KeyboardKey)
// covers: rmp::input::pressed(::MouseButton)
// covers: rmp::input::pressed(std::string_view)
// covers: rmp::input::set_deadzone(float)
// covers: rmp::input::stick(::GamepadAxis, int)
// covers: rmp::input::vector()
// covers: rmp::input::vector(std::string_view, std::string_view, std::string_view, std::string_view)
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/internal.h"
#include "../../src/rmp/scene_internal.h"

#include <rmp/input.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

using rmp::input::detail::Binding;
using rmp::input::detail::Device;
using rmp::input::detail::DeviceState;

// What the fake device hands the seam. The dot says "file state".
struct {
    DeviceState devices;
} fake;

void fake_sample(DeviceState &out) { out = fake.devices; }

// No actions, no history, the fake device and the log off for a property;
// raylib's provider and the log back afterwards.
struct Devices {
    Devices() {
        SetTraceLogLevel(LOG_NONE);
        rmp::input::detail::reset();
        fake.devices = DeviceState{};
        rmp::input::detail::set_sample_provider(fake_sample);
        rmp::detail::reset_reports_for_tests();
    }
    ~Devices() {
        rmp::input::detail::reset();
        fake.devices = DeviceState{};
        rmp::detail::reset_reports_for_tests();
        SetTraceLogLevel(LOG_INFO);
    }
    Devices(const Devices &) = delete;
    Devices &operator=(const Devices &) = delete;
    Devices(Devices &&) = delete;
    Devices &operator=(Devices &&) = delete;
};

// The largest a pointer coordinate reads: past it a float stops counting
// every pixel, and no window is anywhere near (src/rmp/input.cpp).
constexpr float POINTER_LIMIT = 16777216.0f;

// A stick's reading: the ones a dead zone is about -- at rest, its ends, a
// hair either side of the default dead zone -- the ones a broken driver sends,
// and any float. By category: inRange would shrink with the run's size.
rc::Gen<float> stick_reading() {
    return rc::gen::weightedOneOf<float>({
        { 2,
          rc::gen::element(0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.2f, -0.2f,
                           std::nextafter(0.2f, 1.0f), std::nextafter(0.2f, 0.0f), 0.95f,
                           -0.95f, 0.99f) },
        { 1, rc::gen::element(4.0f, -4.0f, 1.0001f, -1.0001f, 1e30f, -1e30f) },
        { 2, rmp::test::gen::hostile_float() },
    });
}

// A dead zone a game can set: inside 0..0.95, at its ends, past them, and any
// float.
rc::Gen<float> dead_zone() {
    return rc::gen::weightedOneOf<float>({
        { 2, rc::gen::element(0.0f, 0.1f, 0.2f, 0.5f, 0.9f, 0.95f) },
        { 1, rc::gen::element(-1.0f, 0.96f, 1.0f, 2.0f) },
        { 1, rmp::test::gen::hostile_float() },
    });
}

// What a stick's reading is to the framework: a number in -1..1, NaN at rest
// (input.h: "-1..+1").
float as_read(const float raw) {
    if (std::isnan(raw)) return 0.0f;
    return std::fmax(-1.0f, std::fmin(1.0f, raw));
}

// A whole frame of devices: a few keys, buttons and pad buttons down -- codes
// within each enum's range, some past the end of the table -- six stick
// readings, and a pointer.
DeviceState draw_devices() {
    DeviceState state;
    for (const int key :
         *rc::gen::container<std::vector<int>>(rc::gen::weightedOneOf<int>(
             { { 3,
                 rc::gen::element<int>(KEY_A, KEY_D, KEY_W, KEY_S, KEY_SPACE, KEY_ENTER,
                                       KEY_LEFT, KEY_ESCAPE) },
               { 1, rc::gen::inRange(0, DeviceState::KEYS) } }))) {
        state.keys[static_cast<std::size_t>(key)] = true;
    }
    for (const int button : *rc::gen::container<std::vector<int>>(
             rc::gen::inRange(0, DeviceState::MOUSE_BUTTONS))) {
        state.mouse[static_cast<std::size_t>(button)] = true;
    }
    for (const int button : *rc::gen::container<std::vector<int>>(
             rc::gen::inRange(0, DeviceState::PAD_BUTTONS))) {
        state.pad[static_cast<std::size_t>(button)] = true;
    }
    for (float &axis : state.axes) axis = *stick_reading();
    state.pointer =
        Vector2{ *rmp::test::gen::hostile_float(), *rmp::test::gen::hostile_float() };
    return state;
}

// One binding of any kind, its code inside the enum the overloads take: a
// KeyboardKey is 0..511, a MouseButton 0..7, a GamepadButton 0..31, a
// GamepadAxis 0..7 -- casting past those is undefined behaviour by itself --
// and past the end of DeviceState's tables for the last three.
Binding draw_binding() {
    switch (*rc::gen::element(0, 1, 2, 3)) {
        case 0:
            return rmp::input::detail::to_binding(
                static_cast<KeyboardKey>(*rc::gen::weightedOneOf<int>(
                    { { 3, rc::gen::element<int>(KEY_A, KEY_D, KEY_SPACE, KEY_ENTER) },
                      { 1, rc::gen::inRange(0, 512) } })));
        case 1:
            return rmp::input::detail::to_binding(
                static_cast<MouseButton>(*rc::gen::inRange(0, 8)));
        case 2:
            return rmp::input::detail::to_binding(
                static_cast<GamepadButton>(*rc::gen::inRange(0, 32)));
        default:
            return rmp::input::stick(static_cast<GamepadAxis>(*rc::gen::inRange(0, 8)),
                                     *rc::gen::element(-1, 1));
    }
}

// Is `b` down in `state`, read the way input.h says a binding is read?
bool model_down(const Binding &b, const DeviceState &state, const float zone) {
    const auto at = [](const auto &table, int code) {
        return code >= 0 && static_cast<std::size_t>(code) < table.size() &&
            table[static_cast<std::size_t>(code)];
    };
    switch (b.device) {
        case Device::KEY:
            return at(state.keys, b.code);
        case Device::MOUSE:
            return at(state.mouse, b.code);
        case Device::PAD_BUTTON:
            return at(state.pad, b.code);
        case Device::PAD_AXIS: {
            if (b.code < 0 || b.code >= DeviceState::AXES) return false;
            const float v = as_read(state.axes[static_cast<std::size_t>(b.code)]);
            if (std::fabs(v) < zone) return false;
            return b.sign < 0 ? v < 0 : v > 0;
        }
    }
    return false;
}

// rmp::input::action() with however many bindings were drawn -- it is a
// variadic template, so the count is a case of a switch.
void define(const std::string &name, const std::vector<Binding> &b) {
    switch (b.size()) {
        case 1:
            rmp::input::action(name, b[0]);
            return;
        case 2:
            rmp::input::action(name, b[0], b[1]);
            return;
        case 3:
            rmp::input::action(name, b[0], b[1], b[2]);
            return;
        case 4:
            rmp::input::action(name, b[0], b[1], b[2], b[3]);
            return;
        case 5:
            rmp::input::action(name, b[0], b[1], b[2], b[3], b[4]);
            return;
        case 6:
            rmp::input::action(name, b[0], b[1], b[2], b[3], b[4], b[5]);
            return;
        case 7:
            rmp::input::action(name, b[0], b[1], b[2], b[3], b[4], b[5], b[6]);
            return;
        case 8:
            rmp::input::action(name, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
            return;
        default:
            rmp::input::action(name, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                               b[8]);
            return;
    }
}

// A scene on the stack for a property, and none once it ends however it ends.
struct OneScene {
    OneScene() { rmp::scenes::detail::start(std::make_unique<rmp::Scene>()); }
    ~OneScene() { rmp::scenes::detail::shutdown(); }
    OneScene(const OneScene &) = delete;
    OneScene &operator=(const OneScene &) = delete;
    OneScene(OneScene &&) = delete;
    OneScene &operator=(OneScene &&) = delete;
};

float length(const Vector2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

void frame() { rmp::input::detail::begin_frame(); }

} // namespace

TEST_SUITE("property: input") {
    TEST_CASE("directions stay directions") {
        rmp::test::check(
            "vector() is never longer than 1 and never NaN, for any device state and "
            "dead zone",
            [] {
                const Devices devices;
                rmp::input::set_deadzone(*dead_zone());
                // Four more actions on the sticks, so vector() of names reads
                // two sticks pushing one way.
                rmp::input::action("l", rmp::input::stick(GAMEPAD_AXIS_RIGHT_X, -1),
                                   rmp::input::stick(GAMEPAD_AXIS_LEFT_X, -1));
                rmp::input::action("r", rmp::input::stick(GAMEPAD_AXIS_RIGHT_X, 1),
                                   KEY_D);
                rmp::input::action("u", rmp::input::stick(GAMEPAD_AXIS_RIGHT_Y, -1));
                rmp::input::action("d", rmp::input::stick(GAMEPAD_AXIS_LEFT_TRIGGER, 1));
                const int frames = *rc::gen::inRange(1, 4);
                for (int f = 0; f < frames; ++f) {
                    fake.devices = draw_devices();
                    frame();
                    for (const Vector2 v : { rmp::input::vector(),
                                             rmp::input::vector("l", "r", "u", "d") }) {
                        RC_ASSERT(std::isfinite(v.x));
                        RC_ASSERT(std::isfinite(v.y));
                        RC_ASSERT(length(v) <= 1.0f + 1e-6f);
                    }
                }
            });

        rmp::test::check(
            "axis() and axis_value() stay in -1..1, and a stick's two halves read alike",
            [] {
                const Devices devices;
                rmp::input::set_deadzone(*dead_zone());
                rmp::input::action("l", rmp::input::stick(GAMEPAD_AXIS_RIGHT_X, -1));
                rmp::input::action("r", rmp::input::stick(GAMEPAD_AXIS_LEFT_Y, 1));
                fake.devices = draw_devices();
                frame();
                for (const float a : { rmp::input::axis(), rmp::input::axis("l", "r") }) {
                    RC_ASSERT(std::isfinite(a));
                    RC_ASSERT(std::fabs(a) <= 1.0f);
                }
                const auto which = static_cast<GamepadAxis>(*rc::gen::inRange(0, 6));
                const float raw = *stick_reading();
                fake.devices.axes[static_cast<std::size_t>(which)] = raw;
                frame();
                const float there = rmp::input::axis_value(which);
                RC_ASSERT(std::isfinite(there));
                RC_ASSERT(std::fabs(there) <= 1.0f);
                if (std::fabs(as_read(raw)) < rmp::input::deadzone())
                    RC_ASSERT(there == 0.0f);
                fake.devices.axes[static_cast<std::size_t>(which)] = -raw;
                frame();
                RC_ASSERT(rmp::input::axis_value(which) == -there);
            });

        rmp::test::check("a bigger dead zone never makes a stick read more", [] {
            const Devices devices;
            const float raw = *stick_reading();
            const float a = *dead_zone();
            const float b = *dead_zone();
            fake.devices.axes[GAMEPAD_AXIS_LEFT_X] = raw;
            frame();
            rmp::input::set_deadzone(a);
            const float zone_a = rmp::input::deadzone();
            const float value_a = std::fabs(rmp::input::axis_value(GAMEPAD_AXIS_LEFT_X));
            const float amount_a = std::fabs(rmp::input::axis());
            rmp::input::set_deadzone(b);
            const float zone_b = rmp::input::deadzone();
            const float value_b = std::fabs(rmp::input::axis_value(GAMEPAD_AXIS_LEFT_X));
            const float amount_b = std::fabs(rmp::input::axis());
            RC_PRE(zone_a != zone_b);
            // A millionth for the rounding of two divisions.
            const bool a_smaller = zone_a < zone_b;
            const float more = a_smaller ? value_b : value_a;
            const float less = a_smaller ? value_a : value_b;
            RC_ASSERT(more <= less + 1e-6f);
            RC_ASSERT((a_smaller ? amount_b : amount_a) <=
                      (a_smaller ? amount_a : amount_b) + 1e-6f);
        });

        rmp::test::check("set_deadzone keeps a number in 0..0.95, and one that is not a "
                         "number changes "
                         "nothing and is said once",
                         [] {
                             const Devices devices;
                             const float first = *dead_zone();
                             rmp::input::set_deadzone(first);
                             const float kept = rmp::input::deadzone();
                             RC_ASSERT(kept >= 0.0f);
                             RC_ASSERT(kept <= 0.95f);
                             if (std::isnan(first)) {
                                 RC_ASSERT(kept == RMP_INPUT_DEADZONE);
                             } else if (first >= 0 && first <= 0.95f) {
                                 RC_ASSERT(kept == first);
                             }
                             const int before = rmp::detail::report_count();
                             for (int again = 0; again < 2; ++again) {
                                 rmp::input::set_deadzone(
                                     std::numeric_limits<float>::quiet_NaN());
                                 RC_ASSERT(rmp::input::deadzone() == kept);
                             }
                             RC_ASSERT(rmp::detail::report_count() ==
                                       (std::isnan(first) ? before : before + 1));
                         });
    }

    TEST_CASE("an action is a boolean") {
        rmp::test::check(
            "an action is held while any of its bindings is down, and its edges are the "
            "edges of that",
            [] {
                const Devices devices;
                const std::size_t count =
                    *rc::gen::element<std::size_t>(1, 2, 3, 4, 8, 9);
                std::vector<Binding> bindings;
                for (std::size_t i = 0; i < count; ++i)
                    bindings.push_back(draw_binding());
                rmp::input::set_deadzone(*rc::gen::element(0.0f, 0.2f, 0.5f));
                const std::string name =
                    *rc::gen::element<std::string>("jump", "", "fire");
                define(name, bindings);
                // The ninth binding is past the table: ignored, and said once.
                RC_ASSERT(rmp::detail::report_count() == (count > 8 ? 1 : 0));
                define(name, bindings);
                RC_ASSERT(rmp::detail::report_count() == (count > 8 ? 1 : 0));
                const std::size_t live = std::min<std::size_t>(count, 8);
                const auto held = [&](const DeviceState &state) {
                    return std::any_of(
                        bindings.begin(),
                        bindings.begin() + static_cast<std::ptrdiff_t>(live),
                        [&](const Binding &b) {
                            return model_down(b, state, rmp::input::deadzone());
                        });
                };
                DeviceState before;
                bool first = true;
                const int frames = *rc::gen::inRange(1, 8);
                for (int f = 0; f < frames; ++f) {
                    // Each frame the last one with a little changed, or another.
                    if (*rc::gen::element(0, 1, 2) == 0 || first) {
                        fake.devices = draw_devices();
                    } else {
                        const Binding &touched =
                            bindings[*rc::gen::inRange<std::size_t>(0, live)];
                        if (touched.device == Device::KEY && touched.code >= 0 &&
                            touched.code < DeviceState::KEYS) {
                            auto &key =
                                fake.devices.keys[static_cast<std::size_t>(touched.code)];
                            key = !key;
                        }
                    }
                    frame();
                    const DeviceState now = fake.devices;
                    if (first) before = now;
                    first = false;
                    // A layer the input cannot reach hears nothing by name.
                    const bool reachable = *rc::gen::element(true, true, true, false);
                    rmp::input::detail::set_layer_input(reachable);
                    const bool was = held(before);
                    const bool is = held(now);
                    RC_ASSERT(rmp::input::pressed(name) == (reachable && is));
                    RC_ASSERT(rmp::input::just_pressed(name) ==
                              (reachable && is && !was));
                    RC_ASSERT(rmp::input::just_released(name) ==
                              (reachable && !is && was));
                    before = now;
                }
            });

        rmp::test::check(
            "an action nobody defined reads false and 0, and is said once per name", [] {
                const Devices devices;
                fake.devices = draw_devices();
                frame();
                // No NUL: the report is keyed by the name as a C string, and
                // two names that differ only past a NUL are one key (said in
                // the commit that added this; report.cpp is not this file's).
                std::string name = *rmp::test::gen::hostile_string();
                std::erase(name, '\0');
                RC_PRE(name.rfind("move_", 0) != 0 && name.rfind("ui_", 0) != 0);
                for (int again = 0; again < 2; ++again) {
                    RC_ASSERT(!rmp::input::pressed(name));
                    RC_ASSERT(!rmp::input::just_pressed(name));
                    RC_ASSERT(!rmp::input::just_released(name));
                    RC_ASSERT(rmp::input::axis(name, name) == 0.0f);
                    const Vector2 v = rmp::input::vector(name, name, name, name);
                    RC_ASSERT(v.x == 0.0f);
                    RC_ASSERT(v.y == 0.0f);
                }
                RC_ASSERT(rmp::detail::report_count() == 2); // false, and 0
            });

        rmp::test::check("stick() is one half of one axis for any sign", [] {
            const auto which = static_cast<GamepadAxis>(*rc::gen::inRange(0, 8));
            const int sign = *rc::gen::arbitrary<int>();
            const Binding b = rmp::input::stick(which, sign);
            RC_ASSERT(b.device == Device::PAD_AXIS);
            RC_ASSERT(b.code == static_cast<int>(which));
            RC_ASSERT(b.sign == (sign < 0 ? -1 : 1));
        });
    }

    TEST_CASE("the raw reads are the devices") {
        rmp::test::check(
            "reading by device is the device state, edges included, and never routed",
            [] {
                const Devices devices;
                DeviceState before;
                const int frames = *rc::gen::inRange(1, 6);
                for (int f = 0; f < frames; ++f) {
                    fake.devices = draw_devices();
                    frame();
                    const DeviceState now = fake.devices;
                    if (f == 0) before = now;
                    rmp::input::detail::set_layer_input(*rc::gen::arbitrary<bool>());
                    const auto key = static_cast<std::size_t>(*rc::gen::inRange(0, 512));
                    const auto k = static_cast<KeyboardKey>(key);
                    RC_ASSERT(rmp::input::pressed(k) == now.keys[key]);
                    RC_ASSERT(rmp::input::just_pressed(k) ==
                              (now.keys[key] && !before.keys[key]));
                    RC_ASSERT(rmp::input::just_released(k) ==
                              (!now.keys[key] && before.keys[key]));
                    const auto button = static_cast<std::size_t>(*rc::gen::inRange(0, 8));
                    const auto m = static_cast<MouseButton>(button);
                    RC_ASSERT(rmp::input::pressed(m) == now.mouse[button]);
                    RC_ASSERT(rmp::input::just_pressed(m) ==
                              (now.mouse[button] && !before.mouse[button]));
                    RC_ASSERT(rmp::input::just_released(m) ==
                              (!now.mouse[button] && before.mouse[button]));
                    const auto pad = static_cast<std::size_t>(*rc::gen::inRange(0, 32));
                    const auto p = static_cast<GamepadButton>(pad);
                    const bool down = pad < now.pad.size() && now.pad[pad];
                    const bool was = pad < before.pad.size() && before.pad[pad];
                    RC_ASSERT(rmp::input::pressed(p) == down);
                    RC_ASSERT(rmp::input::just_pressed(p) == (down && !was));
                    RC_ASSERT(rmp::input::just_released(p) == (!down && was));
                    const bool left = now.mouse[MOUSE_BUTTON_LEFT];
                    const bool left_was = before.mouse[MOUSE_BUTTON_LEFT];
                    RC_ASSERT(rmp::input::pointer_down() == left);
                    RC_ASSERT(rmp::input::pointer_pressed() == (left && !left_was));
                    RC_ASSERT(rmp::input::pointer_released() == (!left && left_was));
                    // What is consumed is the layer's, or the interface's.
                    const bool layer = rmp::input::detail::layer_input();
                    RC_ASSERT(rmp::input::consumed_pointer() ==
                              (!layer || rmp::ui::wants_pointer()));
                    RC_ASSERT(rmp::input::consumed_keyboard() ==
                              (!layer || rmp::ui::wants_keyboard()));
                    before = now;
                }
            });

        rmp::test::check(
            "the pointer is a finite number of pixels, its delta the difference, and in "
            "world units through the scene's camera",
            [] {
                const Devices devices;
                Vector2 last{ 0, 0 };
                const bool scene = *rc::gen::arbitrary<bool>();
                const std::unique_ptr<OneScene> stack =
                    scene ? std::make_unique<OneScene>() : nullptr;
                const int frames = *rc::gen::inRange(1, 6);
                for (int f = 0; f < frames; ++f) {
                    fake.devices = draw_devices();
                    frame();
                    const Vector2 raw = fake.devices.pointer;
                    // A coordinate that is not a number is the last one; one
                    // past POINTER_LIMIT is held there.
                    const auto read = [](float v, float was) {
                        return std::isnan(v)
                            ? was
                            : std::fmax(-POINTER_LIMIT, std::fmin(POINTER_LIMIT, v));
                    };
                    const Vector2 want{ read(raw.x, last.x), read(raw.y, last.y) };
                    const Vector2 got = rmp::input::pointer_screen();
                    RC_ASSERT(got.x == want.x);
                    RC_ASSERT(got.y == want.y);
                    const Vector2 delta = rmp::input::pointer_delta();
                    RC_ASSERT(std::isfinite(delta.x));
                    RC_ASSERT(std::isfinite(delta.y));
                    RC_ASSERT(delta.x == (f == 0 ? 0.0f : want.x - last.x));
                    RC_ASSERT(delta.y == (f == 0 ? 0.0f : want.y - last.y));
                    const Vector2 world = rmp::input::pointer();
                    if (scene) {
                        const Vector2 through =
                            rmp::Scene::current().camera.to_world(got);
                        RC_ASSERT(world.x == through.x);
                        RC_ASSERT(world.y == through.y);
                    } else {
                        RC_ASSERT(world.x == got.x);
                        RC_ASSERT(world.y == got.y);
                    }
                    last = want;
                }
            });
    }
}
