#include "pause.h"

#include <rmp/ui.h>

void PauseScene::_draw() {
    // blur: how far, in design units -- CSS's blur(), the Gaussian's standard
    // deviation. dim: how far towards the veil's colour, black unless you say
    // otherwise. Six and 0.4 say "not now" and still show where you were.
    rmp::ui::overlay({ .blur = 6, .dim = 0.4f }, [] {
        rmp::ui::text("Paused", { .size = rmp::ui::Size::LARGE });
        if (rmp::ui::button("Resume")) rmp::Scene::pop();
    });
}
