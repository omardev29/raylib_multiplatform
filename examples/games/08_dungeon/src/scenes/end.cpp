// The end of a run, won or lost, over the level it ended in.

#include "dungeon.h"

#include <rmp/ui.h>

namespace game {

EndScene::EndScene(bool won, bool record) : _won(won), _record(record) {}

void EndScene::_draw() {
    rmp::ui::overlay({ .blur = 4, .dim = 0.55f }, [this] {
        rmp::ui::panel([this] {
            rmp::ui::text(_won ? "You walk out into the dawn" : "The crypt keeps you",
                          { .size = rmp::ui::Size::LARGE, .wrap = false });
            rmp::ui::text(TextFormat("%d %s in %.0f seconds", run().gems,
                                     run().gems == 1 ? "gem" : "gems", run().seconds),
                          { .color = rmp::ui::ColorRole::MUTED, .wrap = false });
            if (_record) rmp::ui::text("More than ever before!", { .wrap = false });
            if (rmp::ui::button("Again")) {
                new_run();
                rmp::Scene::change<LevelScene>("Crypt");
            }
            if (rmp::ui::button("Title")) rmp::Scene::change<TitleScene>();
        });
    });
}

} // namespace game
