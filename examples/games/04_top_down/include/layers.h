#pragma once
// ---------------------------------------------------------------------------
// examples/games/04_top_down/include/layers.h — who is what, in one place.
//
// One bit per kind of thing. They are the numbers three different fields speak:
// rmp::Object::collision_layer (what I am), rmp::Object::collision_mask (what I
// collide with) and rmp::behavior::Health::hurt_by (what may hurt me) -- so a
// bit written out as `1u << 2` at three call sites is a bug you only find by
// reading the third one.
//
// A header of its own because a second scene file would need exactly these,
// and examples/README.md's layout rule puts what a game shares in include/.
// ---------------------------------------------------------------------------

namespace layer {
constexpr unsigned kPlayer = 1u << 0;
constexpr unsigned kEnemy = 1u << 1;
constexpr unsigned kBullet = 1u << 2;
constexpr unsigned kWorld = 1u << 3;
constexpr unsigned kTrigger = 1u << 4; // the door: it sees the player, nothing else
} // namespace layer
