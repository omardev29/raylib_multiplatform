// ---------------------------------------------------------------------------
// rmp::Camera: following, limits, and the two conversions.
//
// Small on purpose. The parts that are easy to get wrong -- the clamp that
// has to happen AFTER the follow, the limit narrower than the view -- are
// here; the parts that are not here yet (smoothing with delta in the exponent,
// a shake that never touches position) are phase 11's.
// ---------------------------------------------------------------------------

#include <rmp/scene.h>

#include "object_internal.h"

#include <raylib.h>

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
    return Camera2D{ .offset = Vector2{ screen.x / 2, screen.y / 2 },
                     .target = position,
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

void Camera::detail_settle() {
    if (const Object *target = follow.get()) position = target->position;

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
}

} // namespace rmp
