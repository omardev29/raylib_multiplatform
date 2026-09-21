#include "main_menu.h"

#include "game.h"
#include "progress.h"

#include <rmp/app.h>
#include <rmp/ui.h>

#include <string>

void MainMenuScene::_draw() {
    rmp::ui::begin();
    rmp::ui::text("Scene stack example");

    // change<T>() from inside _draw(), which is the case the deferral exists
    // for: this scene is destroyed by that call, and it is the scene the call
    // is being made from.
    if (rmp::ui::button("Play")) rmp::Scene::change<GameScene>(1);

    const Progress &p = rmp::global<Progress>();
    if (p.runs > 0) {
        rmp::ui::text("Best " + std::to_string(p.best_score),
                      { .color = rmp::ui::ColorRole::MUTED });
    }

#if !defined(PLATFORM_IOS)
    // Inert on iPhone: Apple rejects apps that terminate themselves.
    if (rmp::ui::button("Quit")) rmp::app::quit();
#endif
    rmp::ui::end();
}
