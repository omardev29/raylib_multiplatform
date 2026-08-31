#pragma once
// ---------------------------------------------------------------------------
// rmp::random — numbers you can reproduce.
//
//     float angle = rmp::random::range(0.0f, 360.0f);
//     Vector2 spot = rmp::random::in_circle(80);
//
// It exists for two reasons and the second one is the one that surprises people.
//
// 1. TESTS. raylib's GetRandomValue() keeps its state inside raylib and is
//    seeded from the clock, so a test of anything that varies — a spawner's
//    jitter, a tween's wobble, a wander — would give a different answer every
//    run. A test that fails one time in twenty is worse than no test: it
//    teaches everyone to ignore the colour red.
//
// 2. THE GAME. Same seed, same run. That is what makes a roguelike seed
//    shareable, what makes "it happened at minute 12" reproducible from a bug
//    report, and what lets a replay be a list of inputs instead of a video.
//
// The generator is xoshiro128++: about ten lines, no allocation, no global C
// state, and good enough for a game by a wide margin. It is NOT for anything
// that needs to be unguessable — no shuffling of cards for money, no tokens.
// ---------------------------------------------------------------------------

#include <raylib.h> // Vector2
#include <cstdint>

namespace rmp::random {

// Start the sequence. Called for you at startup with a value taken from the
// clock, so a game that never mentions seeds still behaves differently each
// time. Call it yourself to get the same run twice.
void seed(uint64_t value);

// What the current sequence started from. This is the number you print in a
// bug report, or show on the pause screen of a roguelike.
uint64_t current_seed();

float value(); // 0..1
float range(float min, float max);
int range(int min, int max); // inclusive at both ends, like raylib's
bool chance(float probability); // true with that probability, 0..1

Vector2 in_circle(float radius); // uniform inside the disc, not on the rim
Vector2 direction(); // a unit vector, any angle

// Pick one. Returns 0 for an empty range, which is the only answer that does
// not require the caller to have checked first.
int index(int count);

} // namespace rmp::random
