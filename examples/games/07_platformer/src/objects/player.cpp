// The player. rmp::behavior::Platformer does the running and the jumping; this
// is what belongs to this game: being hit, the stomp, the sound of a jump and
// which animation to show.

#include "game.h"
#include "objects.h"

#include <rmp/assets.h>
#include <rmp/audio.h>
#include <rmp/behavior.h>
#include <rmp/input.h>

#include <cmath>

namespace game {

namespace {
// After a hit it cannot be hurt again for GRACE_SECONDS, and for KNOCK_SECONDS
// it is thrown back at KNOCK_SPEED, out of control.
constexpr float GRACE_SECONDS = 1.5f;
constexpr float KNOCK_SECONDS = 0.3f;
constexpr float KNOCK_SPEED = 110;
} // namespace

void Player::_ready() {
    // Space, W or Up, or the gamepad's bottom button.
    rmp::input::action("jump", KEY_SPACE, KEY_W, KEY_UP, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);

    sprite.sheet = rmp::assets::load_sheet("player.aseprite");
    sprite.play("idle");
    // The alien fills 20 px of its 24 px frame and stands on the bottom edge.
    collider = rmp::rect({ 12, 20 });
    collider.offset = { 0, 2 };
    collision_layer = layer::PLAYER;
    collision_mask = layer::ENEMY | layer::PICKUP | layer::HAZARD | layer::SOLID |
        layer::GOAL | layer::SIGN;
    layer = 5; // in front of everything else
    add<rmp::behavior::Platformer>({
        .speed = 130,
        .acceleration = 1400,
        .gravity = 1100,
        .jump = 380, // 3.6 tiles high, 4.5 long at full speed
        .jump_action = "jump",
    });
}

void Player::_update(float delta) {
    _grace -= delta;
    _knocked -= delta;

    // Thrown back after a hit: the knock wins over the controls for a moment.
    if (_knocked > 0) velocity.x = velocity.x < 0 ? -KNOCK_SPEED : KNOCK_SPEED;

    // Blinking while it cannot be hurt: shown for 0.12 s of every 0.2 s.
    visible = _grace <= 0 || std::fmod(_grace, 0.2f) < 0.12f;

    // The jump sound. Platformer counts the jumps it makes, so when the count
    // goes up a jump has just happened -- a buffered or a coyote jump too.
    const auto platformer = get<rmp::behavior::Platformer>();
    if (platformer->ours.jumps_used > _jumps_heard)
        rmp::audio::play("jump", { .volume = 0.6f });
    _jumps_heard = platformer->ours.jumps_used;

    if (hurting()) {
        sprite.play("hurt");
    } else if (!platformer->grounded()) {
        sprite.play("jump");
    } else if (std::fabs(velocity.x) > 10) {
        sprite.play("walk");
    } else {
        sprite.play("idle");
    }

    // The art faces left: turn it round while it moves right.
    if (velocity.x < -1) flip_x = false;
    if (velocity.x > 1) flip_x = true;
}

void Player::hurt(float from_x) {
    if (_grace > 0) return;
    _grace = GRACE_SECONDS;
    _knocked = KNOCK_SECONDS;
    velocity = { position.x < from_x ? -KNOCK_SPEED : KNOCK_SPEED, -240 };
    // The player only ever lives in a LevelScene.
    static_cast<LevelScene &>(scene()).lose_life();
}

void Player::bounce() { velocity.y = -260; }

bool Player::falling_onto(const rmp::Object &other) const {
    if (velocity.y <= 0) return false;
    const Rectangle me = world_collider();
    const Rectangle it = other.world_collider();
    // The feet are in the top half of it. One frame of falling is a few pixels,
    // so a stomp always lands there.
    return me.y + me.height - it.y < it.height / 2;
}

} // namespace game
