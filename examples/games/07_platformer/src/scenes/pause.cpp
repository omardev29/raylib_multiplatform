// The pause, pushed on top of the level by the HUD's Pause button, Escape, P
// or a gamepad's Start. The level underneath freezes -- its clock too -- and
// stays on screen, and stops hearing the keyboard: the scene stack's defaults.
// What is here is the menu, and the look of it: the level blurred behind it,
// so the eye goes to Resume.

#include "game.h"

#include <rmp/audio.h>
#include <rmp/input.h>
#include <rmp/ui.h>

namespace game {

void PauseScene::_update(float /*delta*/) {
    // The key that paused takes the pause away again.
    if (rmp::input::just_pressed("pause")) rmp::Scene::pop();
}

void PauseScene::_draw() {
    // A real blur wherever there is a GPU, a plain veil where there is none
    // (raylib's software renderer); either way Resume has the focus, so Enter
    // and the A button resume too.
    rmp::ui::overlay({ .blur = 6, .dim = 0.4f }, [] {
        rmp::ui::text("Paused", { .size = rmp::ui::Size::LARGE });
        if (rmp::ui::button("Resume")) {
            rmp::audio::play("select");
            rmp::Scene::pop();
        }
        if (rmp::ui::button("Menu")) {
            rmp::audio::play("select");
            rmp::Scene::change<TitleScene>();
        }
    });
}

} // namespace game
