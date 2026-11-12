// How the evening ended: the town at dusk, and -- at the festival, at the
// concert -- a sky of lanterns rising over it.

#include "school.h"

#include <rmp/assets.h>
#include <rmp/random.h>
#include <rmp/ui.h>

#include <array>
#include <cmath>
#include <utility>

namespace game {

namespace {

constexpr int LANTERNS = 40;
constexpr float RISE = 16; // world units a second
constexpr float SWAY = 0.7f; // turns of a sine a second
const Color DUSK = { 24, 20, 60, 150 };
const Color FLAME = { 255, 170, 70, 255 };
const Color GLOW = { 255, 120, 40, 60 };

// Where each lantern starts and how it drifts: seeded, so every run (and
// every e2e shot) draws the same sky.
struct Lantern {
    Vector2 from;
    float phase;
    float size;
};
std::array<Lantern, LANTERNS> &lanterns() {
    static std::array<Lantern, LANTERNS> all{};
    return all;
}

} // namespace

EndScene::EndScene(std::string kind) : _kind(std::move(kind)) {}

void EndScene::_ready() {
    load_town(*this);
    map.spawn_objects(*this);
    const Rectangle b = map.bounds();
    camera.zoom = ZOOM;
    camera.position = { b.x + b.width / 2, b.y + b.height / 3 };
    for (Lantern &l : lanterns()) {
        l = { { rmp::random::range(b.x, b.x + b.width),
                rmp::random::range(b.y, b.y + b.height * 1.2f) },
              rmp::random::range(0.0f, 6.28f),
              rmp::random::range(2.0f, 4.0f) };
    }
}

void EndScene::_update(float delta) { _clock += delta; }

void EndScene::_draw() {
    // Dusk over the town; the lanterns are drawn in the world's coordinates.
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), DUSK);
    if (_kind != "home") {
        BeginMode2D(camera.raylib());
        for (const Lantern &l : lanterns()) {
            const Vector2 at{ l.from.x + std::sin(_clock * SWAY + l.phase) * 6,
                              l.from.y - _clock * RISE };
            DrawCircleV(at, l.size * 2.5f, GLOW);
            DrawCircleV(at, l.size, FLAME);
        }
        EndMode2D();
    }
    rmp::ui::begin();
    rmp::ui::panel([this] {
        if (_kind == "festival") {
            rmp::ui::text("The Lantern Festival",
                          { .size = rmp::ui::Size::LARGE, .wrap = false });
            rmp::ui::text("You and Mika watch a thousand lanterns rise.",
                          { .wrap = false });
        } else if (_kind == "concert") {
            rmp::ui::text("Front Row", { .size = rmp::ui::Size::LARGE, .wrap = false });
            rmp::ui::text("Sora plays the best set of his life.", { .wrap = false });
        } else {
            rmp::ui::text("A Quiet Evening",
                          { .size = rmp::ui::Size::LARGE, .wrap = false });
            rmp::ui::text("The lanterns rise far away. Maybe next year.",
                          { .wrap = false });
        }
        if (rmp::ui::button("Back to the title")) rmp::Scene::change<TitleScene>();
    });
    rmp::ui::end();
}

} // namespace game
