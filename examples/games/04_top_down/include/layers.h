#pragma once
// ---------------------------------------------------------------------------
// Who is what, in one place.
//
// One bit per kind of thing. Three fields speak these numbers:
// rmp::Object::collision_layer (what I am), rmp::Object::collision_mask (what
// I collide with) and rmp::behavior::Health::hurt_by (what may hurt me). A
// header of its own, because a second scene file would need exactly these.
// ---------------------------------------------------------------------------

namespace layer {
constexpr unsigned PLAYER = 1u << 0;
constexpr unsigned ENEMY = 1u << 1;
constexpr unsigned BULLET = 1u << 2;
constexpr unsigned WORLD = 1u << 3;
constexpr unsigned TRIGGER = 1u << 4; // the door: it sees the player, nothing else
} // namespace layer
