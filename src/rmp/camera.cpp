// ---------------------------------------------------------------------------
// rmp::Camera: following, limits, and the two conversions.
//
// Small on purpose, and every line of it is one of the things that is easy to
// get wrong: the clamp that has to happen AFTER the follow, the limit narrower
// than the view, smoothing with delta in the exponent rather than a per-frame
// lerp, a new target snapped to rather than glided to, and a shake that never
// touches position.
// ---------------------------------------------------------------------------

#include <rmp/scene.h>

#include "object_internal.h"

#include <raylib.h>

#include <cmath>

namespace rmp {

namespace {

// The screen the camera projects onto: the window when there is one, the
// design size when there is not (tests, and the first frame).
Vector2 screen_size() {
    const Rectangle r = rmp::objects::detail::view_rect();
    return Vector2{ r.width, r.height };
}

} // namespace

Camera2D Camera::raylib() const {
    const Vector2 screen = screen_size();
    // The shake goes HERE and in nothing that gameplay reads: this is what is
    // drawn, and to_screen/to_world below go through it so a click lands on
    // what is under the pointer. view() and position do not see it.
    return Camera2D{ .offset = Vector2{ screen.x / 2, screen.y / 2 },
                     .target =
                         Vector2{ position.x + shake_now_.x, position.y + shake_now_.y },
                     .rotation = rotation,
                     .zoom = zoom > 0 ? zoom : 1.0f };
}

Rectangle Camera::view() const {
    const Vector2 screen = screen_size();
    const float z = zoom > 0 ? zoom : 1.0f;
    const float w = screen.x / z;
    const float h = screen.y / z;
    return Rectangle{ position.x - w / 2, position.y - h / 2, w, h };
}

Vector2 Camera::to_screen(Vector2 world) const {
    return GetWorldToScreen2D(world, raylib());
}

Vector2 Camera::to_world(Vector2 screen) const {
    return GetScreenToWorld2D(screen, raylib());
}

void Camera::shake(float strength, float seconds) {
    if (!(strength > 0) || !(seconds > 0)) return; // also rejects NaN
    // The stronger of the two, never the sum: ten hits in a frame must not
    // build a shake that throws the screen across the room. And a strong
    // shake is not cut short by a weak one arriving late.
    const float remaining = shake_seconds_ - shake_elapsed_;
    const float current = (shake_seconds_ > 0 && remaining > 0)
        ? shake_strength_ * (remaining / shake_seconds_)
        : 0.0f;
    if (strength < current) return;
    shake_strength_ = strength;
    shake_seconds_ = seconds;
    shake_elapsed_ = 0;
}

void Camera::detail_settle(float delta) {
    // A negative or NaN delta advances nothing. The clock seam clamps it, but
    // a test or a game driving this by hand should not be able to run the
    // smoothing backwards.
    const float dt = delta > 0 ? delta : 0.0f;

    if (const Object *target = follow.get()) {
        const bool new_target = !(follow == followed_);
        if (smoothing > 0 && !new_target) {
            // Delta in the EXPONENT. The fraction of the remaining distance
            // covered is 1 - exp(-rate * dt), which composes exactly: two
            // frames of dt/2 land where one frame of dt does. A per-frame
            // lerp factor does not, and follows twice as fast at 120 Hz.
            const float t = 1.0f - std::exp(-smoothing * dt);
            position.x += (target->position.x - position.x) * t;
            position.y += (target->position.y - position.y) * t;
        } else {
            position = target->position;
        }
        followed_ = follow;
    } else {
        followed_ = Handle<Object>();
    }

    const Rectangle v = view();
    // Limits win over follow, one axis at a time: a zero width or height
    // means "unbounded on this axis", which is what an endless runner wants
    // (follow x, pin y) and used to need a made-up level length to say. A
    // limit narrower than the view has no position that shows nothing outside
    // it, so the view is centred on it instead -- the alternative, showing the
    // void on one side, is the black strip this exists to prevent.
    if (limits.width > 0) {
        if (limits.width <= v.width) {
            position.x = limits.x + limits.width / 2;
        } else {
            const float lo = limits.x + v.width / 2;
            const float hi = limits.x + limits.width - v.width / 2;
            position.x = position.x < lo ? lo : (position.x > hi ? hi : position.x);
        }
    }
    if (limits.height > 0) {
        if (limits.height <= v.height) {
            position.y = limits.y + limits.height / 2;
        } else {
            const float lo = limits.y + v.height / 2;
            const float hi = limits.y + limits.height - v.height / 2;
            position.y = position.y < lo ? lo : (position.y > hi ? hi : position.y);
        }
    }

    // The shake last, and apart. Its amplitude falls off with the square of
    // the time left, which reads as a jolt that settles rather than a buzz
    // that stops. The motion is two sines per axis at unrelated frequencies,
    // not random numbers: white noise changes direction every frame and
    // reads as a broken monitor, and a pure function of the elapsed time is
    // also exactly reproducible, so a test can pin it.
    if (shake_seconds_ > 0) {
        shake_elapsed_ += dt;
        if (shake_elapsed_ >= shake_seconds_) {
            shake_strength_ = 0;
            shake_seconds_ = 0;
            shake_elapsed_ = 0;
            shake_now_ = Vector2{ 0, 0 };
        } else {
            const float left = 1.0f - shake_elapsed_ / shake_seconds_;
            const float amplitude = shake_strength_ * left * left;
            const float e = shake_elapsed_;
            const float sx =
                0.6f * std::sin(e * 47.0f) + 0.4f * std::sin(e * 83.0f + 1.3f);
            const float sy =
                0.6f * std::sin(e * 59.0f + 0.7f) + 0.4f * std::sin(e * 97.0f);
            // Each axis reaches +-1 on its own, so together the offset could
            // reach sqrt(2) times the strength -- 41 % more than the call asked
            // for, which the test for the bound caught. Clamped to the length,
            // so "up to `strength`" is true in every direction.
            const float len = std::sqrt(sx * sx + sy * sy);
            const float scale = len > 1.0f ? amplitude / len : amplitude;
            shake_now_ = Vector2{ sx * scale, sy * scale };
        }
    }
}

} // namespace rmp
