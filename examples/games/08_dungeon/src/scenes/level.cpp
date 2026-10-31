// A level: the map, the hero, what LDtk placed, and the HUD over it all.

#include "game.h"
#include "objects.h"

#include <rmp/assets.h>
#include <rmp/audio.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/tilemap.h>
#include <rmp/ui.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace game {

namespace {

constexpr float BANNER_SECONDS = 2.6f;
constexpr float SHADE = 150; // how dark the edges of the screen go, of 255
constexpr float SHADE_SHARE = 0.22f; // how far in from each edge the shade reaches
constexpr int COLUMNS = 12;

const Color HEALTH = { 214, 64, 64, 255 };

rmp::ui::Picture icon(int tile) {
    return { rmp::assets::load_texture("tiles.png"),
             Rectangle{ static_cast<float>(tile % COLUMNS) * TILE,
                        std::floor(static_cast<float>(tile) / COLUMNS) * TILE, TILE,
                        TILE } };
}

// The torchlight's edge: the screen darkens towards its borders.
void shade_edges() {
    const auto w = static_cast<float>(GetScreenWidth());
    const auto h = static_cast<float>(GetScreenHeight());
    const Color dark{ 10, 4, 6, static_cast<unsigned char>(SHADE) };
    const Color clear{ 10, 4, 6, 0 };
    const float dx = w * SHADE_SHARE;
    const float dy = h * SHADE_SHARE;
    DrawRectangleGradientV(0, 0, static_cast<int>(w), static_cast<int>(dy), dark, clear);
    DrawRectangleGradientV(0, static_cast<int>(h - dy), static_cast<int>(w),
                           static_cast<int>(dy), clear, dark);
    DrawRectangleGradientH(0, 0, static_cast<int>(dx), static_cast<int>(h), dark, clear);
    DrawRectangleGradientH(static_cast<int>(w - dx), 0, static_cast<int>(dx),
                           static_cast<int>(h), clear, dark);
}

} // namespace

LevelScene::LevelScene(std::string level) : _level(std::move(level)) {}

void LevelScene::_ready() {
    background = Color{ 37, 19, 26, 255 };
    rmp::input::action("pause", KEY_ESCAPE, KEY_P, GAMEPAD_BUTTON_MIDDLE_RIGHT);
    rmp::input::action("attack", KEY_SPACE, KEY_J, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    build();
}

void LevelScene::build() {
    map = rmp::assets::load_map("world.ldtk", _level);
    add_darkness(*this); // first: every Light from here on is drawn over it
    auto &hero = spawn<Hero>();
    _hero = hero.handle();
    map.on_object("Player", [this](rmp::Scene &, const rmp::MapObject &o) {
        if (const auto h = _hero.get()) h->position = o.position;
    });
    add_monsters(*this, _hero);
    add_things(*this, _hero);
    map.spawn_objects(*this);

    camera.follow = _hero;
    camera.limits = map.bounds();
    camera.smoothing = 8;
    camera.zoom = ZOOM;
    announce(_level == "Crypt" ? "The Crypt" : "The Sanctum");
}

void LevelScene::_update(float delta) {
    if (_over) return;
    if (rmp::input::just_pressed("pause")) {
        rmp::Scene::push<PauseScene>();
        return;
    }
    run().seconds += delta;
    _banner_left = std::max(0.0f, _banner_left - delta);
}

void LevelScene::announce(std::string_view text) {
    _banner = std::string(text);
    _banner_left = BANNER_SECONDS;
}

void LevelScene::hero_died() {
    if (_over) return;
    _over = true;
    camera.shake(6, 0.5f);
    if (const auto h = _hero.get()) h->sprite.play("die", false);
    rmp::Scene::push<EndScene>(false, keep_if_best(run().gems));
}

void LevelScene::go_down(const std::string &level) {
    if (_over) return;
    _over = true;
    rmp::audio::play("door");
    rmp::Scene::change<LevelScene>(level);
}

void LevelScene::win() {
    if (_over) return;
    _over = true;
    rmp::Scene::push<EndScene>(true, keep_if_best(run().gems));
}

void LevelScene::_draw() {
    shade_edges();

    // The HUD: health and gems in a frame at the top left.
    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT, .padding = 12 });
    rmp::ui::panel([] {
        rmp::ui::row([] {
            rmp::ui::image(icon(tiles::POTION), { .width = 24, .height = 24 });
            rmp::ui::progress(static_cast<float>(run().hp) /
                                  static_cast<float>(run().max_hp),
                              { .width = 120, .fill = HEALTH });
            rmp::ui::text(TextFormat("%d/%d", run().hp, run().max_hp), { .wrap = false });
        });
        rmp::ui::row([] {
            rmp::ui::image(icon(tiles::GEM), { .width = 24, .height = 24 });
            rmp::ui::text(TextFormat("%d", run().gems), { .wrap = false });
        });
    });
    rmp::ui::end();

    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_RIGHT, .padding = 12 });
    if (rmp::ui::button("Pause")) rmp::Scene::push<PauseScene>();
    rmp::ui::end();

    if (_banner_left > 0) {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_CENTER, .padding = 70 });
        rmp::ui::panel([this] {
            rmp::ui::text(_banner, { .size = rmp::ui::Size::LARGE, .wrap = false });
        });
        rmp::ui::end();
    }
}

} // namespace game
