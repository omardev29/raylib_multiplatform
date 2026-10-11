// The end of a run, pushed on top of the level. The level underneath freezes
// and stays on screen, and stops hearing the keyboard -- the scene stack's
// defaults -- so this is only what it says and the two ways out, in an
// overlay: in front of the level, which it dims towards a night blue.

#include "game.h"

#include <rmp/audio.h>
#include <rmp/ui.h>

namespace game {

EndScene::EndScene(bool won, bool record) : _won(won), _record(record) {}

void EndScene::_draw() {
    const Run &r = run();
    rmp::ui::overlay({ .dim = 2 / 3.0f, .veil = Color{ 10, 12, 24, 255 } }, [&] {
        rmp::ui::text(_won ? "You made it!" : "Game over",
                      { .size = rmp::ui::Size::LARGE });
        rmp::ui::text(TextFormat("%d coins in %.1f s", r.coins, r.seconds));
        if (_won && _record)
            rmp::ui::text("A new best!", { .color = rmp::ui::ColorRole::PRIMARY });
        if (rmp::ui::button("Play again")) {
            rmp::audio::play("select");
            new_run();
            rmp::Scene::change<LevelScene>("");
        }
        if (rmp::ui::button("Menu")) {
            rmp::audio::play("select");
            rmp::Scene::change<TitleScene>();
        }
    });
}

} // namespace game
