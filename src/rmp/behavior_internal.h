#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. The half of the catalogue that needs no GPU.
//
// The public surface is include/rmp/behavior.h. Everything here exists so that
// tests/behaviors_test.cpp can check arithmetic whose only other home is inside
// a _draw -- and a _draw cannot run in a unit test, because raylib's drawing
// reaches into a render batch that InitWindow() creates and calling it without
// one is a segfault rather than a failed assertion. Splitting the pure half out
// is the same seam rmp::ui and the animation clock already have.
// ---------------------------------------------------------------------------

#include <raylib.h>

#include <initializer_list>
#include <string_view>

namespace rmp::behavior::detail {

// The arithmetic every behavior shares, defined in src/rmp/behaviors.cpp.

inline constexpr float NEAR_ZERO = 0.0001f;

float length_of(Vector2 v);
bool finite(Vector2 v);
// The direction of `v`, or {0, 0} for one too short to have one or that is
// not a number.
Vector2 normalised(Vector2 v);
// The seconds a hook was handed, as a behavior counts them: a finite number, 0
// or more; anything else is 0, said once.
float seconds_of(float delta);
// Writes `value` into `slot` when it is a finite number; otherwise leaves the
// slot and says so once, for `who`.
void put(float &slot, float value, std::string_view who);
void put(Vector2 &slot, Vector2 value, std::string_view who);

// A field the game set, and its name.
struct Field {
    std::string_view name;
    float value;
};
// Says once, by name, each of `fields` that is NaN.
void say_nan(std::string_view who, std::initializer_list<Field> fields);
// `current` moved towards `target` at `rate` per second; straight there at 0.
float approach(float current, float target, float rate, float delta);

// The most calls one Timer makes in one frame. More owed than that is an
// interval far shorter than any frame, and the rest of the time is dropped.
inline constexpr int MAX_TIMEOUTS = 1000;

// Where a Parallax layer's copies go. The twenty lines everybody writes wrong
// the first time: one copy leaves a gap the moment the offset passes the
// texture's width, and a second copy placed a pixel out is a seam that crawls
// sideways across the screen forever.
struct ParallaxTiling {
    float offset = 0; // the FIRST copy's left edge, always in (-width, 0]
    int copies = 0; // how many to draw to cover `screen`
};

// How far a layer is shifted before the tiling: where the object is relative
// to the view's left edge, times `factor`, plus the layer's own drift. Factor 1
// moves the layer exactly as the world moves under the view -- it stands still
// in the world -- and factor 0 does not move it at all: it stays put on the
// screen, carried along with the camera.
float parallax_shift(float x, float view_x, float factor, float scroll);

// The most copies of one layer a frame draws.
inline constexpr int MAX_PARALLAX_COPIES = 4096;

// `shift` is what parallax_shift() gives. A width of zero or less gives no
// copies rather than a division by it.
ParallaxTiling parallax_tiling(float shift, float width, float screen);

} // namespace rmp::behavior::detail
