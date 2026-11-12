// Everyone who walks through town: which way they look, and who is drawn in
// front of whom.

#include "school.h"

#include <cmath>
#include <string>
#include <string_view>

namespace game {

namespace {

// The sheet's tags end in the side they face: idle_s, walk_e.
std::string_view side(Vector2 facing) {
    if (std::abs(facing.x) > std::abs(facing.y)) return facing.x > 0 ? "e" : "w";
    return facing.y > 0 ? "s" : "n";
}

} // namespace

void Character::_update(rmp::Object &self, float /*delta*/) {
    const bool walking = self.velocity.x != 0 || self.velocity.y != 0;
    if (walking) facing = self.velocity;
    self.sprite.play(std::string(walking ? "walk_" : "idle_") +
                     std::string(side(facing)));
    // Every tag is drawn for its own side: nothing is mirrored.
    self.flip_x = false;
    // Lower on the screen is nearer: over whoever, and whatever, stands behind.
    self.layer = static_cast<int>(self.position.y);
}

void face(rmp::Object &who, Vector2 at) {
    if (auto character = who.get<Character>()) {
        character->facing = { at.x - who.position.x, at.y - who.position.y };
    }
}

} // namespace game
