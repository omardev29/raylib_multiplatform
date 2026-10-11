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
#include <rmp/config.h>
#include <cstdint>

namespace rmp::random {

// Start the sequence. Called for you at startup with a value taken from the
// clock, so a game that never mentions seeds still behaves differently each
// time. Call it yourself to get the same run twice.
void seed(uint64_t value);

// What the current sequence started from. This is the number you print in a
// bug report, or show on the pause screen of a roguelike.
//
// IT IS ALWAYS TRUE, including before anything calls seed(): an unseeded run
// starts from a fixed default seed rather than from a state with no number on
// it, so handing this back always gives the sequence the process was running.
// A headless test never goes through the entry point, so it never gets the
// clock and never has to seed to be reproducible.
uint64_t current_seed();

float value(); // 0..1, never reaching 1

// From min up to, never reaching, max -- or down to it, when max is the
// smaller; min itself when they are equal. Any two floats, -FLT_MAX to
// FLT_MAX included. A NaN or an infinite end is a mistake in the game: said
// once, and the answer is min, or max when min is the one that is not a
// number, or 0. One number of the sequence each call, whatever the ends.
float range(float min, float max);

// Inclusive at both ends, like raylib's, for any two ints: range(INT_MIN,
// INT_MAX) is every int. min when max <= min, and then the sequence does not
// move.
int range(int min, int max);

// True with that probability, 0..1: never at 0 or below, always at 1 or
// above. A NaN is never, and said once.
bool chance(float probability);

// Uniform inside the disc, not on the rim. A NaN or infinite radius is the
// centre, {0, 0}, and said once.
Vector2 in_circle(float radius);
Vector2 direction(); // a unit vector, any angle

// Pick one. Returns 0 for an empty range, which is the only answer that does
// not require the caller to have checked first.
int index(int count);

} // namespace rmp::random
