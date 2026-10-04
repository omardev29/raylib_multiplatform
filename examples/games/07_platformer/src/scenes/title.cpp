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
    _best = best();
}

void TitleScene::_update(float delta) {
    if (GetScreenHeight() > 0)
        camera.zoom = static_cast<float>(GetScreenHeight()) / VIEW_HEIGHT;
    _clock += delta;
    const Rectangle b = map.bounds();
    // Slowly from one end of the level to the other and back; `limits` keeps
    // the view inside it, so the drift needs no arithmetic about the window.
    camera.position = { b.x + (b.width / 2) + (std::sin(_clock * 0.15f) * b.width / 2),
                        b.y + (b.height / 2) };
}

void TitleScene::_draw() {
    rmp::ui::begin();
    // A dark panel, because the level behind is bright and the theme's text is
    // light: the menu itself is the three lines inside it.
    rmp::ui::panel({ .background = Color{ 20, 24, 40, 210 } }, [this] {
        rmp::ui::text("Pixel Platformer",
                      { .size = rmp::ui::Size::LARGE, .wrap = false });
        if (_best.coins >= 0) {
            rmp::ui::text(
                TextFormat("Best: %d coins in %.1f s", _best.coins, _best.seconds),
                { .color = rmp::ui::ColorRole::MUTED, .wrap = false });
        }
        if (rmp::ui::button("Play")) {
            rmp::audio::play("select");
            new_run();
            rmp::Scene::change<LevelScene>("");
        }
        // quit() does nothing on iOS, where Apple rejects an app that ends
        // itself -- so the button is not there at all rather than dead.
#if !defined(PLATFORM_IOS)
        if (rmp::ui::button("Quit")) rmp::app::quit();
#endif
    });
    rmp::ui::end();
}

} // namespace game
