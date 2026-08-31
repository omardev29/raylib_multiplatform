#include "main_menu.h"

#include <rmp/app.h>
#include <rmp/config.h>
#include <rmp/ui.h>

// Once, when the scene enters the stack. The window is already open by then —
// RMP_GAME opened it from [window] in raylib_multiplatform.toml — and the
// resource pack is already mounted, so this is simply where assets load.
void MainMenuScene::_ready() {
    // Served from resources.rres when a release packed one, and from the loose
    // file in resources/ otherwise. Same call either way. Asking twice for this
    // name gives the same texture back, not a second copy: the name is the key.
    rabbit = rmp::assets::load_texture("rabbit.png");
}

// Every frame, in screen space. Nothing here mentions a coordinate, a size, a
// font or a hitbox, and it stays centred and correctly proportioned at any
// window size — resize the window and watch. The containers take their contents
// as a lambda, so there is no closing call to forget.
void MainMenuScene::_draw() {
    rmp::ui::begin();

    rmp::ui::panel([&] {
        rmp::ui::row({ .gap = 16 }, [&] {
            rmp::ui::image(rabbit, { .width = 64, .height = 64 });
            rmp::ui::column({ .items = rmp::ui::Align::CENTER_LEFT }, [&] {
                rmp::ui::text(APP_WINDOW_TITLE);
                rmp::ui::text("raylib + rmp::ui",
                              { .color = rmp::ui::ColorRole::MUTED, .size = 14 });
            });
        });

        if (rmp::ui::button("Play")) TraceLog(LOG_INFO, "MENU: play");
        if (rmp::ui::button("Options")) TraceLog(LOG_INFO, "MENU: options");

        // rmp::app::quit() rather than std::exit(): it lets this frame finish,
        // then unwinds the scene stack and closes the window properly. It does
        // nothing on iOS, where Apple rejects apps that terminate themselves —
        // so the button is hidden there rather than left dead.
#if !defined(PLATFORM_IOS)
        if (rmp::ui::button("Quit")) rmp::app::quit();
#endif
    });

    rmp::ui::end();
}
