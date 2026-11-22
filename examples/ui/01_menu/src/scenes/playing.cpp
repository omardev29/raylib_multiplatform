// A HUD: not everything is a centred menu.
#include "screens.h"

#include <rmp/input.h>
#include <rmp/ui.h>

#include <string>

void PlayingScene::_update(float) {
    game.score += 1;
    // "ui_cancel" is one of the actions that come defined: Escape, or the
    // gamepad's right face button. Through rmp::input rather than
    // IsKeyPressed(KEY_ESCAPE), so a controller can go back too.
    if (rmp::input::just_pressed("ui_cancel")) change<MainMenuScene>();
}

void PlayingScene::_draw() {
    // placement moves the whole thing. The nine Align values are the ones you
    // would guess: TOP_LEFT, TOP_CENTER, ..., BOTTOM_RIGHT.
    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT, .gap = 4 });

    // Built fresh every frame, which is exactly how immediate mode is meant to
    // be used: no label object to update, no "setText" to remember.
    rmp::ui::text("Score: " + std::to_string(game.score));
    rmp::ui::text("Press ESC for the menu",
                  { .color = rmp::ui::ColorRole::MUTED, .size = 14 });

    rmp::ui::end();
}
