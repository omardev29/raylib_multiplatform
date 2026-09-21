#include "pause.h"

#include "main_menu.h"

#include <raylib.h>
#include <rmp/ui.h>

void PauseScene::_draw() {
    // The veil. _draw() runs after everything the game scene drew -- its world
    // AND its HUD, because each scene's UI is drawn in its own pass -- so a
    // plain rectangle here lands over all of it and under this menu.
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 0, 0, 0, 180 });

    rmp::ui::begin();
    rmp::ui::text("Paused");
    if (rmp::ui::button("Resume")) rmp::Scene::pop();
    if (rmp::ui::button("Give up")) rmp::Scene::change<MainMenuScene>();
    rmp::ui::end();
}
