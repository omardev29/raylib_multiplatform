#include "game.h"

#include "pause.h"
#include "progress.h"

#include <rmp/app.h>
#include <rmp/input.h>
#include <rmp/ui.h>

#include <string>

namespace {
constexpr float PLAYER_SPEED = 220.0f; // units per second
} // namespace

void GameScene::_ready() {
    ++rmp::global<Progress>().runs;

    // The player: a dot, kept inside the window by Edge::CLAMP.
    rmp::Shape dot = rmp::circle(14);
    dot.color = rmp::ui::current_theme().primary;
    _player = spawn({ .position = { 400, 260 }, .shape = dot, .edges = rmp::Edge::CLAMP })
                  .handle();
}

void GameScene::_update(float delta) {
    // No pause check. When PauseScene is on top, this is not called at all.
    _elapsed += delta;
    _score = static_cast<int>(_elapsed * 10.0f);

    // The arrows, WASD, the d-pad and the left stick, in one call. Reading
    // them is what makes this a scene being PLAYED -- see the HUD below.
    const Vector2 move = rmp::input::vector();
    if (_player) _player->velocity = { move.x * PLAYER_SPEED, move.y * PLAYER_SPEED };
}

void GameScene::_draw() {
    // A HUD over a game being played. This scene reads the arrows above, so
    // the framework does not hand its Pause button the focus: the arrows,
    // Enter and Space stay the game's, a click or a tap presses it, and Tab
    // walks into it from the keyboard (Escape walks back out). A menu that is
    // the whole screen -- the main menu, the pause menu -- reads no keys of
    // its own, and its first button has the focus from the start.
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
