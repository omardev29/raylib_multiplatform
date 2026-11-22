// ---------------------------------------------------------------------------
// examples/ui/12_font/src/main.cpp
//
// A menu in a font of your own: a title screen, its options and its credits,
// one scene each, every word in Hack Bold.
//
// One call puts it on everything rmp::ui draws, from the title down to a
// dropdown's items:
//
//     rmp::ui::set_font(rmp::assets::load_font("fonts/hack-bold.ttf", 20));
//
// The size you load it at does not limit the sizes it is drawn at. The UI
// keeps the font's FILE and bakes it again at each size it needs, so the
// title at 48 is as sharp as the buttons at 20 -- and the letters a language
// needs come when they are first drawn, not before.
//
// [ui] font in the .toml does the same for a whole game, from the first
// frame. set_font() is for when it changes: a setting, a language, a scene of
// its own. The examples share one .toml, so this one calls it.
//
// One scene per screen under src/scenes/, change<>() between them.
// Run it: `rmp example 12_font`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>

#include "scenes/scenes.h"

RMP_GAME(TitleScene);
