// ---------------------------------------------------------------------------
// examples/ui/04_settings/src/main.cpp
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
// And they are kept: load() reads them through rmp::save when the screen
// opens, store() writes them on Apply. The Settings struct stays plain data;
// the Value is only the shape it has on disk.
//
// Run it: `rmp example 04_settings`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/audio.h>
#include <rmp/save.h>
#include <rmp/ui.h>

#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <string>

static Settings cfg;
static Settings saved; // what was on disk, to know if anything changed
static bool dirty = false;
static bool damaged = false; // the saved settings were edited or cut short

static void apply(const Settings &s) {
    // Where you would actually act on it. Called only when something changed,
    // which is why the controls return a bool at all.
#if !defined(PLATFORM_WEB) && !defined(__EMSCRIPTEN__)
    // VSync is a window state, on and off at any time. Not on the web: the
    // browser paces the frame there and there is nothing to switch.
    if (s.vsync)
        SetWindowState(FLAG_VSYNC_HINT);
    else
        ClearWindowState(FLAG_VSYNC_HINT);
#endif
    // The two volume sliders are the audio buses. Neither call opens the
    // sound device: a game can show and apply this screen before it has made
    // a sound, and what is already playing changes at once.
    rmp::audio::set_volume(rmp::audio::Bus::MASTER, s.master);
    rmp::audio::set_volume(rmp::audio::Bus::MUSIC, s.music);
    TraceLog(LOG_INFO, "SETTINGS: applied (master %.2f, quality %s)", (double)s.master,
             QUALITY[s.quality]);
}

// What is on disk, over what the struct says. Every field reads with its
// current value as the default, so the first run -- no save yet -- and a save
// from an older version missing a field both simply keep the defaults. There
// is no `if` around the read for the same reason.
static void load(Settings &s) {
    rmp::Value v;
    // When the read fails, `v` stays empty and every field keeps its default
    // -- that is the whole of "the first run". Only a damaged file is worth
    // telling the player about; a missing one is simply the first time.
    const rmp::save::Result read = rmp::save::read("settings", &v);
    damaged = read.status == rmp::save::Status::MODIFIED ||
        read.status == rmp::save::Status::TRUNCATED;
    s.fullscreen = v["fullscreen"].as_bool(s.fullscreen);
    s.vsync = v["vsync"].as_bool(s.vsync);
    s.master = v["master"].as_float(s.master);
    s.music = v["music"].as_float(s.music);
    s.sensitivity = v["sensitivity"].as_float(s.sensitivity);
    // An index from a file is clamped before it indexes anything: a save can
    // be edited, or come from a version with a longer list.
    s.quality = std::clamp(v["quality"].as_int(s.quality), 0, 3);
    s.language = std::clamp(v["language"].as_int(s.language), 0, 2);
    const std::string name(v["player"].as_string(s.player));
    std::snprintf(s.player, sizeof(s.player), "%s", name.c_str());
}

static void store(const Settings &s) {
    rmp::Value v;
    v["fullscreen"] = s.fullscreen;
    v["vsync"] = s.vsync;
    v["master"] = s.master;
    v["music"] = s.music;
    v["sensitivity"] = s.sensitivity;
    v["quality"] = s.quality;
    v["language"] = s.language;
    v["player"] = s.player;
    rmp::save::write("settings", v);
}

static void on_ready() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(RMP_WINDOW_WIDTH, RMP_WINDOW_HEIGHT, RMP_WINDOW_TITLE);
    // The controls start where the game does -- [window] vsync and [audio] in
    // the .toml -- and then whatever the player saved last time wins.
    cfg.vsync = RMP_WINDOW_VSYNC != 0;
    cfg.master = rmp::audio::volume(rmp::audio::Bus::MASTER);
    cfg.music = rmp::audio::volume(rmp::audio::Bus::MUSIC);
    load(cfg);
    apply(cfg);
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
#if !defined(PLATFORM_WEB) && !defined(__EMSCRIPTEN__)
        // Not on the web, where there is no such switch at all. That is not
        // the same as the Subtitles box below: a control that is unavailable
        // RIGHT NOW is disabled, one that cannot exist here is not shown.
        if (rmp::ui::checkbox("VSync", &cfg.vsync)) dirty = true;
#endif

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
                store(cfg);
                saved = cfg;
                dirty = false;
            }
        });

        if (dirty) {
            rmp::ui::text("unsaved changes",
                          { .color = rmp::ui::ColorRole::DANGER, .size = 14 });
        }
        if (damaged) {
            rmp::ui::text("the saved settings were damaged; these are the defaults",
                          { .color = rmp::ui::ColorRole::MUTED, .size = 14 });
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
//   Enter / Space / gamepad bottom button   activate; start typing in a field
//   Enter / Escape / gamepad B              stop typing (the focus stays)
//   Left / Right / d-pad / stick            move a slider
//   Up / Down in an open dropdown           walk its items; Enter picks one,
//                                           Escape closes it unchanged
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
// has the keyboard: from a click on it or the focus arriving on it, until
// Enter, Escape, a click elsewhere or the focus moving on.
