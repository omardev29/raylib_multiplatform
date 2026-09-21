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
// One screen per file under src/screens/; this file only says which one is on.
// Built and booted by CI on every push, and by `just example 01_menu` here.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/ui.h>

#include "screens.h"

static void on_ready() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE); // BEFORE InitWindow, or it is ignored
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
}

static void on_frame(float delta) {
    (void)delta;
    if (g_state.screen == Screen::PLAYING) {
        g_state.score += 1;
        if (IsKeyPressed(KEY_ESCAPE)) g_state.screen = Screen::MENU;
    }

    BeginDrawing();
    ClearBackground(rmp::ui::current_theme().background);

    // The UI draws in end(), so the whole pair belongs between BeginDrawing()
    // and EndDrawing() -- and after whatever you want it to sit on top of.
    switch (g_state.screen) {
        case Screen::MENU:
            main_menu();
            break;
        case Screen::OPTIONS:
            options_menu();
            break;
        case Screen::CONFIRM:
            confirm_quit();
            break;
        case Screen::PLAYING:
            hud();
            break;
    }

    EndDrawing();
}

static void on_exit() { CloseWindow(); }

RMP_ENTRY_POINT(on_ready, on_frame, on_exit);

// ---------------------------------------------------------------------------
// Things worth knowing, none of which you need on day one
// ---------------------------------------------------------------------------
//
// THEME. Plain data. Copy, change, set back — usually once, in on_ready():
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
