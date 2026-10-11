// ===========================================================================
// The two built-in themes, and the one that is currently in use.
//
// A Theme is data, not behaviour, so there is very little code here. The dark
// theme's values live as member initialisers on rmp::ui::Theme in the public
// header, where someone reading the API can see them; theme_dark() just hands
// back a default-constructed one. theme_light() is that, overwritten.
//
// No inheritance, no cascade, no selectors. Copy it, change what you want, set
// it back:
//
//     auto t = rmp::ui::current_theme();
//     t.primary = GOLD;
//     rmp::ui::set_theme(t);
//
// Which one starts is [ui] theme in raylib_multiplatform.toml. Everything
// after that is a runtime call, so an in-game appearance setting is one line.
// ===========================================================================

#include "../internal.h" // RMP_REPORT_ONCE_KEYED

#include <rmp/ui.h>

#include <rmp/config.h>

#include <cmath>
#include <string>

// The fallback for a translation unit compiled without the generated header;
// tools/configure.py always writes RMP_UI_THEME into it.
#ifndef RMP_UI_THEME
#define RMP_UI_THEME "dark"
#endif

namespace rmp::ui {

namespace {

// The active Theme lives inside a function so that it is constructed on first
// use rather than at load time. current_theme() is reachable from a static
// constructor in the user's code, and a global would make that a coin flip.
Theme &active() {
    static Theme t = (RMP_UI_THEME[0] == 'l') ? theme_light() : theme_dark();
    return t;
}

} // namespace

// Dark. It is what a game expects, it sits on top of any background, and it
// forgives a contrast ratio nobody has measured yet.
Theme theme_dark() { return Theme{}; }

// Light. Not the dark one with the numbers flipped: a light interface needs
// separation the dark one gets for free from its own shadows, which is why
// border_width is 1 here and 0 there. Every surface gets an outline, so a pale
// button on a pale page is still a button.
Theme theme_light() {
    Theme t{};

    t.background = Color{ 246, 246, 249, 255 };
    t.panel = Color{ 255, 255, 255, 255 };
    t.surface = Color{ 233, 233, 240, 255 };
    t.surface_hover = Color{ 220, 220, 230, 255 };
    t.surface_press = Color{ 203, 203, 216, 255 };
    t.border = Color{ 199, 199, 212, 255 };

    t.text = Color{ 26, 26, 33, 255 };
    t.text_muted = Color{ 94, 94, 110, 255 };
    t.text_on_accent = Color{ 255, 255, 255, 255 };

    // Darker than the dark theme's accents on purpose: the same blue that
    // reads as bright on near-black reads as washed out on near-white, and
    // white label text on it stops being legible.
    t.primary = Color{ 46, 86, 214, 255 };
    t.primary_hover = Color{ 62, 104, 236, 255 };
    t.primary_press = Color{ 34, 68, 178, 255 };

    t.danger = Color{ 196, 42, 54, 255 };
    t.danger_hover = Color{ 218, 60, 72, 255 };
    t.danger_press = Color{ 162, 30, 40, 255 };

    t.disabled = Color{ 228, 228, 235, 255 };
    t.disabled_text = Color{ 154, 154, 168, 255 };

    t.focus = Color{ 38, 84, 210, 255 };

    // The one metric that differs, and the reason the light Theme works at all.
    t.border_width = 1;

    return t;
}

namespace {

// The most a metric of a theme can be, in design units: a gap or a font of
// four thousand is a screen of nothing already. Seconds, for `transition`.
constexpr float MOST_UNITS = 4096.0f;
constexpr float MOST_SECONDS = 60.0f;

// A metric that is not one -- NaN, below 0, an infinity, past MOST -- is the
// game's mistake: said once by its name, and the dark theme's in its place, so
// the interface goes on being laid out and drawn.
void keep(float &metric, float fallback, float most, const std::string &name) {
    if (metric >= 0.0f && metric <= most) return;
    RMP_REPORT_ONCE_KEYED(name.c_str(),
                          "UI: set_theme() with %s = %g, which is not between 0 and %g; "
                          "%g is used",
                          name.c_str(), static_cast<double>(metric),
                          static_cast<double>(most), static_cast<double>(fallback));
    metric = fallback;
}

} // namespace

const Theme &current_theme() { return active(); }

void set_theme(const Theme &t) {
    Theme kept = t;
    const Theme dark = theme_dark();
    keep(kept.font_size, dark.font_size, MOST_UNITS, "font_size");
    keep(kept.font_size_small, dark.font_size_small, MOST_UNITS, "font_size_small");
    keep(kept.font_size_large, dark.font_size_large, MOST_UNITS, "font_size_large");
    keep(kept.padding_x, dark.padding_x, MOST_UNITS, "padding_x");
    keep(kept.padding_y, dark.padding_y, MOST_UNITS, "padding_y");
    keep(kept.gap, dark.gap, MOST_UNITS, "gap");
    keep(kept.panel_padding, dark.panel_padding, MOST_UNITS, "panel_padding");
    keep(kept.corner_radius, dark.corner_radius, MOST_UNITS, "corner_radius");
    keep(kept.border_width, dark.border_width, MOST_UNITS, "border_width");
    keep(kept.control_size, dark.control_size, MOST_UNITS, "control_size");
    keep(kept.track_thickness, dark.track_thickness, MOST_UNITS, "track_thickness");
    keep(kept.focus_ring, dark.focus_ring, MOST_UNITS, "focus_ring");
    keep(kept.transition, dark.transition, MOST_SECONDS, "transition");
    keep(kept.min_touch_size, dark.min_touch_size, MOST_UNITS, "min_touch_size");
    active() = kept;
}

} // namespace rmp::ui
