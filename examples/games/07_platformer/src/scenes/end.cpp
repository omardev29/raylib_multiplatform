// The end of a run, pushed on top of the level. The level underneath freezes
// and stays on screen, and stops hearing the keyboard -- the scene stack's
// defaults -- so this is only what it says and the two ways out.

#include "game.h"

#include <rmp/audio.h>
#include <rmp/ui.h>

namespace game {

EndScene::EndScene(bool won, bool record) : won_(won), record_(record) {}

void EndScene::_draw() {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{ 10, 12, 24, 170 });
    const Run &r = run();
    rmp::ui::begin();
    rmp::ui::text(won_ ? "You made it!" : "Game over", { .size = rmp::ui::Size::LARGE });
    rmp::ui::text(TextFormat("%d coins in %.1f s", r.coins, r.seconds));
    if (won_ && record_)
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
    rmp::ui::end();
}

} // namespace game
