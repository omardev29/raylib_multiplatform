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

// Where a view of `view_w` x `view_h` centred on `p` may be so that it shows
// nothing outside `limits`, one axis at a time: a zero width or height means
// "unbounded on this axis", which is what an endless runner wants (follow x,
// pin y). A limit narrower than the view has no position that shows nothing
// outside it, so the view is centred on it instead -- the alternative,
// showing the void on one side, is the black strip this exists to prevent.
Vector2 clamp_to(Vector2 p, const Rectangle &limits, float view_w, float view_h) {
    const auto axis = [](float at, float lo_edge, float size, float view) {
        if (!(size > 0)) return at;
        if (size <= view) return lo_edge + size / 2;
        const float lo = lo_edge + view / 2;
        const float hi = lo_edge + size - view / 2;
        return at < lo ? lo : (at > hi ? hi : at);
    };
    return Vector2{ axis(p.x, limits.x, limits.width, view_w),
                    axis(p.y, limits.y, limits.height, view_h) };
}

} // namespace

Camera2D Camera::raylib() const {
    const Vector2 screen = screen_size();
    // The shake goes HERE and in nothing that gameplay reads: this is what is
    // drawn, and to_screen/to_world below go through it so a click lands on
    // what is under the pointer. view() and position do not see it. It is
    // clamped to the limits like the position is -- "never show outside this
    // rectangle" holds while shaking too -- so near an edge only the part of
    // the shake that points inwards shows.
    const Rectangle v = view();
    const Vector2 shaken =
        clamp_to(Vector2{ position.x + shake_now_.x, position.y + shake_now_.y }, limits,
                 v.width, v.height);
    return Camera2D{ .offset = Vector2{ screen.x / 2, screen.y / 2 },
                     .target = shaken,
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
    // Also rejects NaN, and infinity: an infinite strength would put NaN into
    // the drawn camera for the length of the shake.
    if (!(strength > 0) || !(seconds > 0) || !std::isfinite(strength) ||
        !std::isfinite(seconds)) {
        return;
    }
    // The stronger of the two, never the sum: ten hits in a frame must not
    // build a shake that throws the screen across the room. "Stronger" is
    // measured against what the running shake is doing NOW -- its strength
    // times the square of the time left, the same envelope that moves the
    // screen -- so a hit landing on the tail of a big shake is felt.
    const float remaining = shake_seconds_ - shake_elapsed_;
    const float left =
        (shake_seconds_ > 0 && remaining > 0) ? remaining / shake_seconds_ : 0.0f;
    if (strength < shake_strength_ * left * left) return;
    // And a short strong hit does not cut a long shake short: it lasts at
    // least as long as what was left of the one it replaces.
    shake_strength_ = strength;
    shake_seconds_ = remaining > seconds ? remaining : seconds;
    shake_elapsed_ = 0;
}

void Camera::detail_settle(float delta) {
    // A negative or NaN delta advances nothing. The clock seam clamps it, but
    // a test or a game driving this by hand should not be able to run the
    // smoothing backwards.
    const float dt = delta > 0 ? delta : 0.0f;

    if (const Object *target = follow.get()) {
        const bool new_target = !(follow == followed_);
        // An infinite rate is a snap, and must not become -inf * 0 = NaN on a
        // frame with no time in it.
        if (smoothing > 0 && std::isfinite(smoothing) && !new_target) {
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

    // Limits win over follow -- see clamp_to() for the rule, which raylib()
    // applies to the shaken view as well.
    const Rectangle v = view();
    position = clamp_to(position, limits, v.width, v.height);

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
