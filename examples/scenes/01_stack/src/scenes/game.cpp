#include "game.h"

#include "pause.h"
#include "progress.h"

#include <rmp/app.h>
#include <rmp/ui.h>

#include <string>

void GameScene::_ready() { ++rmp::global<Progress>().runs; }

void GameScene::_update(float delta) {
    // No pause check. When PauseScene is on top, this is not called at all.
    _elapsed += delta;
    _score = static_cast<int>(_elapsed * 10.0f);
}

void GameScene::_draw() {
    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
    rmp::ui::text("Level " + std::to_string(_level));
    // Safe: rmp::ui interns every string it is given into a frame arena, so a
    // temporary like this one outlives the layout that reads it.
    rmp::ui::text("Score " + std::to_string(_score));
    if (rmp::ui::button("Pause")) rmp::Scene::push<PauseScene>();
    rmp::ui::end();
}

void GameScene::_end() {
    Progress &p = rmp::global<Progress>();
    if (_score > p.best_score) p.best_score = _score;
}
