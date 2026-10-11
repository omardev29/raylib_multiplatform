// ---------------------------------------------------------------------------
// rmp::Camera: following, limits, and the two conversions.
//
// Small on purpose, and every line of it is one of the things that is easy to
// get wrong: the clamp that has to happen AFTER the follow, the limit narrower
// than the view, smoothing with delta in the exponent rather than a per-frame
// lerp, a new target snapped to rather than glided to, a shake that never
// touches position -- and a number that is not one, NaN or an infinity in
// anything a game hands it, which never reaches what it draws or where it is.
// ---------------------------------------------------------------------------

#include <rmp/scene.h>

#include "internal.h"

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

namespace rmp {

namespace {

// Where a camera nobody touched looks -- the middle of the design -- and where
// a position that is not a number goes back to.
constexpr Vector2 HOME{ RMP_WINDOW_WIDTH / 2.0f, RMP_WINDOW_HEIGHT / 2.0f };

// How far a zoom goes either way: 1024 times bigger, or 1024 times smaller.
// Past that a float cannot draw it -- the largest zoom times the stretch's
// scale is infinite, and the smallest makes the view an infinite rectangle --
// and no game means it.
constexpr float MIN_ZOOM = 1.0f / 1024;
constexpr float MAX_ZOOM = 1024;
constexpr float INF = std::numeric_limits<float>::infinity();

// A number the game handed the camera -- a field it set, an argument it
// passed, the position of the object it follows -- that is not finite: NaN or
// an infinity, a mistake in the game's arithmetic. Said once per field, and
// the camera carries on with `instead`. Never from a read: raylib(), view()
// and the conversions only draw from the same safe values, in silence.
void not_finite(const std::string_view what, const std::string_view instead) {
    RMP_REPORT_ONCE_KEYED(
        std::string(what).c_str(),
        "rmp::Camera: %.*s is not a finite number (NaN or an infinity), "
        "so %.*s.",
        static_cast<int>(what.size()), what.data(), static_cast<int>(instead.size()),
        instead.data());
}

bool finite(const Vector2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

// The zoom the camera draws with. One that is not positive is 1, as it always
// was, and so is one that is not a number; one past MIN_ZOOM or MAX_ZOOM stops
// there.
constexpr float usable_zoom(const float zoom) {
    if (!(zoom > 0) || zoom == INF) return 1.0f;
    return std::clamp(zoom, MIN_ZOOM, MAX_ZOOM);
}

// How many pixels one world unit is on screen: the camera's own zoom on top of
// the scale [window] stretch gives the design -- 1 under NONE, so there the
// zoom is the camera's alone, as it always was.
constexpr float pixels_per_unit(const float zoom, const float scale) {
    return usable_zoom(zoom) * scale;
}

// The position the camera draws from: an axis that is not a number is the
// design's middle on that axis -- what detail_settle() writes back, and says.
Vector2 usable_position(const Vector2 p) {
    return Vector2{ std::isfinite(p.x) ? p.x : HOME.x,
                    std::isfinite(p.y) ? p.y : HOME.y };
}

// A rotation that is not a number draws unturned.
float usable_rotation(const float degrees) {
    return std::isfinite(degrees) ? degrees : 0.0f;
}

// What a camera at `centre` shows of the world: the game's area on the screen,
// in world units. Under NONE the area is the whole window and this is the
// window divided by the zoom; under LETTERBOX it is the design divided by the
// zoom whatever the window; under EXPAND the design, and more of the world the
// way the window is longer than the design's shape.
Rectangle view_around(const Vector2 centre, const Rectangle &area, const float ppu) {
    const float w = area.width / ppu;
    const float h = area.height / ppu;
    return Rectangle{ centre.x - w / 2, centre.y - h / 2, w, h };
}

// Where a view of `view_w` x `view_h` centred on `p` may be so that it shows
// nothing outside `limits`, one axis at a time: a zero width or height means
// "unbounded on this axis", which is what an endless runner wants (follow x,
// pin y). A limit narrower than the view has no position that shows nothing
// outside it, so the view is centred on it instead -- the alternative,
// showing the void on one side, is the black strip this exists to prevent.
// An axis whose edge or size is not a number bounds nothing: a NaN edge used
// to put NaN into the position, and an infinite one infinity.
Vector2 clamp_to(Vector2 p, const Rectangle &limits, float view_w, float view_h) {
    const auto axis = [](float at, float lo_edge, float size, float view) {
        if (!(size > 0) || !std::isfinite(size) || !std::isfinite(lo_edge)) return at;
        if (size <= view) return lo_edge + size / 2;
        const float lo = lo_edge + view / 2;
        const float hi = lo_edge + size - view / 2;
        return at < lo ? lo : (at > hi ? hi : at);
    };
    return Vector2{ axis(p.x, limits.x, limits.width, view_w),
                    axis(p.y, limits.y, limits.height, view_h) };
}

bool limits_finite(const Rectangle &r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
        std::isfinite(r.height);
}

// One axis of the shaken view: the shake on top of the position, or none at
// all where the two add up past what a float holds.
float shaken(const float at, const float by) {
    const float sum = at + by;
    return std::isfinite(sum) ? sum : at;
}

// One axis of a smoothed follow: `t` of the way from `from` to `to`. The whole
// way is `to` itself -- the float sum rounds, and from -25421.14 to a target
// at -399.7803 it landed at -399.7793, a thousandth PAST it, and from 1e30 to
// -0.06 on 0 -- and a way longer than a float holds, which made the camera
// infinite, is summed in double. Short of the whole way the float step is
// the one it always was (and never passes `to`: it falls short of it unless t
// is exactly 1), so a camera that met neither draws exactly what it drew.
float approach(const float from, const float to, const float t) {
    if (t >= 1) return to;
    const float at = from + (to - from) * t;
    if (std::isfinite(at)) return at;
    return static_cast<float>(from + ((static_cast<double>(to) - from) * t));
}

} // namespace

Camera2D Camera::raylib() const {
    // Where on the screen the world goes, and at what scale: the window at 1:1
    // under NONE, the design fitted into it otherwise. The design size at 1:1
    // with no window (tests, and the first frame).
    const rmp::app::detail::WorldViewport fit = rmp::app::detail::world_viewport();
    const float ppu = pixels_per_unit(zoom, fit.scale);
    // A read: a field that is not a number is drawn from its safe value here,
    // and said by the next detail_settle(), never by this.
    const Vector2 at = usable_position(position);
    // The shake goes HERE and in nothing that gameplay reads: this is what is
    // drawn, and to_screen/to_world below go through it so a click lands on
    // what is under the pointer. view() and position do not see it. It is
    // clamped to the limits like the position is -- "never show outside this
    // rectangle" holds while shaking too -- so near an edge only the part of
    // the shake that points inwards shows.
    const Rectangle v = view_around(at, fit.area, ppu);
    const Vector2 drawn =
        clamp_to(Vector2{ shaken(at.x, _shake_now.x), shaken(at.y, _shake_now.y) },
                 limits, v.width, v.height);
    // The position at the middle of the game's area -- of the window, unless
    // LETTERBOX left bars -- so the stretch moves and scales the world and
    // nothing else, and to_screen()/to_world() carry it with them.
    return Camera2D{ .offset = Vector2{ fit.area.x + fit.area.width / 2,
                                        fit.area.y + fit.area.height / 2 },
                     .target = drawn,
                     .rotation = usable_rotation(rotation),
                     .zoom = ppu };
}

Rectangle Camera::view() const {
    const rmp::app::detail::WorldViewport fit = rmp::app::detail::world_viewport();
    return view_around(usable_position(position), fit.area,
                       pixels_per_unit(zoom, fit.scale));
}

Vector2 Camera::to_screen(Vector2 world) const {
    return GetWorldToScreen2D(world, raylib());
}

Vector2 Camera::to_world(Vector2 screen) const {
    return GetScreenToWorld2D(screen, raylib());
}

void Camera::shake(float strength, float seconds) {
    // NaN and the infinities are refused, and said: an infinite strength
    // would put NaN into the drawn camera for the length of the shake. Zero
    // and less is nothing to shake, refused in silence.
    if (!std::isfinite(strength)) {
        not_finite("a shake's strength", "the shake is ignored");
        return;
    }
    if (!std::isfinite(seconds)) {
        not_finite("a shake's duration", "the shake is ignored");
        return;
    }
    if (!(strength > 0) || !(seconds > 0)) return;
    // The stronger of the two, never the sum: ten hits in a frame must not
    // build a shake that throws the screen across the room. "Stronger" is
    // measured against what the running shake is doing NOW -- its strength
    // times the square of the time left, the same envelope that moves the
    // screen -- so a hit landing on the tail of a big shake is felt.
    const float remaining = _shake_seconds - _shake_elapsed;
    const float left =
        (_shake_seconds > 0 && remaining > 0) ? remaining / _shake_seconds : 0.0f;
    if (strength < _shake_strength * left * left) return;
    // And a short strong hit does not cut a long shake short: it lasts at
    // least as long as what was left of the one it replaces.
    _shake_strength = strength;
    _shake_seconds = remaining > seconds ? remaining : seconds;
    _shake_elapsed = 0;
}

void Camera::detail_settle(float delta) {
    // A negative or NaN delta advances nothing. The clock seam clamps it, but
    // a test or a game driving this by hand should not be able to run the
    // smoothing backwards. Not reported: the delta is the framework's.
    const float dt = delta > 0 ? delta : 0.0f;

    // What the game set and the camera cannot use, said once each. zoom and
    // rotation stay as the game left them -- every read draws from their safe
    // values -- but a position is the camera's own state, which follow and
    // smoothing build on: one that is not a number is put back where a camera
    // nobody touched looks, or, below, where its target is.
    if (!std::isfinite(zoom)) not_finite("zoom", "it draws at zoom 1");
    if (!std::isfinite(rotation)) not_finite("rotation", "it draws unturned");
    if (!limits_finite(limits)) not_finite("a limit", "that axis is unbounded");
    // An infinite rate is a snap, as it always was; NaN and minus infinity
    // are no rate at all, and snap too.
    if (std::isnan(smoothing) || smoothing == -INF) {
        not_finite("smoothing", "it snaps to its target");
    }
    const bool lost_x = !std::isfinite(position.x);
    const bool lost_y = !std::isfinite(position.y);
    if (lost_x || lost_y) {
        not_finite("position",
                   "it goes back to the middle of the design, or to its target");
        position = usable_position(position);
    }

    if (const Ref<Object> target = follow.get()) {
        const bool new_target = !(follow == _followed);
        const Vector2 to = target->position;
        // An axis of the target that is not a number is not followed: the
        // camera waits where it is, and a smoothed one that took it in stayed
        // NaN for good -- NaN plus anything is NaN, even once the target was
        // back.
        if (!finite(to)) {
            not_finite("the followed object's position", "the camera does not follow it");
        }
        // An infinite rate is a snap, and must not become -inf * 0 = NaN on a
        // frame with no time in it.
        if (smoothing > 0 && std::isfinite(smoothing) && !new_target) {
            // Delta in the EXPONENT. The fraction of the remaining distance
            // covered is 1 - exp(-rate * dt), which composes exactly: two
            // frames of dt/2 land where one frame of dt does. A per-frame
            // lerp factor does not, and follows twice as fast at 120 Hz.
            // A camera that was nowhere joins its target at once.
            const float t = 1.0f - std::exp(-smoothing * dt);
            if (std::isfinite(to.x))
                position.x = lost_x ? to.x : approach(position.x, to.x, t);
            if (std::isfinite(to.y))
                position.y = lost_y ? to.y : approach(position.y, to.y, t);
        } else {
            if (std::isfinite(to.x)) position.x = to.x;
            if (std::isfinite(to.y)) position.y = to.y;
        }
        _followed = follow;
    } else {
        _followed = Handle<Object>();
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
    if (_shake_seconds > 0) {
        _shake_elapsed += dt;
        if (_shake_elapsed >= _shake_seconds) {
            _shake_strength = 0;
            _shake_seconds = 0;
            _shake_elapsed = 0;
            _shake_now = Vector2{ 0, 0 };
        } else {
            const float left = 1.0f - _shake_elapsed / _shake_seconds;
            const float amplitude = _shake_strength * left * left;
            const float e = _shake_elapsed;
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
            _shake_now = Vector2{ sx * scale, sy * scale };
        }
    }
}

} // namespace rmp
