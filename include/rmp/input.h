#pragma once
// ---------------------------------------------------------------------------
// rmp::input — named actions, and who gets to hear them.
//
// Two things raylib deliberately does not decide for you, and a game needs
// both on its first day.
//
//     rmp::input::action("jump", KEY_SPACE, KEY_W, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
//     if (rmp::input::just_pressed("jump")) player.jump();
//
// A name and as many bindings as you like. The TYPE of each one says which
// device it belongs to — raylib's KeyboardKey, MouseButton and GamepadButton
// are four different enums in C++ — so the dispatch happens at compile time and
// passing anything else is a compile error that names the argument.
//
// And moving in eight directions, with keyboard, d-pad and stick at once, with
// no configuration at all:
//
//     player.velocity = rmp::input::vector() * speed;
//
// THE PART THAT IS NOT OBVIOUS is who gets to hear an action. A finger that
// presses a UI button must not also fire the gun underneath it, and that has to
// be true without anyone writing an `if`. So a query BY NAME is routed:
//
//     rmp::input::just_pressed("fire")   // silent while the UI wants the pointer
//     rmp::input::pressed(KEY_W)         // raw, exactly IsKeyDown, never routed
//
// The argument type says which of the two you asked for, and you can see it at
// the call site without going to look anything up.
// ---------------------------------------------------------------------------

#include <raylib.h>
#include <rmp/config.h>

#include <cstdint>
#include <string_view>

namespace rmp::input {

namespace detail {

// Where a binding comes from. One byte, because an action holds several and
// they are copied around by value.
enum class Device : uint8_t { KEY, MOUSE, PAD_BUTTON, PAD_AXIS };

struct Binding {
    Device device = Device::KEY;
    int16_t code = 0;
    // Only for PAD_AXIS: which half of the stick counts. A stick reads -1..+1
    // and an action is a boolean, so "left" is the negative half of X.
    int8_t sign = 0;
};

// One overload per raylib enum. This is the whole dispatch: there is no runtime
// tagging and no way to pass a MouseButton where a key was meant, because the
// types simply do not convert to one another in C++.
constexpr Binding to_binding(::KeyboardKey key) {
    return Binding{ Device::KEY, static_cast<int16_t>(key), 0 };
}
constexpr Binding to_binding(::MouseButton button) {
    return Binding{ Device::MOUSE, static_cast<int16_t>(button), 0 };
}
constexpr Binding to_binding(::GamepadButton button) {
    return Binding{ Device::PAD_BUTTON, static_cast<int16_t>(button), 0 };
}
// stick() has already produced one. Passing a Binding through unchanged is what
// lets the two spellings live in the same argument list.
constexpr Binding to_binding(Binding binding) { return binding; }

// Compiled once, in src/rmp/input.cpp.
void define(std::string_view name, const Binding *bindings, int count);

} // namespace detail

// Half of an analogue stick, as a binding. The one place you write something
// that is not a plain raylib constant:
//
//     rmp::input::action("left", KEY_A, rmp::input::stick(GAMEPAD_AXIS_LEFT_X, -1));
//
// `sign` is -1 or +1 and says which way counts. Anything else is clamped to
// those two, because a stick has two halves and there is no third thing to mean.
constexpr detail::Binding stick(::GamepadAxis axis, int sign) {
    return detail::Binding{ detail::Device::PAD_AXIS, static_cast<int16_t>(axis),
                            static_cast<int8_t>(sign < 0 ? -1 : 1) };
}

// Define an action. At least one binding — an action with none is a mistake the
// compiler can catch, so it does.
//
// REGISTERING THE SAME NAME AGAIN REPLACES IT. It does not add to it. That is
// what makes calling these from a scene's _ready() safe when the player enters
// that scene twenty times, which is where they naturally get written.
template <class First, class... Rest>
void action(std::string_view name, First first, Rest... rest) {
    const detail::Binding bindings[] = { detail::to_binding(first),
                                         detail::to_binding(rest)... };
    detail::define(name, bindings, 1 + static_cast<int>(sizeof...(rest)));
}

// ---------------------------------------------------------------------------
// Reading actions.
//
// BY NAME these are routed: they answer false whenever something above has
// taken the input. An action bound to a mouse button goes quiet while
// rmp::ui::wants_pointer(); one bound to a key goes quiet while
// rmp::ui::wants_keyboard(); and every action goes quiet in a scene that has
// another above it with input_below = false.
//
// Which means the canonical case is simply true:
//
//     if (rmp::input::just_pressed("fire")) shoot();   // not from a UI button
//
// instead of what it takes today:
//
//     if (!rmp::ui::wants_pointer() && IsMouseButtonPressed(0)) shoot();
//
// An action nobody has defined answers false and says so once, rather than
// every frame for the rest of the run.
// ---------------------------------------------------------------------------

bool pressed(std::string_view name); // held down
bool just_pressed(std::string_view name); // the frame it goes down
bool just_released(std::string_view name); // the frame it comes up

// BY DEVICE these are raw: exactly IsKeyDown and friends, never routed, for
// when you want to know regardless of who else is listening.
bool pressed(::KeyboardKey key);
bool pressed(::MouseButton button);
bool pressed(::GamepadButton button);
bool just_pressed(::KeyboardKey key);
bool just_pressed(::MouseButton button);
bool just_pressed(::GamepadButton button);
bool just_released(::KeyboardKey key);
bool just_released(::MouseButton button);
bool just_released(::GamepadButton button);

// The raw position of an analogue axis, -1..+1, with the dead zone applied.
float axis_value(::GamepadAxis which);

// ---------------------------------------------------------------------------
// Directions
// ---------------------------------------------------------------------------

// -1, 0 or +1 from two actions, or the analogue value when a stick is what is
// moving them. `negative` and `positive` pressed at once gives 0, which is what
// a player mashing both keys expects.
float axis(std::string_view negative, std::string_view positive);

// A direction from four actions, NORMALISED — so going diagonally is not 41 %
// faster than going straight. That is the single most common bug in 2D movement
// code and it costs one line here instead of being everybody's to remember.
//
// +Y is DOWN, matching raylib's screen coordinates and rmp::Object::position,
// so "up" gives a negative Y and nothing has to be flipped on the way out.
Vector2 vector(std::string_view left, std::string_view right, std::string_view up,
               std::string_view down);

// The same two, using the actions that come defined out of the box:
//
//     move_left  A  <-  d-pad left   left stick -X
//     move_right D  ->  d-pad right  left stick +X
//     move_up    W  ^   d-pad up     left stick -Y
//     move_down  S  v   d-pad down   left stick +Y
//     ui_accept  Enter, Space        bottom face button
//     ui_cancel  Escape              right face button
//
// They are ordinary actions and can be redefined like any other.
float axis();
Vector2 vector();

// ---------------------------------------------------------------------------
// The pointer. Mouse and finger are the same pointer, which is what lets the
// same code work on all fourteen targets without an #ifdef.
// ---------------------------------------------------------------------------

Vector2 pointer_screen(); // in pixels, top-left origin
Vector2 pointer_delta(); // how far it moved since the last frame
bool pointer_down();
bool pointer_pressed();
bool pointer_released();

// ---------------------------------------------------------------------------
// Escape hatches. These exist because "I want to decide for myself" is a real
// answer, and because rmp::ui::wants_pointer() was public before this header
// existed and stays public.
// ---------------------------------------------------------------------------

// The dead zone below which a stick reads as zero, 0..1. From [input] deadzone
// in raylib_multiplatform.toml; set it here to change it at runtime.
float deadzone();
void set_deadzone(float value);

// Is anything above the caller holding the input? Useful for drawing a
// different cursor, or for code that wants the reason rather than the answer.
bool consumed_pointer();
bool consumed_keyboard();

namespace detail {

// Sample every device, once, at the top of the frame. rmp::app calls it, so
// that two scenes in the same frame cannot disagree about what is held down.
void begin_frame();

// Whether the layer being updated can be reached by input at all. rmp::app
// drives it from Scene::input_below; it resets to true at every frame boundary.
void set_layer_input(bool reachable);
bool layer_input();

// Everything the framework knows about the devices this frame. It is one
// struct, and the whole of the test seam: a headless test writes one of these
// and every function above answers from it.
struct DeviceState {
    static constexpr int kKeys = 512;
    static constexpr int kMouseButtons = 8;
    static constexpr int kPadButtons = 18;
    static constexpr int kAxes = 6;

    bool keys[kKeys] = {};
    bool mouse[kMouseButtons] = {};
    bool pad[kPadButtons] = {};
    float axes[kAxes] = {};
    Vector2 pointer = { 0, 0 };
};

// The provider. Replace it and there is no raylib underneath any more, which is
// what makes tests/input_test.cpp possible with no window and no devices.
using SampleFn = void (*)(DeviceState *out);
void set_sample_provider(SampleFn fn);
void sample_with_raylib(DeviceState *out);

// Forget every action and every device reading. Tests use it between cases; the
// framework uses it on the way out.
void reset();

} // namespace detail

} // namespace rmp::input
