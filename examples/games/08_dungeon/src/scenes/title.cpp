// The title: the Crypt behind the menu, the camera drifting through it.

#include "game.h"

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/ui.h>

#include <cmath>

namespace game {

namespace {
constexpr float DRIFT = 0.12f; // how fast the camera wanders, in turns of a sine
const Color TITLE = { 255, 210, 120, 255 };
const Color SHADOW = { 20, 8, 6, 255 };
} // namespace

void TitleScene::_ready() {
    rmp::app::set_stretch(rmp::app::Stretch::EXPAND);
    set_look();
    background = Color{ 37, 19, 26, 255 };
    map = rmp::assets::load_map("world.ldtk", "Crypt");
    camera.limits = map.bounds();
    camera.zoom = ZOOM;
    _best = best_gems();
}

void TitleScene::_update(float delta) {
    _clock += delta;
    const Rectangle b = map.bounds();
    camera.position = { b.x + (b.width / 2) + (std::sin(_clock * DRIFT) * b.width / 3),
                        b.y + (b.height / 2) + (std::cos(_clock * DRIFT * 0.7f) * b.height / 4) };
}

void TitleScene::_draw() {
    rmp::ui::begin();
    rmp::ui::panel([this] {
        rmp::ui::Label title{ .text = "EMBERFALL",
                              .size = 44,
                              .wrap = false,
                              .style = { .text = TITLE,
                                         .text_outline = rmp::ui::TextOutline{ .width = 2,
                                                                               .color = SHADOW } } };
        title.show();
        rmp::ui::text("A crypt, a cyclops, and you", { .color = rmp::ui::ColorRole::MUTED,
                                                        .wrap = false });
        if (_best >= 0) {
            rmp::ui::text(TextFormat("Most gems carried out: %d", _best),
                          { .color = rmp::ui::ColorRole::MUTED, .wrap = false });
        }
        if (rmp::ui::button("Descend")) {
            new_run();
            rmp::Scene::change<LevelScene>("Crypt");
        }
#if !defined(PLATFORM_IOS)
        if (rmp::ui::button("Quit")) rmp::app::quit();
#endif
    });
    rmp::ui::end();
}

} // namespace game
