// The pause: the town waits below.

#include "school.h"

#include <rmp/input.h>
#include <rmp/ui.h>

namespace game {

void PauseScene::_update(float /*delta*/) {
    if (rmp::input::just_pressed("pause")) rmp::Scene::pop();
}

void PauseScene::_draw() {
    rmp::ui::overlay([] {
        rmp::ui::panel([] {
            rmp::ui::text("Paused", { .size = rmp::ui::Size::LARGE, .wrap = false });
            if (rmp::ui::button("Back to town")) rmp::Scene::pop();
            if (rmp::ui::button("To the title")) {
                save_story();
                rmp::Scene::change<TitleScene>();
            }
        });
    });
}

} // namespace game
