// ===========================================================================
// rmp::input — see include/rmp/input.h.
//
// Three things live here and only the first is obvious:
//
//   1. a table of named actions and their bindings;
//   2. ONE sample of the devices per frame, so that two scenes in the same
//      frame cannot disagree about what is held down;
//   3. the routing rule — a query by name answers false when something above
//      the caller has taken the input.
//
// The third is the whole reason this file exists rather than everybody calling
// IsKeyDown. It is also the reason every device read goes through a provider:
// with it replaced there is no raylib underneath, which is what lets
// tests/input_test.cpp check all of this with no window and no devices.
// ===========================================================================

#include <rmp/input.h>

#include <rmp/ui.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace rmp::input {

namespace {

constexpr int kMaxBindings = 8;

struct Action {
    std::string name;
    detail::Binding bindings[kMaxBindings];
    int count = 0;
};

std::vector<Action> g_actions;

// Two frames of device state. `just_pressed` is the difference between them,
// which is why sampling has to happen exactly once per frame: sample twice and
// the edge disappears between the two reads.
detail::DeviceState g_now;
detail::DeviceState g_before;
bool g_have_previous = false;

detail::SampleFn g_sample = detail::sample_with_raylib;

float g_deadzone = APP_INPUT_DEADZONE;
bool g_layer_input = true;

// One warning per unknown name for the life of the run. The alternative is
// sixty lines a second, which is the same as no warning at all.
std::vector<std::string> g_warned;

Action *find(std::string_view name) {
    for (Action &action : g_actions) {
        if (action.name == name) return &action;
    }
    return nullptr;
}

// Is `binding`'s device currently being listened to by something above us?
//
// The pointer and the keyboard are asked separately because they are taken
// separately: a text field holds the keyboard while the mouse is free, and a
// hovered button holds the pointer while the keyboard is free. Gamepad buttons
// follow the keyboard, because that is what the UI's focus navigation uses —
// a d-pad press that moves the selection must not also move the player.
bool consumed(const detail::Binding &binding) {
    if (!g_layer_input) return true;
    if (binding.device == detail::Device::MOUSE) return rmp::ui::wants_pointer();
    return rmp::ui::wants_keyboard();
}

bool in_range(int value, int limit) { return value >= 0 && value < limit; }

// Is this binding down, in the given frame of device state? Analogue axes count
// as down once they are past the dead zone on the half the binding named.
bool binding_down(const detail::Binding &binding, const detail::DeviceState &state) {
    switch (binding.device) {
        case detail::Device::KEY:
            return in_range(binding.code, detail::DeviceState::kKeys) &&
                state.keys[binding.code];
        case detail::Device::MOUSE:
            return in_range(binding.code, detail::DeviceState::kMouseButtons) &&
                state.mouse[binding.code];
        case detail::Device::PAD_BUTTON:
            return in_range(binding.code, detail::DeviceState::kPadButtons) &&
                state.pad[binding.code];
        case detail::Device::PAD_AXIS: {
            if (!in_range(binding.code, detail::DeviceState::kAxes)) return false;
            const float value = state.axes[binding.code];
            if (std::fabs(value) < g_deadzone) return false;
            return binding.sign < 0 ? value < 0.0f : value > 0.0f;
        }
    }
    return false;
}

// How far this binding is pushed, 0..1. A key is all or nothing; a stick is
// analogue, rescaled so that the dead zone is the new zero — without that
// rescale the stick jumps from 0 to `deadzone` the moment it wakes up.
float binding_amount(const detail::Binding &binding, const detail::DeviceState &state) {
    if (binding.device != detail::Device::PAD_AXIS) {
        return binding_down(binding, state) ? 1.0f : 0.0f;
    }
    if (!in_range(binding.code, detail::DeviceState::kAxes)) return 0.0f;
    const float raw = state.axes[binding.code];
    const float magnitude = std::fabs(raw);
    if (magnitude < g_deadzone) return 0.0f;
    const bool right_half = binding.sign < 0 ? raw < 0.0f : raw > 0.0f;
    if (!right_half) return 0.0f;
    const float span = 1.0f - g_deadzone;
    return span <= 0.0f ? 1.0f : (magnitude - g_deadzone) / span;
}

enum class Edge { HELD, DOWN, UP };

bool action_state(std::string_view name, Edge edge) {
    const Action *action = find(name);
    if (action == nullptr) {
        bool already = false;
        for (const std::string &seen : g_warned) {
            if (seen == name) {
                already = true;
                break;
            }
        }
        if (!already) {
            g_warned.emplace_back(name);
            TraceLog(
                LOG_WARNING,
                "INPUT: no action called \"%.*s\". Define it with "
                "rmp::input::action(\"%.*s\", KEY_...); until then it reads as false.",
                static_cast<int>(name.size()), name.data(), static_cast<int>(name.size()),
                name.data());
        }
        return false;
    }

    for (int i = 0; i < action->count; i++) {
        const detail::Binding &binding = action->bindings[i];
        if (consumed(binding)) continue;
        const bool now = binding_down(binding, g_now);
        const bool before = g_have_previous && binding_down(binding, g_before);
        switch (edge) {
            case Edge::HELD:
                if (now) return true;
                break;
            case Edge::DOWN:
                if (now && !before) return true;
                break;
            case Edge::UP:
                if (!now && before) return true;
                break;
        }
    }
    return false;
}

// How far an action is pushed, 0..1, taking the strongest of its bindings. A
// key and a stick on the same action means the key wins whenever it is down,
// which is what someone pressing both expects.
float action_amount(std::string_view name) {
    const Action *action = find(name);
    if (action == nullptr) return 0.0f;
    float best = 0.0f;
    for (int i = 0; i < action->count; i++) {
        const detail::Binding &binding = action->bindings[i];
        if (consumed(binding)) continue;
        const float amount = binding_amount(binding, g_now);
        if (amount > best) best = amount;
    }
    return best;
}

bool raw_down(detail::Device device, int code, const detail::DeviceState &state) {
    return binding_down(detail::Binding{ device, static_cast<int16_t>(code), 0 }, state);
}

bool raw_edge(detail::Device device, int code, Edge edge) {
    const bool now = raw_down(device, code, g_now);
    const bool before = g_have_previous && raw_down(device, code, g_before);
    switch (edge) {
        case Edge::HELD:
            return now;
        case Edge::DOWN:
            return now && !before;
        case Edge::UP:
            return !now && before;
    }
    return false;
}

// The actions that exist before anyone defines anything, so that vector() with
// no arguments is a whole eight-direction movement scheme. They are ordinary
// actions: redefining any of them replaces it like any other.
void install_factory_actions() {
    action("move_left", KEY_A, KEY_LEFT, GAMEPAD_BUTTON_LEFT_FACE_LEFT,
           stick(GAMEPAD_AXIS_LEFT_X, -1));
    action("move_right", KEY_D, KEY_RIGHT, GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
           stick(GAMEPAD_AXIS_LEFT_X, 1));
    action("move_up", KEY_W, KEY_UP, GAMEPAD_BUTTON_LEFT_FACE_UP,
           stick(GAMEPAD_AXIS_LEFT_Y, -1));
    action("move_down", KEY_S, KEY_DOWN, GAMEPAD_BUTTON_LEFT_FACE_DOWN,
           stick(GAMEPAD_AXIS_LEFT_Y, 1));
    action("ui_accept", KEY_ENTER, KEY_SPACE, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    action("ui_cancel", KEY_ESCAPE, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);
}

bool g_factory_installed = false;

void ensure_factory() {
    if (g_factory_installed) return;
    g_factory_installed = true;
    install_factory_actions();
}

} // namespace

// ---------------------------------------------------------------------------

namespace detail {

void define(std::string_view name, const Binding *bindings, int count) {
    ensure_factory();
    if (count > kMaxBindings) {
        TraceLog(LOG_WARNING,
                 "INPUT: \"%.*s\" was given %d bindings and the limit is %d; the extra "
                 "ones are ignored.",
                 static_cast<int>(name.size()), name.data(), count, kMaxBindings);
        count = kMaxBindings;
    }

    Action *existing = find(name);
    // Replace, never accumulate. This is what makes it safe to call from a
    // scene's _ready() when the player enters that scene twenty times.
    Action &slot = existing != nullptr ? *existing : g_actions.emplace_back();
    slot.name.assign(name);
    slot.count = count;
    for (int i = 0; i < count; i++) slot.bindings[i] = bindings[i];
}

void sample_with_raylib(DeviceState *out) {
    for (int key = 0; key < DeviceState::kKeys; key++) {
        out->keys[key] = IsKeyDown(key);
    }
    for (int button = 0; button < DeviceState::kMouseButtons; button++) {
        out->mouse[button] = IsMouseButtonDown(button);
    }
    // Gamepad 0 only, deliberately: local multiplayer is a real feature and a
    // bigger one than a second index, so it waits for a game that needs it
    // rather than being half-there.
    const bool pad = IsGamepadAvailable(0);
    for (int button = 0; button < DeviceState::kPadButtons; button++) {
        out->pad[button] = pad && IsGamepadButtonDown(0, button);
    }
    for (int axis = 0; axis < DeviceState::kAxes; axis++) {
        out->axes[axis] = pad ? GetGamepadAxisMovement(0, axis) : 0.0f;
    }
    // Touch and mouse are the same pointer, which is what lets the same code
    // work on all fourteen targets with no #ifdef. raylib already maps the
    // first touch onto the mouse, so this is one call and not two.
    out->pointer = GetMousePosition();
}

void set_sample_provider(SampleFn fn) {
    g_sample = fn != nullptr ? fn : sample_with_raylib;
}

void begin_frame() {
    ensure_factory();
    const bool first = !g_have_previous;
    g_before = g_now;
    g_layer_input = true;
    g_sample(&g_now);

    // THE FIRST FRAME HAS NO EDGES, and it takes a line to say so. Without it
    // the previous frame is a zeroed struct, so anything already held when the
    // game launches — a key, a mouse button, a stick — reads as just_pressed on
    // frame one and fires whatever it is bound to. The pointer has the same
    // problem in a more visible form: its delta would be the whole distance
    // from the origin to wherever the cursor happens to be.
    //
    // Copying the fresh sample backwards is the whole fix: frame one compares
    // against itself, so nothing changed, which is the truth.
    if (first) {
        g_before = g_now;
        g_have_previous = true;
    }
}

void set_layer_input(bool reachable) { g_layer_input = reachable; }
bool layer_input() { return g_layer_input; }

void reset() {
    g_actions.clear();
    g_warned.clear();
    g_now = DeviceState{};
    g_before = DeviceState{};
    g_have_previous = false;
    g_layer_input = true;
    g_deadzone = APP_INPUT_DEADZONE;
    g_factory_installed = false;
    g_sample = sample_with_raylib;
}

} // namespace detail

// ---------------------------------------------------------------------------

bool pressed(std::string_view name) { return action_state(name, Edge::HELD); }
bool just_pressed(std::string_view name) { return action_state(name, Edge::DOWN); }
bool just_released(std::string_view name) { return action_state(name, Edge::UP); }

bool pressed(::KeyboardKey key) { return raw_edge(detail::Device::KEY, key, Edge::HELD); }
bool pressed(::MouseButton button) {
    return raw_edge(detail::Device::MOUSE, button, Edge::HELD);
}
bool pressed(::GamepadButton button) {
    return raw_edge(detail::Device::PAD_BUTTON, button, Edge::HELD);
}
bool just_pressed(::KeyboardKey key) {
    return raw_edge(detail::Device::KEY, key, Edge::DOWN);
}
bool just_pressed(::MouseButton button) {
    return raw_edge(detail::Device::MOUSE, button, Edge::DOWN);
}
bool just_pressed(::GamepadButton button) {
    return raw_edge(detail::Device::PAD_BUTTON, button, Edge::DOWN);
}
bool just_released(::KeyboardKey key) {
    return raw_edge(detail::Device::KEY, key, Edge::UP);
}
bool just_released(::MouseButton button) {
    return raw_edge(detail::Device::MOUSE, button, Edge::UP);
}
bool just_released(::GamepadButton button) {
    return raw_edge(detail::Device::PAD_BUTTON, button, Edge::UP);
}

float axis_value(::GamepadAxis which) {
    const int index = static_cast<int>(which);
    if (!in_range(index, detail::DeviceState::kAxes)) return 0.0f;
    const float raw = g_now.axes[index];
    if (std::fabs(raw) < g_deadzone) return 0.0f;
    const float span = 1.0f - g_deadzone;
    const float scaled = (std::fabs(raw) - g_deadzone) / (span <= 0.0f ? 1.0f : span);
    return raw < 0.0f ? -scaled : scaled;
}

float axis(std::string_view negative, std::string_view positive) {
    ensure_factory();
    // Both at once is 0, which is what somebody mashing both keys expects, and
    // it falls out of the subtraction rather than needing a rule.
    return action_amount(positive) - action_amount(negative);
}

Vector2 vector(std::string_view left, std::string_view right, std::string_view up,
               std::string_view down) {
    ensure_factory();
    Vector2 result{ action_amount(right) - action_amount(left),
                    action_amount(down) - action_amount(up) };
    // NORMALISED, and this is the line the whole function exists for: without
    // it, holding right and down moves you 1.41 times as fast as holding right,
    // which is the single most common bug in 2D movement code.
    //
    // Only when it is longer than 1, so a stick pushed halfway stays halfway.
    const float length = std::sqrt(result.x * result.x + result.y * result.y);
    if (length > 1.0f) {
        result.x /= length;
        result.y /= length;
    }
    return result;
}

float axis() { return axis("move_left", "move_right"); }
Vector2 vector() { return vector("move_left", "move_right", "move_up", "move_down"); }

Vector2 pointer_screen() { return g_now.pointer; }

Vector2 pointer_delta() {
    // No special case for the first frame: begin_frame() makes g_before equal
    // g_now there, so this subtraction is already zero.
    return Vector2{ g_now.pointer.x - g_before.pointer.x,
                    g_now.pointer.y - g_before.pointer.y };
}

bool pointer_down() { return pressed(MOUSE_BUTTON_LEFT); }
bool pointer_pressed() { return just_pressed(MOUSE_BUTTON_LEFT); }
bool pointer_released() { return just_released(MOUSE_BUTTON_LEFT); }

float deadzone() { return g_deadzone; }

void set_deadzone(float value) {
    // Clamped rather than rejected: this one is reachable from a settings
    // slider, and a slider that can wedge the sticks off is worse than one
    // whose ends do nothing.
    g_deadzone = value < 0.0f ? 0.0f : (value > 0.95f ? 0.95f : value);
}

bool consumed_pointer() { return !detail::layer_input() || rmp::ui::wants_pointer(); }
bool consumed_keyboard() { return !detail::layer_input() || rmp::ui::wants_keyboard(); }

} // namespace rmp::input
