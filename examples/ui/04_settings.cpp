// ---------------------------------------------------------------------------
// examples/ui/04_settings.cpp
//
// A settings screen: checkbox, slider, dropdown, text input — and the thing
// that makes all four playable on a TV with a controller, which is focus.
//
// THE STATE MODEL, because it is the part that surprises people coming from a
// retained-mode toolkit: every control takes a pointer to YOUR variable and
// writes to it. There is no widget object holding a copy, nothing to
// synchronise, no setter to remember. What is on screen is what is in your
// struct, because it was read this frame. Delete this file and your settings
// still exist; they were never ours.
//
// Each one returns true on the frame it changed, so "apply when something
// changes" reads exactly like that:
//
//     if (rmp::ui::checkbox("Fullscreen", &cfg.fullscreen)) apply(cfg);
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/ui.h>

#include <cstring>

// Plain data. This is the whole model.
struct Settings {
    bool fullscreen = false;
    bool vsync = true;
    bool subtitles = false;
    float master = 0.8f;
    float music = 0.5f;
    float sensitivity = 0.35f;
    int quality = 1;
    int language = 0;
    char player[24] = "Player";
};

static Settings cfg;
static Settings saved; // what was on disk, to know if anything changed
static bool dirty = false;

static const char *QUALITY[] = { "Low", "Medium", "High", "Ultra" };
static const char *LANGUAGE[] = { "English", "Espanol", "Francais" };

static void apply(const Settings &s) {
    // Where you would actually act on it. Called only when something changed,
    // which is why the controls return a bool at all.
    if (s.vsync)
        SetTargetFPS(60);
    else
        SetTargetFPS(0);
    TraceLog(LOG_INFO, "SETTINGS: applied (master %.2f, quality %s)", (double)s.master,
             QUALITY[s.quality]);
}

static void on_ready() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
    saved = cfg;

    // Put the focus somewhere when the screen opens. Without this a controller
    // arrives at a screen with nothing selected and the first press does
    // nothing, which reads as "the menu is broken".
    rmp::ui::focus("Fullscreen");
}

static void on_frame(float delta) {
    BeginDrawing();
    ClearBackground(rmp::ui::current_theme().background);

    rmp::ui::begin();
    rmp::ui::panel({ .box = { .width = 420 } }, [&] {
        rmp::ui::text("Settings");

        // --- toggles ------------------------------------------------------
        if (rmp::ui::checkbox("Fullscreen", &cfg.fullscreen)) dirty = true;
        if (rmp::ui::checkbox("VSync", &cfg.vsync)) dirty = true;

        // A control that is not available right now is disabled, not missing.
        // A menu whose items appear and disappear is a menu nobody can learn.
        rmp::ui::checkbox("Subtitles", &cfg.subtitles, { .enabled = false });

        // --- sliders ------------------------------------------------------
        // Continuous: drag it anywhere, or hold left/right on a controller.
        if (rmp::ui::slider("Master volume", &cfg.master, 0.0f, 1.0f)) dirty = true;
        if (rmp::ui::slider("Music", &cfg.music, 0.0f, 1.0f)) dirty = true;

        // step snaps to multiples, which is what you want for a value the
        // player will want to describe to someone else ("I play on 40").
        if (rmp::ui::slider("Sensitivity", &cfg.sensitivity, 0.0f, 1.0f,
                            { .step = 0.05f }))
            dirty = true;

        // --- pick one of a list -------------------------------------------
        // The dropdown owns nothing but the open/closed flag, and that is ours,
        // not yours: *selected is an index into the array you passed.
        if (rmp::ui::dropdown("Quality", &cfg.quality, QUALITY, 4)) dirty = true;
        if (rmp::ui::dropdown("Language", &cfg.language, LANGUAGE, 3)) dirty = true;

        // --- typing -------------------------------------------------------
        // Writes into your buffer, NUL-terminated, never past capacity.
        if (rmp::ui::text_input("Name", cfg.player, sizeof(cfg.player),
                                { .placeholder = "your name" })) {
            dirty = true;
        }

        // --- actions ------------------------------------------------------
        rmp::ui::row({ .grow_x = true }, [&] {
            if (rmp::ui::button("Revert", { .enabled = dirty })) {
                cfg = saved;
                dirty = false;
            }
            rmp::ui::spacer();
            if (rmp::ui::button(
                    "Apply", { .style = rmp::ui::Variant::PRIMARY, .enabled = dirty })) {
                apply(cfg);
                saved = cfg;
                dirty = false;
            }
        });

        if (dirty) {
            rmp::ui::text("unsaved changes",
                          { .color = rmp::ui::ColorRole::DANGER, .size = 14 });
        }
    });
    rmp::ui::end();

    EndDrawing();
}

static void on_exit() { CloseWindow(); }

RMP_ENTRY_POINT(on_ready, on_frame, on_exit);

// ---------------------------------------------------------------------------
// Focus, keyboard and gamepad — which you did not have to write
// ---------------------------------------------------------------------------
//
// Everything above is already navigable. Nothing in this file asked for it:
//
//   Tab / Down / d-pad down / left stick    next control
//   Shift+Tab / Up / d-pad up               previous
//   Enter / Space / gamepad bottom button   activate
//   Left / Right / d-pad / stick            move a slider
//
// The focused control draws the theme's focus ring. That colour is in the
// Theme rather than in each widget for a reason: a controller build where one
// widget forgot to draw it is a controller build that gets stuck.
//
//   rmp::ui::focus("Fullscreen")   put the focus somewhere (when a menu opens)
//   rmp::ui::focused()             what has it
//   rmp::ui::set_navigation_enabled(false)   if your game drives focus itself
//
// WHO GETS THE INPUT. The UI reads the pointer and the keyboard itself, so the
// game has to be told to keep its hands off:
//
//     if (!rmp::ui::wants_pointer()  && IsMouseButtonPressed(0)) shoot();
//     if (!rmp::ui::wants_keyboard() && IsKeyDown(KEY_W))        walk();
//
// Without the second one, typing "Wolf" into the name field above walks the
// player across the level. wants_keyboard() is true only while a text field
// has the focus.
