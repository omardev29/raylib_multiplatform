// ---------------------------------------------------------------------------
// examples/ui/01_menu.cpp
//
// rmp::ui, from the three-line version to the parts you reach for later.
//
// The API is deliberately small: begin, end, button, text. Everything that
// would normally make a UI tedious — coordinates, sizes, fonts, hitboxes,
// hover states, what happens when the window is resized — is decided for you,
// and each of those decisions can be overridden individually when you need it.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/ui.h>

#include <string>

// Where the game currently is. Note that this is a plain variable of YOURS:
// the UI is immediate mode, so it holds no state and there is nothing to keep
// in sync. What you draw each frame is decided by your own data.
enum class Screen { Menu, Options, Confirm, Playing };

static Screen screen = Screen::Menu;
static int score = 0;
static bool music = true;

static void on_ready() {
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE); // resize it and watch the UI follow
}

// -----------------------------------------------------------------------
// 1. The whole point: a main menu
// -----------------------------------------------------------------------
static void main_menu() {
    // A centred column, sized and spaced by the theme, scaled to the window.
    // Three ifs and you have a menu.
    rmp::ui::begin();

    rmp::ui::text("MY GAME");

    if (rmp::ui::button("Play")) screen = Screen::Playing;
    if (rmp::ui::button("Options")) screen = Screen::Options;
    if (rmp::ui::button("Quit")) screen = Screen::Confirm;

    rmp::ui::end();
}

// -----------------------------------------------------------------------
// 2. One step up: options on the widgets
// -----------------------------------------------------------------------
static void options_menu() {
    rmp::ui::begin();

    rmp::ui::text("Options");

    // A button whose label is built this frame. std::string_view takes it, and
    // the text is copied immediately — temporaries are safe here.
    if (rmp::ui::button(std::string("Music: ") + (music ? "on" : "off"))) {
        music = !music;
    }

    // Semantic variants: you say what the button MEANS, the theme decides what
    // that looks like. Restyling the game never means revisiting this line.
    rmp::ui::button("Reset progress", { .style = rmp::ui::Variant::DANGER });

    // Disabled controls still lay out, and still look deliberate.
    rmp::ui::button("Cloud saves", { .enabled = false });

    if (rmp::ui::button("Back")) screen = Screen::Menu;

    rmp::ui::end();
}

// -----------------------------------------------------------------------
// 3. Two buttons with the same label, in different screens
// -----------------------------------------------------------------------
static void confirm_quit() {
    rmp::ui::begin();

    rmp::ui::text("Really quit?");
    rmp::ui::text("Progress since the last save will be lost.",
                  { .color = rmp::ui::ColorRole::MUTED });

    // rmp::app::quit(), never std::exit(): this lets the frame finish and
    // then runs on_exit() and CloseWindow() on the way out. On Android it also
    // finishes the Activity, so the app does not leave a dead task behind.
    //
    // On iOS it does nothing at all, on purpose — Apple rejects apps that
    // terminate themselves. Guarding it out is optional; the call is safe
    // everywhere. Here it is guarded so the button is not dead on iPhone.
#if !defined(PLATFORM_IOS)
    if (rmp::ui::button("Quit", { .style = rmp::ui::Variant::DANGER })) rmp::app::quit();
#endif

    // This screen and the main menu both have a "Back"-ish button. Identical
    // labels inside ONE frame are told apart automatically; an explicit id is
    // for when the UI is conditional — as it is here — and you want an
    // element's hover state to stay its own across screen changes.
    if (rmp::ui::button("Cancel", { .id = "confirm.cancel" })) screen = Screen::Menu;

    rmp::ui::end();
}

// -----------------------------------------------------------------------
// 4. A HUD: not everything is a centred menu
// -----------------------------------------------------------------------
static void hud() {
    // placement moves the whole thing. The nine Align values are the ones you
    // would guess: top_left, top_center, ..., bottom_right.
    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT, .gap = 4 });

    // Built fresh every frame, which is exactly how immediate mode is meant to
    // be used: no label object to update, no "setText" to remember.
    rmp::ui::text("Score: " + std::to_string(score));
    rmp::ui::text("Press ESC for the menu",
                  { .color = rmp::ui::ColorRole::MUTED, .size = 14 });

    rmp::ui::end();
}

static void on_frame(float delta) {
    if (screen == Screen::Playing) {
        score += 1;
        if (IsKeyPressed(KEY_ESCAPE)) screen = Screen::Menu;
    }

    BeginDrawing();
    ClearBackground(rmp::ui::current_theme().background);

    // The UI draws in end(), so the whole pair belongs between BeginDrawing()
    // and EndDrawing() — and after whatever you want it to sit on top of.
    switch (screen) {
        case Screen::Menu:
            main_menu();
            break;
        case Screen::Options:
            options_menu();
            break;
        case Screen::Confirm:
            confirm_quit();
            break;
        case Screen::Playing:
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
