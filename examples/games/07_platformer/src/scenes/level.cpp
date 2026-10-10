// ---------------------------------------------------------------------------
// One level of the world: load it, build what LDtk placed in it, and watch the
// player leave it -- into the next level, or off the bottom.
// ---------------------------------------------------------------------------

#include "game.h"
#include "objects.h"

#include <rmp/assets.h>
#include <rmp/audio.h>
#include <rmp/tilemap.h>

#include <algorithm>
#include <utility>

namespace game {

namespace {
// A sign's picture: plain, or with the arrow LDtk's `arrow` Enum asks for.
int sign_tile(std::string_view arrow) {
    if (arrow == "Left") return tiles::SIGN_LEFT;
    if (arrow == "Right") return tiles::SIGN_RIGHT;
    return tiles::SIGN;
}
} // namespace

LevelScene::LevelScene(std::string level, Entry entry)
    : _level(std::move(level)), _entry(entry) {}

void LevelScene::_ready() {
    // "" is the first level of the world, whatever LDtk calls it: rename or
    // reorder the levels there and the game still starts at the beginning.
    map = rmp::assets::load_map("world.ldtk", _level);
    rmp::audio::music("music"); // already playing after a level change: not restarted

    // The player first, so that what is built below can know who it is.
    auto &player =
        spawn<Player>({ .position = _entry.position, .velocity = _entry.velocity });
    _player = player.handle();

    // ---- what LDtk placed, one factory per entity -------------------------
    //
    // Anything already collected, stomped or opened in this run is in
    // run().gone by its iid, and is simply not built again.

    map.on_object("Player", [this](rmp::Scene &, const rmp::MapObject &o) {
        if (!_entry.carried) _player->position = o.position;
    });
    map.on_object("Coin", [](rmp::Scene &s, const rmp::MapObject &o) {
        if (contains(run().gone, o.iid)) return;
        auto &coin = s.spawn<Coin>({ .position = o.position, .layer = 1 });
        coin.iid = o.iid;
        coin.value = o.property_int("value", 1);
    });
    map.on_object("Walker", [](rmp::Scene &s, const rmp::MapObject &o) {
        if (contains(run().gone, o.iid)) return;
        auto &walker = s.spawn<Walker>({ .position = o.position, .layer = 2 });
        walker.iid = o.iid;
        walker.speed = o.property_float("speed", 40);
        walker.stompable = o.property_bool("stompable", true);
        // The route is drawn in LDtk as Points; a walker keeps to its floor
        // and only needs to know where to turn.
        walker.left = walker.right = o.position.x;
        for (int i = 0; i < o.property_count("patrol"); i++) {
            const float x = o.property_point("patrol", i).x;
            walker.left = std::min(walker.left, x);
            walker.right = std::max(walker.right, x);
        }
    });
    map.on_object("Bat", [](rmp::Scene &s, const rmp::MapObject &o) {
        if (contains(run().gone, o.iid)) return;
        auto &bat = s.spawn<Bat>({ .position = o.position, .layer = 2 });
        bat.iid = o.iid;
        bat.speed = o.property_float("speed", 50);
        for (int i = 0; i < o.property_count("patrol"); i++) {
            bat.route.push_back(o.property_point("patrol", i));
        }
        if (bat.route.empty()) bat.route.push_back(o.position);
    });
    map.on_object("MovingPlatform", [this](rmp::Scene &s, const rmp::MapObject &o) {
        auto &platform = s.spawn<Platform>({ .position = o.position });
        platform.collider = rmp::rect(o.size);
        platform.from = o.position;
        platform.to = o.property_point("to", o.position);
        platform.speed = o.property_float("speed", 40);
        platform.rider = _player;
    });
    map.on_object("Key", [](rmp::Scene &s, const rmp::MapObject &o) {
        if (contains(run().gone, o.iid)) return;
        auto &key = s.spawn<Key>({ .position = o.position, .layer = 1 });
        key.iid = o.iid;
        key.opens = o.property_string("opens");
    });
    map.on_object("Door", [](rmp::Scene &s, const rmp::MapObject &o) {
        if (contains(run().gone, o.iid)) return;
        auto &door = s.spawn<Door>({ .position = o.position });
        door.collider = rmp::rect(o.size);
        door.iid = o.iid;
    });
    map.on_object("Spikes", [](rmp::Scene &s, const rmp::MapObject &o) {
        // Only the points hurt: the bottom eight pixels, a little in from the ends.
        auto &spikes = s.spawn<Spikes>({ .position = o.position });
        spikes.collider = rmp::rect({ o.size.x - 4, 8 });
        spikes.collider.offset = { 0, (o.size.y / 2) - 4 };
    });
    map.on_object("Sign", [](rmp::Scene &s, const rmp::MapObject &o) {
        auto &sign = s.spawn<Sign>({ .position = o.position, .layer = -1 });
        sign.text = o.property_string("text");
        sign.tile = sign_tile(o.property_string("arrow", "None"));
    });
    map.on_object("Goal", [](rmp::Scene &s, const rmp::MapObject &o) {
        s.spawn<Goal>({ .position = o.position });
    });
    map.spawn_objects(*this);
    _start = _player->position;

    // ---- the camera ----------------------------------------------------------
    camera.follow = _player;
    camera.limits = map.bounds(); // the level's own rectangle, in the world
    camera.smoothing = 10;
}

void LevelScene::_update(float delta) {
    if (GetScreenHeight() > 0)
        camera.zoom = static_cast<float>(GetScreenHeight()) / VIEW_HEIGHT;
    if (_over) return;
    run().seconds += delta;

    const auto player = _player.get();
    if (!player) return;

    // THE NEXT LEVEL. Out of this one and into another of the world: change
    // to it, and arrive at the same world position going the same way.
    const std::string_view next = map.neighbour_at(player->position);
    if (!next.empty()) {
        _over = true;
        rmp::Scene::change<LevelScene>(std::string(next),
                                       Entry{ true, player->position, player->velocity });
        return;
    }

    // No level there. The sides of the world are walls; below is a fall.
    const Rectangle b = map.bounds();
    if (player->position.x < b.x || player->position.x > b.x + b.width) {
        player->position.x = std::clamp(player->position.x, b.x, b.x + b.width);
        player->velocity.x = 0;
    }
    if (player->position.y > b.y + b.height + 48) {
        lose_life();
        if (!_over) respawn();
    }
}

void LevelScene::lose_life() {
    if (_over) return;
    Run &r = run();
    r.lives--;
    camera.shake(5, 0.35f);
    rmp::audio::play("hurt");
    if (r.lives > 0) return;
    _over = true;
    rmp::audio::music("lose", false);
    rmp::Scene::push<EndScene>(false, false);
}

void LevelScene::respawn() {
    if (const auto player = _player.get()) {
        player->position = _start;
        player->velocity = Vector2{};
    }
}

void LevelScene::win() {
    if (_over) return;
    _over = true;
    const bool record = keep_if_best(run().coins, run().seconds);
    rmp::audio::music("win", false);
    rmp::Scene::push<EndScene>(true, record);
}

// The HUD, in screen space and as big as the window: hearts, coins, the clock.
void LevelScene::_draw() {
    const float scale =
        std::max(1.0f, static_cast<float>(GetScreenHeight()) / VIEW_HEIGHT);
    for (int i = 0; i < 3; i++) {
        const int heart = i < run().lives ? tiles::HEART : tiles::HEART_EMPTY;
        draw_icon(heart, { (6 + (static_cast<float>(i) * 17)) * scale, 4 * scale },
                  scale);
    }
    draw_icon(tiles::COIN, { 6 * scale, 22 * scale }, scale);
    draw_icon(tiles::TIMES, { 20 * scale, 22 * scale }, scale);
    draw_number(run().coins, { 32 * scale, 22 * scale }, scale);
    const float right = static_cast<float>(GetScreenWidth());
    draw_seconds(run().seconds, { right - (60 * scale), 4 * scale }, scale);
}

} // namespace game
