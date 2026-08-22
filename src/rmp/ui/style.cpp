// ===========================================================================
// Style: what a size step resolves to, what colour a control is right now, and
// the transition between the two.
//
// Both halves exist for the same reason — so that no widget has to decide.
// A button that worked out its own hover colour is a button that will disagree
// with the next widget somebody writes, and a widget that eased its own
// transition is a place where the easing can be forgotten. There is one of
// each, here, and every widget calls it.
//
// Nothing in this file moves anything. Sizes resolve before layout, colours
// change after it: a control is never somewhere other than where it was drawn,
// so an animation can never make you miss what you clicked on.
// ===========================================================================

#include "internal.h"

#include <cmath>

namespace rmp::ui::detail {

namespace {

// --- the transition table --------------------------------------------------
//
// Direct-mapped, one slot per key, no probing. A collision means two controls
// share a slot and one of them loses its fade — never correctness, and only
// while both are on screen at once with different states, which in practice
// means one of them is hovered and the other is not. Paying for a real hash
// map to avoid a missing 120 ms fade would be the wrong trade.
constexpr int kSlots = 512;

struct Slot {
    uint32_t key = 0;
    float t = 0.0f;
    bool used = false;
};

Slot g_slots[kSlots];
float g_dt = 0.0f;

// Smoothstep. Linear in, eased out: the value still arrives exactly on time,
// it just does not start and stop abruptly.
float ease(float t) { return t * t * (3.0f - 2.0f * t); }

unsigned char mix8(unsigned char a, unsigned char b, float t) {
    float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return static_cast<unsigned char>(std::lround(v));
}

} // namespace

Color mix_color(Color a, Color b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return Color{ mix8(a.r, b.r, t), mix8(a.g, b.g, t), mix8(a.b, b.b, t),
                  mix8(a.a, b.a, t) };
}

void anim_begin_frame() {
    // In test mode there is no window, so there is no frame time either, and a
    // headless run must produce the same numbers every time it is run.
    g_dt = test_mode() ? 0.0f : GetFrameTime();
    // A Breakpoint, a dropped frame or a window drag can hand us a second and a
    // half. Letting that through makes every transition finish instantly, which
    // is not wrong, but capping it keeps the first frame after a stall looking
    // like the frames around it.
    if (g_dt > 0.1f) g_dt = 0.1f;
    if (g_dt < 0.0f) g_dt = 0.0f;
}

float anim_value(Clay_ElementId id, uint32_t channel, bool on) {
    const float target = on ? 1.0f : 0.0f;
    const float duration = current_theme().transition;

    // transition = 0 is the reduce-motion setting, and it is also what the
    // headless test runs with. No table lookup, no state: the answer is the
    // target.
    if (duration <= 0.0f) return target;

    const uint32_t key = id.id ^ ((channel + 1u) * 2246822519u);
    Slot &s = g_slots[key & (kSlots - 1)];
    if (!s.used || s.key != key) {
        // First sight of this control, or the slot belonged to another one.
        // Start where it is going, so nothing fades in from nowhere the frame
        // it appears.
        s.used = true;
        s.key = key;
        s.t = target;
        return target;
    }

    const float step = (duration > 0.0f) ? (g_dt / duration) : 1.0f;
    if (s.t < target) {
        s.t += step;
        if (s.t > target) s.t = target;
    } else if (s.t > target) {
        s.t -= step;
        if (s.t < target) s.t = target;
    }
    return ease(s.t);
}

Color state_color(Clay_ElementId id, Color base, Color hover, Color press, bool over,
                  bool pressed) {
    // Two channels, not three states, because they overlap: pressing something
    // means the pointer is also over it, and the press has to be able to fade
    // out to hover rather than all the way back to rest.
    const float h = anim_value(id, 0, over);
    const float p = anim_value(id, 1, pressed);
    return mix_color(mix_color(base, hover, h), press, p);
}

// --- sizes -----------------------------------------------------------------

float resolve_size(const Sizing &s, const Theme &t) {
    if (s.named) {
        switch (s.step) {
            case ui::Size::SMALL:
                return t.font_size_small;
            case ui::Size::LARGE:
                return t.font_size_large;
            case ui::Size::MEDIUM:
                break;
        }
        return t.font_size;
    }
    return (s.units > 0.0f) ? s.units : t.font_size;
}

float size_ratio(const Sizing &s, const Theme &t) {
    // Padding and the minimum touch height follow the type, so a large button
    // is a large button all over and not a normal one with bigger letters.
    // Guarded because a theme with font_size = 0 is somebody's typo, not a
    // reason to divide by zero.
    if (t.font_size <= 0.0f) return 1.0f;
    return resolve_size(s, t) / t.font_size;
}

} // namespace rmp::ui::detail
