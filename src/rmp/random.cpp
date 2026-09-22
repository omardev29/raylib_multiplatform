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

// SplitMix64 to expand one number into four words. Seeding xoshiro's state
// directly from a small value leaves it with almost no set bits, and the first
// few outputs come out visibly poor -- the author says to do this.
struct State {
    uint32_t word[4];
};

constexpr State expanded(uint64_t value) {
    uint64_t x = value != 0 ? value : 0x9E3779B97F4A7C15ull;
    State out{};
    for (uint32_t &word : out.word) {
        x += 0x9E3779B97F4A7C15ull;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        word = static_cast<uint32_t>((z ^ (z >> 31)) >> 32);
    }
    return out;
}

// What a run starts from when nobody has called seed(). It is a SEED and not a
// hand-picked state, and that is the whole fix: current_seed() is the number a
// pause screen shows and a bug report carries, and handing it back has to give
// the sequence the process was running. It used to be 0 sitting next to four
// constants that seed(0) does not produce, so the number named a different run.
//
// rmp::app::detail::begin_run() replaces it with the clock, so a shipped game
// still behaves differently each time. Nothing in the tests calls begin_run(),
// which is what keeps every headless run reproducible.
constexpr uint64_t kDefaultSeed = 0x2545F4914F6CDD1Dull;

// Expanded at COMPILE TIME, and then copied. Both halves matter: a run that
// never calls seed() has to start from exactly the state seed(kDefaultSeed)
// produces, or current_seed() names a sequence the process is not running --
// and doing the expansion in a runtime initialiser before main() would be a
// static initialisation order question in the one file whose whole promise is
// that the first value is the same every time.
constexpr State kDefaultState = expanded(kDefaultSeed);

constinit State g_state = kDefaultState;
constinit uint64_t g_seed = kDefaultSeed;

constexpr uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

uint32_t next_u32() {
    uint32_t *s = g_state.word;
    const uint32_t result = rotl(s[0] + s[3], 7) + s[0];
    const uint32_t t = s[1] << 9;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 11);
    return result;
}

} // namespace

void seed(uint64_t value) {
    g_seed = value;
    g_state = expanded(value);
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
