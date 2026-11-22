// ---------------------------------------------------------------------------
// examples/ui/01_menu/src/main.cpp
//
// rmp::ui, from the three-line version to the parts you reach for later.
//
// The API is deliberately small: begin, end, button, text. Everything that
// would normally make a UI tedious -- coordinates, sizes, fonts, hitboxes,
// hover states, what happens when the window is resized -- is decided for you,
// and each of those decisions can be overridden individually when you need it.
//
// One scene per screen under src/scenes/; this file only says which comes first.
// Run it: `rmp example 01_menu`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>

#include "scenes/screens.h"

RMP_GAME(MainMenuScene);

// ---------------------------------------------------------------------------
// Things worth knowing, none of which you need on day one
// ---------------------------------------------------------------------------
//
// THEME. Plain data. Copy, change, set back — usually once, in a _ready():
//
//     auto t = rmp::ui::current_theme();
//     t.primary       = GOLD;
//     t.corner_radius = 0;          // angular instead of rounded
//     rmp::ui::set_theme(t);
//
// SCALE. Every Theme metric is in design units, at the [window] resolution
// from raylib_multiplatform.toml, and multiplied by rmp::ui::scale() before it
// is drawn. That is why this menu looks right on a phone and on a 4K monitor
// without a single conditional. To pin it — an "interface Size" setting, say:
//
//     rmp::ui::set_scale(1.5f);     // and 0 puts it back to automatic
//
// FONT. [ui] font = "ui.ttf" in the toml and every widget picks it up. Left
// empty you get the font built into raylib: no asset, no licence, works
// everywhere, and its scale is rounded to whole numbers so it stays sharp.
//
// TOUCH. This file already works on Android and iOS. Buttons are never shorter
// than the theme's min_touch_size, and hover is suppressed when nothing is
// touching the screen, so nothing stays lit up after a tap.
//
// ONE FRAME BEHIND. A button cannot be clicked on the very first frame it
// appears — 16 ms at 60 fps. It is inherent to immediate mode and the
// alternative is worse; TECHNICAL.md explains why.
