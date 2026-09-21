// Two buttons with the same label, in different screens.
#include "screens.h"

#include <rmp/app.h>
#include <rmp/ui.h>

void confirm_quit() {
    rmp::ui::begin();

    rmp::ui::text("Really quit?");
    rmp::ui::text("Progress since the last save will be lost.",
                  { .color = rmp::ui::ColorRole::MUTED });

    // rmp::app::quit(), never std::exit(): this lets the frame finish and
    // then runs on_exit() and CloseWindow() on the way out. On Android it also
    // finishes the Activity, so the app does not leave a dead task behind.
    //
    // On iOS it does nothing at all, on purpose -- Apple rejects apps that
    // terminate themselves. Guarding it out is optional; the call is safe
    // everywhere. Here it is guarded so the button is not dead on iPhone.
#if !defined(PLATFORM_IOS)
    if (rmp::ui::button("Quit", { .style = rmp::ui::Variant::DANGER })) rmp::app::quit();
#endif

    // This screen and the main menu both have a "Back"-ish button. Identical
    // labels inside ONE frame are told apart automatically; an explicit id is
    // for when the UI is conditional -- as it is here -- and you want an
    // element's hover state to stay its own across screen changes.
    if (rmp::ui::button("Cancel", { .id = "confirm.cancel" }))
        g_state.screen = Screen::MENU;

    rmp::ui::end();
}
