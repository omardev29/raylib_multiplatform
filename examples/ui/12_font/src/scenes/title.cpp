// The title screen, and where the font is set.
#include "scenes.h"

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/ui.h>

void TitleScene::_ready() {
    // Every scene after this one draws in it too: the font is the UI's, not
    // the scene's. Setting it again when the title comes back costs nothing --
    // the asset is loaded once, by name.
    rmp::ui::set_font(rmp::assets::load_font("fonts/hack-bold.ttf", 20));
}

void TitleScene::_draw() {
    rmp::ui::begin();

    rmp::ui::text("NIGHT SHIFT", { .size = 48 });
    rmp::ui::text("a game about staying awake",
                  { .color = rmp::ui::ColorRole::MUTED, .size = rmp::ui::Size::SMALL });
    rmp::ui::spacer(12);

    if (rmp::ui::button("New game", { .style = rmp::ui::Variant::PRIMARY })) {
        rmp::ui::notify("No game yet: this is a menu");
    }
    if (rmp::ui::button("Options")) change<OptionsScene>();
    if (rmp::ui::button("Credits")) change<CreditsScene>();
#if !defined(PLATFORM_IOS)
    // Inert on iPhone: Apple rejects apps that terminate themselves.
    if (rmp::ui::button("Quit", { .style = rmp::ui::Variant::GHOST })) rmp::app::quit();
#endif

    rmp::ui::end();
}
