// The player: Platformer does the running and the jumping; this is the hit,
// the stomp, and which animation to show.

#include "game.h"
#include "objects.h"

#include <rmp/assets.h>
#include <rmp/audio.h>
#include <rmp/behavior.h>
#include <rmp/input.h>

#include <cmath>

namespace game {

namespace {
constexpr float GRACE = 1.5f; // seconds of not being hurt again
constexpr float KNOCK = 0.3f; // seconds without control, thrown back
int jumps_seen = 0;
} // namespace

void Player::_ready() {
    // Space, W or Up, or the gamepad's bottom button. Defining an action again
    // replaces it, so this is safe in a scene that is entered again and again.
    rmp::input::action("jump", KEY_SPACE, KEY_W, KEY_UP, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);

    sprite.sheet = rmp::assets::load_sheet("player.aseprite");
    sprite.play("idle");
    // The alien is 20 px of a 24 px frame, standing on its bottom edge: a box
    // a little narrower than it, with its feet where the picture's are.
    collider = rmp::rect({ 12, 20 });
    collider.offset = { 0, 2 };
    collision_layer = layer::PLAYER;
    collision_mask = layer::ENEMY | layer::PICKUP | layer::HAZARD | layer::SOLID |
        layer::GOAL | layer::SIGN;
    layer = 5;
    add<rmp::behavior::Platformer>({
        .speed = 130,
        .acceleration = 1400,
        .gravity = 1100,
        .jump = 380, // 3.6 tiles high, 4.5 long at full speed
        .jump_action = "jump",
    });
    jumps_seen = 0;
}

void Player::_update(float delta) {
    const auto *platformer = get<rmp::behavior::Platformer>();
    _grace -= delta;
    _knocked -= delta;

    // Thrown back after a hit: the knock wins over the stick for a moment.
    if (_knocked > 0) velocity.x = std::copysign(110.0f, velocity.x);
    // Blinking while it cannot be hurt, so the player can see the grace.
    visible = _grace <= 0 || std::fmod(_grace, 0.2f) < 0.12f;

    // A jump is the Platformer spending one: the sound goes with that, so a
    // buffered jump and a coyote jump sound like any other.
    if (platformer->ours.jumps_used > jumps_seen)
        rmp::audio::play("jump", { .volume = 0.6f });
    jumps_seen = platformer->ours.jumps_used;

    const bool on_ground = platformer->on_ground();
    if (hurting()) {
        sprite.play("hurt");
    } else if (!on_ground) {
        sprite.play("jump");
    } else if (std::fabs(velocity.x) > 10) {
        sprite.play("walk");
    } else {
        sprite.play("idle");
    }
    if (velocity.x < -1) flip_x = false;
    if (velocity.x > 1) flip_x = true;
    _was_on_ground = on_ground;
}

void Player::hurt(float from_x) {
    if (_grace > 0) return;
    _grace = GRACE;
    _knocked = KNOCK;
    velocity = { position.x < from_x ? -110.0f : 110.0f, -240 };
    // The player only ever lives in a LevelScene.
    static_cast<LevelScene *>(scene())->lose_life();
}

void Player::bounce() { velocity.y = -260; }

bool Player::falling_onto(const rmp::Object &other) const {
    if (velocity.y <= 0) return false;
    const Rectangle me = world_collider();
    const Rectangle it = other.world_collider();
    // The feet are in the top part of it: this frame's fall is at most a few
    // pixels, so a stomp lands within the first half of the enemy.
    return me.y + me.height - it.y < it.height / 2;
}

} // namespace game
