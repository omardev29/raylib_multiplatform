// The credits, which is where a font's author goes.
#include "scenes.h"

#include <rmp/input.h>
#include <rmp/ui.h>

void CreditsScene::_update(float) {
    if (rmp::input::just_pressed("ui_cancel")) change<TitleScene>();
}

void CreditsScene::_draw() {
    rmp::ui::begin();
    rmp::ui::panel({ .box = { .width = 640 } }, [] {
        rmp::ui::text("CREDITS", { .size = rmp::ui::Size::LARGE });
        rmp::ui::text("Font: Hack Bold, by Source Foundry");
        rmp::ui::text(
            "MIT licence; derived from DejaVu and Bitstream Vera",
            { .color = rmp::ui::ColorRole::MUTED, .size = rmp::ui::Size::SMALL });
        rmp::ui::text(
            "Made with raylib and rmp",
            { .color = rmp::ui::ColorRole::MUTED, .size = rmp::ui::Size::SMALL });
        if (rmp::ui::button("Back")) change<TitleScene>();
    });
    rmp::ui::end();
}
