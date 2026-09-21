// The whole point: a main menu.
#include "screens.h"

#include <rmp/ui.h>

void main_menu() {
    // A centred column, sized and spaced by the theme, scaled to the window.
    // Three ifs and you have a menu.
    rmp::ui::begin();

    rmp::ui::text("MY GAME");

    if (rmp::ui::button("Play")) g_state.screen = Screen::PLAYING;
    if (rmp::ui::button("Options")) g_state.screen = Screen::OPTIONS;
    if (rmp::ui::button("Quit")) g_state.screen = Screen::CONFIRM;

    rmp::ui::end();
}
