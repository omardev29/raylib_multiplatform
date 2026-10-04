// What is placed in the level and is not alive: the moving platform, coins,
// the key and its door, spikes, signs and the flag.

#include "game.h"
#include "objects.h"

#include <rmp/assets.h>
#include <rmp/audio.h>

#include <cmath>

namespace game {

namespace {
bool is_player(const rmp::Object &other) {
    return (other.collision_layer & layer::PLAYER) != 0;
}

// The top-left corner of an object's box, which is where tiles are drawn from.
Vector2 corner(const rmp::Object &object) {
    const Rectangle box = object.world_collider();
    return Vector2{ box.x, box.y };
}
} // namespace

// ---- the moving platform ----------------------------------------------------

void Platform::_ready() {
    // Solid and immovable: the floor, as far as the player is concerned. The
    // box is LDtk's, set by the factory.
    solid = true;
    immovable = true;
    collision_layer = layer::SOLID;
    collision_mask = layer::PLAYER;
}

void Platform::_update(float delta) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float length = std::sqrt((dx * dx) + (dy * dy));
    if (length <= 0) return;

    // There and back, with a pause at either end: a lift that turns round the
    // instant it arrives is level with the ledge for one frame, and nobody can
    // step off in one frame.
    const Vector2 before = position;
    if (_waiting > 0) {
        _waiting -= delta;
    } else {
        _travelled += static_cast<float>(_direction) * speed * delta / length;
        if (_travelled >= 1 || _travelled <= 0) {
            _travelled = _travelled >= 1 ? 1.0f : 0.0f;
            _direction = -_direction;
            _waiting = 0.6f;
        }
    }
    position = { from.x + (dx * _travelled), from.y + (dy * _travelled) };
    carry_rider(before);
}

// Whoever stands on it goes with it. Moving the platform is not enough: the
// player on top would stay where it was, and the platform would slide out from
// under its feet.
void Platform::carry_rider(Vector2 before) {
    rmp::Object *player = rider.get();
    if (player == nullptr) return;
    const Rectangle me = world_collider();
    const Rectangle them = player->world_collider();
    const float feet = them.y + them.height;

    // Standing on it: feet on its top as it was before this frame's move,
    // within a couple of pixels, and over it across.
    const Rectangle was{ me.x - (position.x - before.x), me.y - (position.y - before.y),
                         me.width, me.height };
    const bool on_top = std::fabs(feet - was.y) < 3 && them.x + them.width > was.x &&
        them.x < was.x + was.width;
    // Not riding: jumping off it (on its way up), or stepping off onto a ledge
    // while the platform goes down -- taking it down too would put it inside
    // the ledge.
    const bool jumping = player->velocity.y < 0;
    const bool going_down = position.y > before.y;
    const bool on_a_ledge = scene() != nullptr &&
        scene()->map.solid_in(Rectangle{ them.x, feet, them.width, 1 });
    if (!on_top || jumping || (going_down && on_a_ledge)) return;

    // Across by as much as it moved, and up or down to stand exactly on its top.
    player->position.x += position.x - before.x;
    player->position.y += me.y - feet;
}

void Platform::_draw() {
    const Vector2 at = corner(*this);
    draw_tile(tiles::PLATFORM_LEFT, at);
    draw_tile(tiles::PLATFORM_MIDDLE, { at.x + TILE, at.y });
    draw_tile(tiles::PLATFORM_RIGHT, { at.x + (2 * TILE), at.y });
}

// ---- coins, the key, the door -------------------------------------------------

void Coin::_ready() {
    sprite.sheet = rmp::assets::load_sheet("coin.aseprite");
    sprite.play("spin");
    collider = rmp::rect({ 10, 10 });
    collision_layer = layer::PICKUP;
    collision_mask = layer::PLAYER;
}

void Coin::_collision(rmp::Object &other) {
    if (!alive() || !is_player(other)) return;
    run().coins += value;
    run().gone.push_back(iid);
    rmp::audio::play("coin", { .volume = 0.7f });
    destroy();
}

void Key::_ready() {
    collider = rmp::rect({ 14, 10 });
    collision_layer = layer::PICKUP;
    collision_mask = layer::PLAYER;
}

void Key::_update(float delta) { _clock += delta; }

void Key::_draw() {
    // It bobs up and down, so it reads as a thing to take.
    const float bob = std::sin(_clock * 3) * 2;
    draw_tile(tiles::KEY, { position.x - (TILE / 2), position.y - (TILE / 2) + bob });
}

void Key::_collision(rmp::Object &other) {
    if (!alive() || !is_player(other)) return;
    run().keys.push_back(opens);
    run().gone.push_back(iid);
    rmp::audio::play("key");
    destroy();
}

void Door::_ready() {
    solid = true;
    immovable = true;
    collision_layer = layer::SOLID;
    collision_mask = layer::PLAYER;
}

void Door::_draw() {
    const Vector2 at = corner(*this);
    // One tile per row the door is tall.
    for (int row = 0; static_cast<float>(row) * TILE + 1 < world_collider().height; row++)
        draw_tile(tiles::DOOR, { at.x, at.y + static_cast<float>(row) * TILE });
}

void Door::_collision(rmp::Object &other) {
    if (!alive() || !is_player(other) || !contains(run().keys, iid.c_str())) return;
    run().gone.push_back(iid);
    rmp::audio::play("door");
    destroy();
}

// ---- what hurts, what talks, what wins ----------------------------------------

void Spikes::_ready() {
    collision_layer = layer::HAZARD;
    collision_mask = layer::PLAYER;
}

void Spikes::_draw() {
    // The collider is the points only, the bottom of the cells; the art fills
    // the cells, so it is drawn from the cells' corner.
    const float width = world_collider().width + 4;
    const Vector2 at{ position.x - (width / 2), position.y - (TILE / 2) };
    for (int column = 0; static_cast<float>(column) * TILE + 1 < width; column++)
        draw_tile(tiles::SPIKES, { at.x + static_cast<float>(column) * TILE, at.y });
}

void Spikes::_collision(rmp::Object &other) {
    if (is_player(other)) static_cast<Player &>(other).hurt(position.x);
}

void Sign::_ready() {
    // The box is where the player can read it from, not the sign itself.
    collider = rmp::rect({ 54, 36 });
    collision_layer = layer::SIGN;
    collision_mask = layer::PLAYER;
}

void Sign::_update(float delta) { _player_near -= delta; }

void Sign::_draw() {
    draw_tile(tile, { position.x - (TILE / 2), position.y - (TILE / 2) });
    if (_player_near <= 0 || text.empty()) return;
    // World space, in raylib's default font at its own 10 px: the camera's
    // zoom makes it as chunky as the art.
    const int width = MeasureText(text.c_str(), 10);
    const auto x = static_cast<int>(position.x) - (width / 2);
    const auto y = static_cast<int>(position.y) - 30;
    DrawRectangle(x - 3, y - 2, width + 6, 14, Color{ 20, 24, 40, 200 });
    DrawText(text.c_str(), x, y, 10, RAYWHITE);
}

void Sign::_collision(rmp::Object &other) {
    if (is_player(other)) _player_near = 0.15f;
}

void Goal::_ready() {
    collider = rmp::rect({ 10, 36 });
    collision_layer = layer::GOAL;
    collision_mask = layer::PLAYER;
}

void Goal::_draw() {
    // The pole in the bottom cell, the flag waving in the top one.
    const Rectangle box = world_collider();
    const float left = position.x - (TILE / 2);
    draw_tile(tiles::POLE, { left, box.y + box.height - TILE });
    const int wave = static_cast<int>(GetTime() * 4) % 2; // four times a second
    draw_tile(tiles::FLAG + wave, { left, box.y + box.height - (2 * TILE) });
}

void Goal::_collision(rmp::Object &other) {
    if (is_player(other)) static_cast<LevelScene *>(scene())->win();
}

} // namespace game
