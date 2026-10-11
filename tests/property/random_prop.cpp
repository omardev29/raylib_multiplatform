// ---------------------------------------------------------------------------
// rmp::random, held to rmp/random.h for any seed and any argument a game can
// pass -- the hostile ones above all: ends a subtraction cannot hold, NaN and
// the infinities, every int.
//
// The generator itself is checked against a reference written here from the
// published algorithms (SplitMix64 to expand the seed, xoshiro128++ 1.0), and
// not against src/rmp/random.cpp's own code: a reference that shares code with
// what it checks is checking nothing. range(INT_MIN, INT_MAX) is the window
// onto the whole 32-bit word, and value() onto its top 24 bits.
//
// What the properties found is a named case in the `random` suite of
// tests/unit_test.cpp, so it is checked on every `rmp test unit` and not only
// when the seed happens to find it again.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/internal.h"

#include <rmp/random.h>

#include <doctest.h>

#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>

namespace {

// SplitMix64 and xoshiro128++ 1.0 as their authors published them
// (prng.di.unimi.it), with the framework's one choice on top: the state is the
// high halves of the first four SplitMix64 outputs, and a seed of 0 starts
// SplitMix64 from its increment instead.
class Reference {
public:
    explicit Reference(uint64_t seed) {
        uint64_t x = seed != 0 ? seed : 0x9E3779B97F4A7C15ULL;
        for (uint32_t &word : _s) {
            x += 0x9E3779B97F4A7C15ULL;
            uint64_t z = x;
            z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
            word = static_cast<uint32_t>((z ^ (z >> 31U)) >> 32U);
        }
    }

    uint32_t next() {
        const uint32_t result = rotl(_s[0] + _s[3], 7) + _s[0];
        const uint32_t t = _s[1] << 9U;
        _s[2] ^= _s[0];
        _s[3] ^= _s[1];
        _s[1] ^= _s[2];
        _s[0] ^= _s[3];
        _s[2] ^= t;
        _s[3] = rotl(_s[3], 11);
        return result;
    }

private:
    static uint32_t rotl(uint32_t x, unsigned k) { return (x << k) | (x >> (32U - k)); }
    std::array<uint32_t, 4> _s{};
};

// The word range(INT_MIN, INT_MAX) is: every int, so the whole of it.
uint32_t word_of(int drawn) { return static_cast<uint32_t>(drawn) ^ 0x80000000U; }

// Every int, its ends and their neighbours far more often than an arbitrary
// draw would pick them.
rc::Gen<int> any_int() {
    return rc::gen::weightedOneOf<int>({
        { 1, rc::gen::element(INT_MIN, INT_MIN + 1, -1, 0, 1, INT_MAX - 1, INT_MAX) },
        { 2, rc::gen::arbitrary<int>() },
    });
}

rc::Gen<float> finite_float() {
    return rc::gen::suchThat(rmp::test::gen::hostile_float(),
                             [](float f) { return std::isfinite(f); });
}

// The value() the sequence of `seed` would give at position `skip`: what a
// call that consumes nothing leaves for the next one.
float value_after(uint64_t seed, int skip) {
    rmp::random::seed(seed);
    for (int i = 0; i < skip; ++i) (void)rmp::random::value();
    return rmp::random::value();
}

// Seeds whose first value() is the smallest and the largest it can be,
// exactly: 0 and 1 - 2^-24. Found by searching with Reference above; the
// `random` suite of tests/unit_test.cpp holds each to what it says.
constexpr uint64_t FIRST_VALUE_ZERO = 1319552;
constexpr uint64_t FIRST_VALUE_LARGEST = 12415944;

} // namespace

TEST_SUITE("property: random") {
    // covers: rmp::random::seed(uint64_t)
    // covers: rmp::random::value()
    TEST_CASE("the generator is xoshiro128++ seeded through SplitMix64") {
        rmp::test::check("random: range(INT_MIN, INT_MAX) is the reference's whole word",
                         [](uint64_t seed) {
                             Reference reference(seed);
                             rmp::random::seed(seed);
                             for (int i = 0; i < 8; ++i)
                                 RC_ASSERT(word_of(rmp::random::range(
                                               INT_MIN, INT_MAX)) == reference.next());
                         });
        rmp::test::check("random: value() is the reference word's top 24 bits over 2^24",
                         [](uint64_t seed) {
                             Reference reference(seed);
                             rmp::random::seed(seed);
                             for (int i = 0; i < 8; ++i) {
                                 const float expected =
                                     static_cast<float>(reference.next() >> 8U) /
                                     16777216.0F;
                                 RC_ASSERT(rmp::random::value() == expected);
                             }
                         });
    }

    // covers: rmp::random::current_seed()
    TEST_CASE("the seed is the run") {
        rmp::test::check("random: current_seed() is what seed() was handed",
                         [](uint64_t seed) {
                             rmp::random::seed(seed);
                             (void)rmp::random::value();
                             RC_ASSERT(rmp::random::current_seed() == seed);
                         });
        rmp::test::check("random: seeding with current_seed() replays the run",
                         [](uint64_t seed) {
                             rmp::random::seed(seed);
                             std::array<float, 6> first{};
                             for (float &f : first) f = rmp::random::value();
                             rmp::random::seed(rmp::random::current_seed());
                             for (const float f : first)
                                 RC_ASSERT(rmp::random::value() == f);
                         });
        rmp::test::check("random: value() is in [0, 1)", [](uint64_t seed) {
            rmp::random::seed(seed);
            for (int i = 0; i < 16; ++i) {
                const float v = rmp::random::value();
                RC_ASSERT(v >= 0.0F);
                RC_ASSERT(v < 1.0F);
            }
        });
    }

    // covers: rmp::random::range(int, int)
    TEST_CASE("range(int) for every pair of ints") {
        rmp::test::check("random: range(int) is inside [min, max] for any ends",
                         [](uint64_t seed) {
                             const int min = *any_int();
                             const int max = *any_int();
                             rmp::random::seed(seed);
                             for (int i = 0; i < 4; ++i) {
                                 const int drawn = rmp::random::range(min, max);
                                 if (max <= min) {
                                     RC_ASSERT(drawn == min);
                                 } else {
                                     RC_ASSERT(drawn >= min);
                                     RC_ASSERT(drawn <= max);
                                 }
                             }
                         });
        rmp::test::check("random: range(int) with max <= min consumes nothing",
                         [](uint64_t seed) {
                             const int a = *any_int();
                             const int b = *any_int();
                             const int min = a > b ? a : b;
                             const int max = a > b ? b : a;
                             rmp::random::seed(seed);
                             RC_ASSERT(rmp::random::range(min, max) == min);
                             const float next = rmp::random::value();
                             RC_ASSERT(next == value_after(seed, 0));
                         });
        rmp::test::check("random: range(int) draws one number, whatever its span",
                         [](uint64_t seed) {
                             const int min = *any_int();
                             const int max = *any_int();
                             RC_PRE(max > min);
                             rmp::random::seed(seed);
                             (void)rmp::random::range(min, max);
                             const float next = rmp::random::value();
                             RC_ASSERT(next == value_after(seed, 1));
                         });
    }

    // covers: rmp::random::range(float, float)
    TEST_CASE("range(float) for every pair of floats") {
        rmp::test::check(
            "random: range(float) stays between finite ends and never reaches max",
            [](uint64_t seed) {
                const float min = *finite_float();
                const float max = *finite_float();
                // The two seeds whose first value() is 0 and 1 - 2^-24, as often
                // as a random one: the ends are where it goes wrong.
                const uint64_t start = *rc::gen::weightedOneOf<uint64_t>(
                    { { 1, rc::gen::element(FIRST_VALUE_ZERO, FIRST_VALUE_LARGEST) },
                      { 1, rc::gen::just(seed) } });
                rmp::random::seed(start);
                for (int i = 0; i < 4; ++i) {
                    const float drawn = rmp::random::range(min, max);
                    RC_ASSERT(std::isfinite(drawn));
                    if (min < max) {
                        RC_ASSERT(drawn >= min);
                        RC_ASSERT(drawn < max);
                    } else if (min > max) {
                        RC_ASSERT(drawn <= min);
                        RC_ASSERT(drawn > max);
                    } else {
                        RC_ASSERT(drawn == min);
                    }
                }
            });
        rmp::test::check("random: range(float) with a NaN or infinite end is a finite "
                         "answer, said once",
                         [] {
                             const float min = *rmp::test::gen::hostile_float();
                             const float max = *rmp::test::gen::hostile_float();
                             RC_PRE(!std::isfinite(min) || !std::isfinite(max));
                             rmp::detail::reset_reports_for_tests();
                             const float first = rmp::random::range(min, max);
                             const float second = rmp::random::range(min, max);
                             RC_ASSERT(first == second);
                             if (std::isfinite(min)) {
                                 RC_ASSERT(first == min);
                             } else if (std::isfinite(max)) {
                                 RC_ASSERT(first == max);
                             } else {
                                 RC_ASSERT(first == 0.0F);
                             }
                             RC_ASSERT(rmp::detail::report_count() == 1);
                         });
        rmp::test::check(
            "random: range(float) draws exactly one number, whatever its ends",
            [](uint64_t seed) {
                const float min = *rmp::test::gen::hostile_float();
                const float max = *rmp::test::gen::hostile_float();
                rmp::random::seed(seed);
                (void)rmp::random::range(min, max);
                const float next = rmp::random::value();
                RC_ASSERT(next == value_after(seed, 1));
            });
    }

    // covers: rmp::random::chance(float)
    TEST_CASE("chance() for every probability") {
        rmp::test::check(
            "random: chance(p) is never for p <= 0 and always for p >= 1",
            [](uint64_t seed) {
                const float p =
                    *rc::gen::suchThat(rmp::test::gen::hostile_float(),
                                       [](float f) { return !std::isnan(f); });
                RC_PRE(p <= 0 || p >= 1);
                const uint64_t start = *rc::gen::weightedOneOf<uint64_t>(
                    { { 1, rc::gen::element(FIRST_VALUE_ZERO, FIRST_VALUE_LARGEST) },
                      { 1, rc::gen::just(seed) } });
                rmp::random::seed(start);
                RC_ASSERT(rmp::random::chance(p) == (p >= 1));
            });
        rmp::test::check("random: chance(p) is value() < p, one number each",
                         [](uint64_t seed) {
                             const float p = *rc::gen::inRange(0, 1001) / 1000.0F;
                             const float v = value_after(seed, 0);
                             rmp::random::seed(seed);
                             RC_ASSERT(rmp::random::chance(p) == (v < p));
                             const float next = rmp::random::value();
                             RC_ASSERT(next == value_after(seed, 1));
                         });
        rmp::test::check("random: chance(NaN) is never, said once", [](uint64_t seed) {
            rmp::detail::reset_reports_for_tests();
            rmp::random::seed(seed);
            const float nan = std::numeric_limits<float>::quiet_NaN();
            RC_ASSERT(!rmp::random::chance(nan));
            RC_ASSERT(!rmp::random::chance(-nan));
            RC_ASSERT(rmp::detail::report_count() == 1);
        });
    }

    // covers: rmp::random::in_circle(float)
    // covers: rmp::random::direction()
    TEST_CASE("in_circle() and direction() for every radius") {
        rmp::test::check(
            "random: in_circle(r) lands within |r| for any finite radius",
            [](uint64_t seed) {
                const float radius = *finite_float();
                rmp::random::seed(seed);
                for (int i = 0; i < 4; ++i) {
                    const Vector2 p = rmp::random::in_circle(radius);
                    RC_ASSERT(std::isfinite(p.x));
                    RC_ASSERT(std::isfinite(p.y));
                    const double reach =
                        std::hypot(static_cast<double>(p.x), static_cast<double>(p.y));
                    // Relative, and four of the smallest denormals besides: a radius
                    // of a few denormals rounds by as much as itself.
                    RC_ASSERT(reach <=
                              (std::fabs(static_cast<double>(radius)) * (1 + 1e-6)) +
                                  (4.0 * std::numeric_limits<float>::denorm_min()));
                }
            });
        rmp::test::check(
            "random: in_circle() of a NaN or infinite radius is the centre, said once",
            [](uint64_t seed) {
                const float radius =
                    *rc::gen::element(std::numeric_limits<float>::quiet_NaN(),
                                      std::numeric_limits<float>::infinity(),
                                      -std::numeric_limits<float>::infinity());
                rmp::detail::reset_reports_for_tests();
                rmp::random::seed(seed);
                for (int i = 0; i < 2; ++i) {
                    const Vector2 p = rmp::random::in_circle(radius);
                    RC_ASSERT(p.x == 0.0F);
                    RC_ASSERT(p.y == 0.0F);
                }
                RC_ASSERT(rmp::detail::report_count() == 1);
            });
        rmp::test::check("random: in_circle() draws two numbers, whatever its radius",
                         [](uint64_t seed) {
                             const float radius = *rmp::test::gen::hostile_float();
                             rmp::random::seed(seed);
                             (void)rmp::random::in_circle(radius);
                             const float next = rmp::random::value();
                             RC_ASSERT(next == value_after(seed, 2));
                         });
        rmp::test::check("random: direction() is a unit vector, one number each",
                         [](uint64_t seed) {
                             rmp::random::seed(seed);
                             const Vector2 d = rmp::random::direction();
                             RC_ASSERT(std::fabs(std::hypot(d.x, d.y) - 1.0F) < 1e-5F);
                             const float next = rmp::random::value();
                             RC_ASSERT(next == value_after(seed, 1));
                         });
    }

    // covers: rmp::random::index(int)
    TEST_CASE("index() for every count") {
        rmp::test::check("random: index(n) is in [0, n), and 0 with nothing to pick",
                         [](uint64_t seed) {
                             const int count = *any_int();
                             rmp::random::seed(seed);
                             for (int i = 0; i < 4; ++i) {
                                 const int picked = rmp::random::index(count);
                                 if (count <= 0) {
                                     RC_ASSERT(picked == 0);
                                 } else {
                                     RC_ASSERT(picked >= 0);
                                     RC_ASSERT(picked < count);
                                 }
                             }
                         });
        rmp::test::check("random: index(n) is the word modulo n", [](uint64_t seed) {
            const int count = *rc::gen::inRange(1, INT_MAX);
            Reference reference(seed);
            rmp::random::seed(seed);
            RC_ASSERT(rmp::random::index(count) ==
                      static_cast<int>(reference.next() % static_cast<uint32_t>(count)));
        });
    }
}
