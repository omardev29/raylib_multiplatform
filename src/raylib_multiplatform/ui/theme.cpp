// ===========================================================================
// The active theme.
//
// There is very little here because a theme is data, not behaviour: the
// default values live as member initialisers on rmp::ui::theme in the public
// header, where someone reading the API can see them. This file only holds the
// one that is currently in use.
//
// No inheritance, no cascade, no selectors. Copy it, change what you want, set
// it back:
//
//     auto t = rmp::ui::current_theme();
//     t.primary = GOLD;
//     rmp::ui::set_theme(t);
//
// The default is dark, which is what a game expects and what forgives an
// unfinished contrast ratio. A second built-in theme, and a [ui] theme key to
// pick between them, is a later phase — a setting that accepts exactly one
// value is worse than no setting.
// ===========================================================================

#include <raylib_multiplatform/ui.h>

namespace rmp::ui {

namespace {
theme g_theme{};
}

const theme &current_theme() { return g_theme; }
void set_theme(const theme &t) { g_theme = t; }

} // namespace rmp::ui
