// The options: every kind of control, and every one of them in the font.
#include "scenes.h"

#include <rmp/input.h>
#include <rmp/ui.h>

#include <array>
#include <string_view>

namespace {
constexpr std::array<std::string_view, 3> DIFFICULTY{ "Relaxed", "Normal", "Wide awake" };
} // namespace

void OptionsScene::_update(float) {
    // Escape, or the gamepad's B: back to the title.
    if (rmp::input::just_pressed("ui_cancel")) change<TitleScene>();
}

void OptionsScene::_draw() {
    rmp::ui::begin();
    rmp::ui::panel({ .box = { .width = 420 } }, [] {
        rmp::ui::text("OPTIONS", { .size = rmp::ui::Size::LARGE });
        rmp::ui::checkbox("Show hints", options.hints);
        rmp::ui::slider("Text speed", options.text_speed, 0.0f, 1.0f, { .step = 0.1f });
        rmp::ui::dropdown("Difficulty", options.difficulty, DIFFICULTY);
        if (rmp::ui::button("Back")) change<TitleScene>();
    });
    rmp::ui::end();
}
