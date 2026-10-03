// The title: the first level behind a three-line menu, the camera drifting
// across it, and the best run so far.

#include "game.h"

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/audio.h>
#include <rmp/ui.h>

#include <cmath>

namespace game {

void TitleScene::_ready() {
    // The map with no factories: the level is drawn and nothing in it is
    // built. A map costs a scene nothing it does not ask for.
    map = rmp::assets::load_map("world.ldtk");
    camera.limits = map.bounds();
    rmp::audio::music("music");
    best_ = best();
}

void TitleScene::_update(float delta) {
    if (GetScreenHeight() > 0)
        camera.zoom = static_cast<float>(GetScreenHeight()) / 225.0f;
    clock_ += delta;
    const Rectangle b = map.bounds();
    // Slowly from one end of the level to the other and back; `limits` keeps
    // the view inside it, so the drift needs no arithmetic about the window.
    camera.position = { b.x + (b.width / 2) + (std::sin(clock_ * 0.15f) * b.width / 2),
                        b.y + (b.height / 2) };
}

void TitleScene::_draw() {
    rmp::ui::begin();
    // A dark panel, because the level behind is bright and the theme's text is
    // light: the menu itself is the three lines inside it.
    rmp::ui::panel({ .background = Color{ 20, 24, 40, 210 } }, [this] {
        rmp::ui::text("Pixel Platformer",
                      { .size = rmp::ui::Size::LARGE, .wrap = false });
        if (best_.coins >= 0) {
            rmp::ui::text(
                TextFormat("Best: %d coins in %.1f s", best_.coins, best_.seconds),
                { .color = rmp::ui::ColorRole::MUTED, .wrap = false });
        }
        if (rmp::ui::button("Play")) {
            rmp::audio::play("select");
            new_run();
            rmp::Scene::change<LevelScene>("");
        }
        if (rmp::ui::button("Quit")) rmp::app::quit();
    });
    rmp::ui::end();
}

} // namespace game
