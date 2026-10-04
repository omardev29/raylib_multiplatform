// The walker and the bat, and the one rule they share: stomped from above,
// hurtful from anywhere else.

#include "game.h"
#include "objects.h"

#include <rmp/assets.h>
#include <rmp/audio.h>

#include <cmath>

namespace game {

void Enemy::_collision(rmp::Object &other) {
    if (dying_ >= 0 || (other.collision_layer & layer::PLAYER) == 0) return;
    auto &player = static_cast<Player &>(other);
    if (stompable && player.falling_onto(*this)) {
        player.bounce();
        rmp::audio::play("stomp");
        run().gone.push_back(iid); // stomped for the rest of the run
        // Out of every collision from now on, flattened for a moment, gone.
        collision_layer = 0;
        collision_mask = 0;
        if (const char *tag = squashed_tag()) {
            sprite.play(tag);
            dying_ = 0.4f;
        } else {
            flip_y = true; // what has no squashed frame falls out of the sky
            gravity_scale = 1;
            dying_ = 1.0f;
        }
        return;
    }
    player.hurt(position.x);
}

void Enemy::_update(float delta) {
    if (dying_ >= 0) {
        dying_ -= delta;
        if (dying_ < 0) destroy();
        return;
    }
    patrol(delta);
}

void Walker::_ready() {
    sprite.sheet = rmp::assets::load_sheet("walker.aseprite");
    sprite.play("walk");
    collider = rmp::rect({ 14, 12 });
    collider.offset = { 0, 6 }; // the beetle is the bottom half of its frame
    collision_layer = layer::ENEMY;
    collision_mask = layer::PLAYER;
}

void Walker::patrol(float delta) {
    position.x += static_cast<float>(direction_) * speed * delta;
    if (position.x <= left) {
        position.x = left;
        direction_ = 1;
    } else if (position.x >= right) {
        position.x = right;
        direction_ = -1;
    }
    flip_x = direction_ > 0;
}

void Bat::_ready() {
    sprite.sheet = rmp::assets::load_sheet("bat.aseprite");
    sprite.play("fly");
    collider = rmp::rect({ 16, 12 });
    collision_layer = layer::ENEMY;
    collision_mask = layer::PLAYER;
}

void Bat::patrol(float delta) {
    if (route.size() < 2) return;
    const Vector2 target = route[next_];
    const Vector2 to{ target.x - position.x, target.y - position.y };
    const float distance = std::sqrt((to.x * to.x) + (to.y * to.y));
    const float step = speed * delta;
    if (distance <= step) {
        position = target;
        // There and back along the route: turn round at either end.
        if (next_ + 1 >= route.size()) step_ = -1;
        if (next_ == 0) step_ = 1;
        next_ = static_cast<std::size_t>(static_cast<int>(next_) + step_);
        return;
    }
    position.x += to.x / distance * step;
    position.y += to.y / distance * step;
    if (std::fabs(to.x) > 0.5f) flip_x = to.x > 0;
}

} // namespace game
