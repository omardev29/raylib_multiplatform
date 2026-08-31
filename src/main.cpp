// Your game starts here, and this file is meant to stay about this size.
//
// It is a start menu and nothing else on purpose: it is the first thing anyone
// reads, so every mechanic added to it is a mechanic between a newcomer and the
// point. Whatever the template grows next gets an example under examples/, not
// a paragraph here.

#include <rmp/app.h>

// The CI render gate. rmp/app.h no longer drags this in — it names nothing of
// ours and nothing of the test harness — so a file that calls it includes it.
// This goes away in phase 4, when RMP_GAME owns the frame and captures for you.
#include <smoke_test.h>
#include <rmp/assets.h>
#include <rmp/ui.h>

// Nothing to unload. rmp::Texture releases itself when it goes out of scope,
// and rmp::assets::shutdown() — which the entry point runs before the window
// closes — releases whatever is still held. That is the whole of the change:
// this file used to carry an Image, a Texture2D, and two Unload calls in
// on_exit() that had to stay paired with them.
static rmp::Texture g_rabbit;

// Called once at startup: set config flags, create the window, load assets.
//
// The resource pack is already open by the time you get here: the entry point
// in <rmp/app.h> calls rmp::assets::init() before this, and
// rmp::assets::shutdown() after on_exit(). Neither is yours to remember.
static inline void on_ready() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    // Title and size come from raylib_multiplatform.toml — see [window].
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);

    // Served from resources.rres when a release packed one, from the loose file
    // in resources/ otherwise. Same call either way.
    // Asking twice for this name would give the same texture back, not a
    // second copy: the name is the cache key.
    g_rabbit = rmp::assets::load_texture("rabbit.png");
}

// Called each frame
static inline void on_frame(float /*delta*/) {
    BeginDrawing();
    ClearBackground(rmp::ui::current_theme().background);

    // A menu. Nothing here mentions a coordinate, a size, a font or a hitbox,
    // and it stays centred and correctly proportioned at any window Size —
    // resize the window and watch. The containers take their contents as a
    // lambda, so there is no closing call to forget.
    rmp::ui::begin();

    rmp::ui::panel([&] {
        rmp::ui::row({ .gap = 16 }, [&] {
            rmp::ui::image(g_rabbit, { .width = 64, .height = 64 });
            rmp::ui::column({ .items = rmp::ui::Align::CENTER_LEFT }, [&] {
                rmp::ui::text(APP_WINDOW_TITLE);
                rmp::ui::text("raylib + rmp::ui",
                              { .color = rmp::ui::ColorRole::MUTED, .size = 14 });
            });
        });

        if (rmp::ui::button("Play")) TraceLog(LOG_INFO, "MENU: play");
        if (rmp::ui::button("Options")) TraceLog(LOG_INFO, "MENU: options");

        // rmp::app::quit() rather than std::exit(): it lets this frame finish,
        // then runs on_exit() and closes the window properly. It does nothing on
        // iOS, where Apple rejects apps that terminate themselves — so the button
        // is hidden there rather than left dead.
#if !defined(PLATFORM_IOS)
        if (rmp::ui::button("Quit")) rmp::app::quit();
#endif
    });

    rmp::ui::end();

    // CI smoke-test hook: read the frame back and check something was actually
    // drawn. Must sit here, between the last draw call and EndDrawing() — see
    // tests/smoke_test.h for why either side of that line is wrong. No-op unless
    // RAY_TEST_MAX_FRAMES is set.
    SmokeTest_CaptureFrame();

    EndDrawing();
}

// Called once at shutdown: close the window, and that is all. There is nothing
// to unload — see the note at the top of this file.
static inline void on_exit() { CloseWindow(); }

// Main function or ios functions + smoke tests
RMP_ENTRY_POINT(on_ready, on_frame, on_exit);
