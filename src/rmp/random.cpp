// ===========================================================================
// rmp::random — see include/rmp/random.h.
//
// xoshiro128++ 1.0, public domain (Blackman & Vigna). Chosen over the obvious
// alternatives for reasons that matter here and nowhere else:
//
//   rand()          global C state, terrible low bits, and every library that
//                   calls srand() fights you for it.
//   std::mt19937    2.5 KB of state, slow to seed, and <random> costs about
//                   80 ms of compile time in every translation unit that wants
//                   a number between 0 and 1.
//   GetRandomValue  raylib's, and it keeps its state inside raylib — which is
//                   the thing this file exists to avoid.
//
// xoshiro128++ is 16 bytes of state and a handful of shifts. Its statistical
// quality is far beyond what a game needs; what it is NOT is unpredictable to
// an attacker, and that is written down in the header.
// ===========================================================================

#include <rmp/random.h>

#include <cmath>

namespace rmp::random {

namespace {

uint32_t g_state[4] = { 0x2545F491u, 0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u };
uint64_t g_seed = 0;

constexpr uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

uint32_t next_u32() {
    const uint32_t result = rotl(g_state[0] + g_state[3], 7) + g_state[0];
    const uint32_t t = g_state[1] << 9;
    g_state[2] ^= g_state[0];
    g_state[3] ^= g_state[1];
    g_state[1] ^= g_state[2];
    g_state[0] ^= g_state[3];
    g_state[2] ^= t;
    g_state[3] = rotl(g_state[3], 11);
    return result;
}

} // namespace

void seed(uint64_t value) {
    g_seed = value;
    // SplitMix64 to expand one number into four words. Seeding xoshiro's state
    // directly from a small value leaves it with almost no set bits, and the
    // first few outputs come out visibly poor — the author says to do this.
    uint64_t x = value ? value : 0x9E3779B97F4A7C15ull;
    for (uint32_t &word : g_state) {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        word = static_cast<uint32_t>((z ^ (z >> 31)) >> 32);
    }
}

uint64_t current_seed() { return g_seed; }

float value() {
    // The top 24 bits, which are the good ones in any xorshift family, scaled
    // into [0, 1). Dividing the whole 32-bit word by 2^32 loses precision in a
    // way that shows up as banding when you use it for angles.
    return static_cast<float>(next_u32() >> 8) * (1.0f / 16777216.0f);
}

float range(float min, float max) { return min + (max - min) * value(); }

int range(int min, int max) {
    if (max <= min) return min;
    // Inclusive at both ends, matching raylib's GetRandomValue so that swapping
    // one for the other does not silently change a game's behaviour.
    const uint32_t span = static_cast<uint32_t>(max - min) + 1u;
    return min + static_cast<int>(next_u32() % span);
}

bool chance(float probability) { return value() < probability; }

Vector2 in_circle(float radius) {
    // sqrt on the radius, and it is not decoration: without it the points bunch
    // up in the middle, because a ring at radius r has area proportional to r.
    const float r = radius * std::sqrt(value());
    const float a = range(0.0f, 6.2831853f);
    return Vector2{ r * std::cos(a), r * std::sin(a) };
}

Vector2 direction() {
    const float a = range(0.0f, 6.2831853f);
    return Vector2{ std::cos(a), std::sin(a) };
}

int index(int count) {
    return count <= 0 ? 0 : static_cast<int>(next_u32() % static_cast<uint32_t>(count));
}

} // namespace rmp::random
