#include "pause.h"

#include "main_menu.h"

#include <rmp/ui.h>

void PauseScene::_draw() {
    // overlay() is a frame of its own, in front of everything drawn before it
    // -- the game underneath, its world AND its HUD -- dimmed behind a veil.
    // The veil takes every click, so the HUD's Pause under it presses
    // nothing, and the first button has the focus, so Enter is Resume.
    rmp::ui::overlay([] {
        rmp::ui::text("Paused");
        if (rmp::ui::button("Resume")) rmp::Scene::pop();
        if (rmp::ui::button("Give up")) rmp::Scene::change<MainMenuScene>();
    });
}
