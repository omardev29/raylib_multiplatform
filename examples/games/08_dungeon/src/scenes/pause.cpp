// The pause: the level frozen and blurred behind it.

#include "game.h"

#include <rmp/input.h>
#include <rmp/ui.h>

namespace game {

void PauseScene::_update(float /*delta*/) {
    if (rmp::input::just_pressed("pause")) rmp::Scene::pop();
}

void PauseScene::_draw() {
    rmp::ui::overlay({ .blur = 6, .dim = 0.5f }, [] {
        rmp::ui::panel([] {
            rmp::ui::text("Paused", { .size = rmp::ui::Size::LARGE, .wrap = false });
            if (rmp::ui::button("Resume")) rmp::Scene::pop();
            if (rmp::ui::button("Leave the dungeon")) rmp::Scene::change<TitleScene>();
        });
    });
}

} // namespace game
