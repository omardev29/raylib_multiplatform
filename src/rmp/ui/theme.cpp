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
// Which one starts is [ui] Theme in raylib_multiplatform.toml. Everything
// after that is a runtime call, so an in-game appearance setting is one line.
// ===========================================================================

#include <rmp/ui.h>

#include <rmp/config.h>

// So this still compiles against a generated header from before [ui] Theme
// existed. tools/configure.py always provides it now.
#ifndef APP_UI_THEME
#define APP_UI_THEME "dark"
#endif

namespace rmp::ui {

namespace {

// The active Theme lives inside a function so that it is constructed on first
// use rather than at load time. current_theme() is reachable from a static
// constructor in the user's code, and a global would make that a coin flip.
Theme &active() {
    static Theme t = (APP_UI_THEME[0] == 'l') ? theme_light() : theme_dark();
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

const Theme &current_theme() { return active(); }
void set_theme(const Theme &t) { active() = t; }

} // namespace rmp::ui
