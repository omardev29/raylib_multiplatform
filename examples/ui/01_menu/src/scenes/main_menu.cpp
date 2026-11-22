// The whole point: a main menu.
#include "screens.h"

#include <rmp/ui.h>

void MainMenuScene::_draw() {
    // A centred column, sized and spaced by the theme, scaled to the window.
    // Three ifs and you have a menu.
    rmp::ui::begin();

    rmp::ui::text("MY GAME");

    if (rmp::ui::button("Play")) change<PlayingScene>();
    if (rmp::ui::button("Options")) change<OptionsScene>();
    if (rmp::ui::button("Quit")) change<ConfirmScene>();

    rmp::ui::end();
}
