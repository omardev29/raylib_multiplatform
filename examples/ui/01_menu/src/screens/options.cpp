// One step up: options on the widgets.
#include "screens.h"

#include <rmp/ui.h>

#include <string>

void options_menu() {
    rmp::ui::begin();

    rmp::ui::text("Options");

    // A button whose label is built this frame. std::string_view takes it, and
    // the text is copied immediately -- temporaries are safe here.
    if (rmp::ui::button(std::string("Music: ") + (g_state.music ? "on" : "off"))) {
        g_state.music = !g_state.music;
    }

    // Semantic variants: you say what the button MEANS, the theme decides what
    // that looks like. Restyling the game never means revisiting this line.
    rmp::ui::button("Reset progress", { .style = rmp::ui::Variant::DANGER });

    // Disabled controls still lay out, and still look deliberate.
    rmp::ui::button("Cloud saves", { .enabled = false });

    if (rmp::ui::button("Back")) g_state.screen = Screen::MENU;

    rmp::ui::end();
}
