// ---------------------------------------------------------------------------
// The unit tests. doctest, no window, no GPU, milliseconds.
//
//     rmp test unit                        all of them
//     ./build/debug/unit_test -ts=random    one suite
//     ./build/debug/unit_test -tc="*seed*"  one case
//
// Every phase from here adds its file next to this one and CMake picks it up:
// tests/*_test.cpp is globbed, so a new test file needs no build change. This
// one holds the pieces that have no other home yet.
//
// The rule that keeps them fast is the same one that keeps them possible: no
// file under src/rmp/ reads the clock, the mouse or the global random state on
// its own. The lint job greps for that, so it stays true.
// ---------------------------------------------------------------------------

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "../src/rmp/internal.h"

#include <rmp/assets.h>
#include <rmp/random.h>

#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <utility>

namespace {

// What rmp::random looks like BEFORE any test has touched it. Captured at
// static-initialisation time and not inside a case, because `rmp test` runs
// the suite a second time with --order-by=rand and every other case here seeds
// on purpose -- by the time one of them runs, "unseeded" is gone.
struct StartState {
    uint64_t seed = 0;
    float values[8] = {};
};

StartState capture_start() {
    StartState out;
    out.seed = rmp::random::current_seed();
    for (float &sample : out.values) sample = rmp::random::value();
    return out;
}

const StartState START = capture_start();

} // namespace

TEST_SUITE("random") {
    TEST_CASE("the same seed gives the same sequence") {
        // The whole reason this generator exists instead of raylib's. Without it,
        // a test of anything that varies is a test that fails one run in twenty.
        rmp::random::seed(12345);
        float first[8];
        for (float &f : first) f = rmp::random::value();

        rmp::random::seed(12345);
        for (float f : first) CHECK(rmp::random::value() == doctest::Approx(f));
    }

    TEST_CASE("different seeds diverge") {
        rmp::random::seed(1);
        const float a = rmp::random::value();
        rmp::random::seed(2);
        CHECK(rmp::random::value() != doctest::Approx(a));
    }

    TEST_CASE("seed(0) still produces a usable sequence") {
        // A zero seed is the classic way to leave an xorshift state with no set
        // bits, and the first outputs come out visibly poor. SplitMix64 is what
        // stops that, and this is the test that says so.
        rmp::random::seed(0);
        std::set<float> seen;
        for (int i = 0; i < 64; ++i) seen.insert(rmp::random::value());
        CHECK(seen.size() > 60);
    }

    TEST_CASE("value() stays inside [0, 1)") {
        rmp::random::seed(7);
        for (int i = 0; i < 20000; ++i) {
            const float v = rmp::random::value();
            REQUIRE(v >= 0.0f);
            REQUIRE(v < 1.0f);
        }
    }

    TEST_CASE("range(float) covers its ends without leaving them") {
        rmp::random::seed(7);
        float lo = 1e9f;
        float hi = -1e9f;
        for (int i = 0; i < 20000; ++i) {
            const float v = rmp::random::range(-3.0f, 5.0f);
            REQUIRE(v >= -3.0f);
            REQUIRE(v <= 5.0f);
            lo = std::fmin(lo, v);
            hi = std::fmax(hi, v);
        }
        CHECK(lo < -2.9f);
        CHECK(hi > 4.9f);
    }

    TEST_CASE("range(int) is inclusive at BOTH ends, like raylib's") {
        // Matching GetRandomValue matters: someone will swap one for the other, and
        // an off-by-one there is a die that never rolls a 6.
        rmp::random::seed(3);
        std::set<int> seen;
        for (int i = 0; i < 5000; ++i) {
            const int v = rmp::random::range(1, 6);
            REQUIRE(v >= 1);
            REQUIRE(v <= 6);
            seen.insert(v);
        }
        CHECK(seen.size() == 6);
    }

    TEST_CASE("range(int) with min == max returns it, and min > max does not hang") {
        CHECK(rmp::random::range(4, 4) == 4);
        CHECK(rmp::random::range(9, 2) == 9);
    }

    TEST_CASE("chance(0) never and chance(1) always") {
        rmp::random::seed(11);
        for (int i = 0; i < 2000; ++i) {
            REQUIRE_FALSE(rmp::random::chance(0.0f));
            REQUIRE(rmp::random::chance(1.0f));
        }
    }

    TEST_CASE("chance(p) lands near p") {
        rmp::random::seed(11);
        int hits = 0;
        for (int i = 0; i < 100000; ++i) hits += rmp::random::chance(0.25f) ? 1 : 0;
        CHECK(hits > 24000);
        CHECK(hits < 26000);
    }

    TEST_CASE("in_circle stays inside, and does not bunch up in the middle") {
        // The sqrt in the implementation is what makes this true. Without it the
        // points crowd the centre, because a ring at radius r has area ~ r.
        rmp::random::seed(5);
        int outer_half = 0;
        const int samples = 20000;
        for (int i = 0; i < samples; ++i) {
            const Vector2 p = rmp::random::in_circle(10.0f);
            const float d = std::sqrt(p.x * p.x + p.y * p.y);
            REQUIRE(d <= 10.0001f);
            // Half the AREA of a disc is outside r/sqrt(2) = 0.7071 r.
            if (d > 7.071f) ++outer_half;
        }
        CHECK(outer_half > samples * 0.45);
        CHECK(outer_half < samples * 0.55);
    }

    TEST_CASE("direction() is a unit vector, and points every way") {
        rmp::random::seed(5);
        bool quadrant[4] = { false, false, false, false };
        for (int i = 0; i < 4000; ++i) {
            const Vector2 d = rmp::random::direction();
            REQUIRE(std::sqrt(d.x * d.x + d.y * d.y) ==
                    doctest::Approx(1.0f).epsilon(0.001));
            quadrant[(d.x >= 0 ? 0 : 1) + (d.y >= 0 ? 0 : 2)] = true;
        }
        for (bool q : quadrant) CHECK(q);
    }

    TEST_CASE("index() covers the range and survives an empty one") {
        rmp::random::seed(2);
        CHECK(rmp::random::index(0) == 0);
        CHECK(rmp::random::index(-4) == 0);
        std::set<int> seen;
        for (int i = 0; i < 500; ++i) {
            const int v = rmp::random::index(3);
            REQUIRE(v >= 0);
            REQUIRE(v < 3);
            seen.insert(v);
        }
        CHECK(seen.size() == 3);
    }

    TEST_CASE("current_seed reports what was set") {
        rmp::random::seed(0xC0FFEE);
        CHECK(rmp::random::current_seed() == 0xC0FFEE);

        // Including the one value that used to take a different path inside
        // seed(): 0 is a seed like any other and has to be reported back.
        rmp::random::seed(0);
        CHECK(rmp::random::current_seed() == 0);
    }

    TEST_CASE("an unseeded run is the run current_seed() names") {
        // THE point of the number. A game that never mentions a seed still
        // prints one on its pause screen and in its crash report, and handing
        // that number back has to give the sequence the process was running.
        //
        // It did not. The seed sat at 0 while the state sat at four compile-time
        // constants that seed(0) does not produce, so the bug report sent the
        // reader to a different run than the one that crashed.
        CHECK(START.seed != 0);
        rmp::random::seed(START.seed);
        for (float expected : START.values) {
            CHECK(rmp::random::value() == doctest::Approx(expected));
        }
    }

    // --- what tests/property/random_prop.cpp found, and the mutants Mull's
    // pilot left alive, each as a case of its own ------------------------------

    TEST_CASE("the sequence is xoshiro128++ seeded through SplitMix64, word for word") {
        // The reference's words, from the published algorithms (and
        // tests/property/random_prop.cpp's Reference, which checks every seed).
        // range(INT_MIN, INT_MAX) is every int, so it hands back the whole
        // word: nothing else checks the generator's arithmetic bit for bit, and
        // Mull's pilot changed a shift or an xor of it and nothing noticed.
        struct Known {
            uint64_t seed;
            std::array<uint32_t, 6> words;
        };
        const std::array<Known, 5> known{ {
            { 0,
              { 0x478c88ae, 0x87f2cdc9, 0x42b83207, 0x651b850f, 0xe3827e2e,
                0x0374b568 } },
            { 1,
              { 0xf6e46bed, 0x9afad234, 0x924aca21, 0x069ee6f4, 0x96ba1e8a,
                0x8cd49558 } },
            { 12345,
              { 0xb5d9ed7f, 0x3a7e17c3, 0xc8843f4a, 0x7ec6aff0, 0xc4c569f8,
                0xf791d9a3 } },
            { 0x2545F4914F6CDD1Dull, // the default seed
              { 0x336368ab, 0x6dab09b5, 0x13684162, 0xde84d920, 0xb741dae2,
                0x11136b8d } },
            { UINT64_MAX,
              { 0xe06be31f, 0xba268ab5, 0xb25635ec, 0xc7abea61, 0xc3d151ba,
                0xfef50fc4 } },
        } };
        for (const Known &k : known) {
            CAPTURE(k.seed);
            rmp::random::seed(k.seed);
            for (const uint32_t word : k.words) {
                CHECK((static_cast<uint32_t>(rmp::random::range(INT_MIN, INT_MAX)) ^
                       0x80000000u) == word);
            }
            rmp::random::seed(k.seed);
            for (const uint32_t word : k.words) {
                CHECK(rmp::random::value() ==
                      static_cast<float>(word >> 8) / 16777216.0f);
            }
        }
        CHECK(START.seed == 0x2545F4914F6CDD1Dull);
    }

    TEST_CASE("range(int) over its widest spans: no overflow, and every int is one") {
        // max - min of INT_MAX and INT_MIN was signed overflow, undefined
        // behaviour -- UBSan's first report in random_prop.cpp -- and the span
        // of every int wrapped to 0 in a uint32_t, so `% span` was a division
        // by zero.
        rmp::random::seed(1);
        CHECK(rmp::random::range(INT_MIN, INT_MAX) == 0x76e46bed);
        for (int i = 0; i < 2000; ++i) {
            const int wide = rmp::random::range(INT_MIN + 1, INT_MAX);
            REQUIRE(wide >= INT_MIN + 1);
            const int low = rmp::random::range(INT_MIN, -1);
            REQUIRE(low < 0);
            const int high = rmp::random::range(-1, INT_MAX);
            REQUIRE(high >= -1);
            const int top = rmp::random::range(INT_MAX - 1, INT_MAX);
            REQUIRE(top >= INT_MAX - 1);
            const int bottom = rmp::random::range(INT_MIN, INT_MIN + 1);
            REQUIRE(bottom <= INT_MIN + 1);
        }
    }

    TEST_CASE("range(int) with max == min moves nothing, and one with room moves once") {
        // `max <= min` and not `<`: equal ends answer without drawing. Mull's
        // pilot changed it to `<`, which draws a number for (4, 4) and answers
        // 4 all the same -- only the NEXT number tells.
        rmp::random::seed(99);
        const float first = rmp::random::value();
        const float second = rmp::random::value();
        rmp::random::seed(99);
        CHECK(rmp::random::range(4, 4) == 4);
        CHECK(rmp::random::range(9, 2) == 9);
        CHECK(rmp::random::value() == first);
        rmp::random::seed(99);
        (void)rmp::random::range(4, 5);
        CHECK(rmp::random::value() == second);
    }

    TEST_CASE("range(float) never lands on max, even where rounding puts it there") {
        // random_prop.cpp's counterexample: the seed whose first value() is
        // the largest, 1 - 2^-24, and -FLT_MIN to 0 -- whose answer rounded to
        // -0, which IS 0, which is max.
        rmp::random::seed(12415944);
        CHECK(rmp::random::value() == 16777215.0f / 16777216.0f);
        rmp::random::seed(12415944);
        const float drawn = rmp::random::range(-std::numeric_limits<float>::min(), 0.0f);
        CHECK(drawn < 0.0f);
        CHECK(drawn >= -std::numeric_limits<float>::min());
        // And the other way round: from 0 down to -FLT_MIN never lands on it.
        rmp::random::seed(12415944);
        CHECK(rmp::random::range(0.0f, -std::numeric_limits<float>::min()) >
              -std::numeric_limits<float>::min());
        // A gap rounding pushed one float PAST max: -(2^24 + 2) to 1, whose
        // difference rounds up to 2^24 + 4.
        rmp::random::seed(12415944);
        CHECK(rmp::random::range(-16777218.0f, 1.0f) < 1.0f);
    }

    TEST_CASE("range(float) from -FLT_MAX to FLT_MAX is a number between them") {
        // max - min is infinite in a float, and inf * 0 is NaN: at the seed
        // whose first value() is exactly 0 the answer was NaN, and at any
        // other seed it was infinity.
        const float lo = -std::numeric_limits<float>::max();
        const float hi = std::numeric_limits<float>::max();
        rmp::random::seed(1319552);
        CHECK(rmp::random::value() == 0.0f);
        rmp::random::seed(1319552);
        CHECK(rmp::random::range(lo, hi) == lo);
        // At the largest value(), 1 - 2^-24: two floats short of FLT_MAX.
        rmp::random::seed(12415944);
        const float top = rmp::random::range(lo, hi);
        CHECK(top < hi);
        CHECK(top == std::nextafter(std::nextafter(hi, 0.0f), 0.0f));
        rmp::random::seed(3);
        for (int i = 0; i < 2000; ++i) {
            const float drawn = rmp::random::range(lo, hi);
            REQUIRE(std::isfinite(drawn));
            const float down = rmp::random::range(hi, lo);
            REQUIRE(std::isfinite(down));
            REQUIRE(down > lo);
        }
    }

    TEST_CASE("range(float) with an end that is not a number: min, max or 0, said once") {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        rmp::detail::reset_reports_for_tests();
        CHECK(rmp::random::range(nan, 1.0f) == 1.0f);
        CHECK(rmp::random::range(2.0f, nan) == 2.0f);
        CHECK(rmp::random::range(-inf, 3.0f) == 3.0f);
        CHECK(rmp::random::range(4.0f, inf) == 4.0f);
        CHECK(rmp::random::range(inf, 1e30f) == 1e30f); // was NaN
        CHECK(rmp::random::range(nan, -inf) == 0.0f);
        CHECK(rmp::detail::report_count() == 1);
        // And it still takes its one number, so the run goes on the same.
        rmp::random::seed(5);
        (void)rmp::random::value();
        const float second = rmp::random::value();
        rmp::random::seed(5);
        (void)rmp::random::range(nan, nan);
        CHECK(rmp::random::value() == second);
    }

    TEST_CASE("chance(0) at a value() of exactly 0 is still never") {
        // `<` and not `<=`: Mull's pilot changed it, and chance(0) then came
        // true one time in 2^24 -- on the seed whose first value() is 0.
        rmp::random::seed(1319552);
        CHECK(rmp::random::value() == 0.0f);
        rmp::random::seed(1319552);
        CHECK_FALSE(rmp::random::chance(0.0f));
        rmp::random::seed(1319552);
        CHECK(rmp::random::chance(std::numeric_limits<float>::denorm_min()));
    }

    TEST_CASE("chance(NaN) is never, and said once") {
        rmp::detail::reset_reports_for_tests();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        rmp::random::seed(1319552);
        CHECK_FALSE(rmp::random::chance(nan));
        CHECK_FALSE(rmp::random::chance(nan));
        CHECK(rmp::detail::report_count() == 1);
    }

    TEST_CASE("in_circle() of a radius that is not a number is the centre, said once") {
        rmp::detail::reset_reports_for_tests();
        for (const float radius : { std::numeric_limits<float>::quiet_NaN(),
                                    std::numeric_limits<float>::infinity(),
                                    -std::numeric_limits<float>::infinity() }) {
            const Vector2 p = rmp::random::in_circle(radius);
            CHECK(p.x == 0.0f);
            CHECK(p.y == 0.0f);
        }
        CHECK(rmp::detail::report_count() == 1);
        rmp::random::seed(8);
        const Vector2 far = rmp::random::in_circle(std::numeric_limits<float>::max());
        CHECK(std::isfinite(far.x));
        CHECK(std::isfinite(far.y));
    }

} // TEST_SUITE

// ---------------------------------------------------------------------------

TEST_SUITE("resources") {
    // The counting is tested with IMAGE payloads that are all zeros, and that is
    // safe on purpose: raylib's UnloadImage is RL_FREE(image.data), and freeing a
    // null pointer is defined. UnloadTexture guards on id > 0. So the table can be
    // exercised with no window, no GL context and no real asset — which is the only
    // way this could be a unit test at all.
    using rmp::detail::ResourceKind;

    rmp::Image make(const char *name) {
        ::Image zeroed{};
        auto *slot = rmp::detail::adopt_named(ResourceKind::IMAGE, name, 0, zeroed);
        return rmp::Image{ slot };
    }

    TEST_CASE("an empty resource is falsy, and drawing it is not a crash") {
        rmp::Image empty;
        CHECK_FALSE(empty.valid());
        CHECK_FALSE(static_cast<bool>(empty));
        // The point: raw() on an empty resource gives a zeroed struct, not a
        // dereferenced null. raylib draws nothing for one of those.
        const ::Image &raw = empty.raw();
        CHECK(raw.data == nullptr);
        CHECK(raw.width == 0);
    }

    TEST_CASE("the same name is the same resource, counted") {
        rmp::detail::release_all();
        {
            rmp::Image a = make("shared.png");
            CHECK(a.valid());
            CHECK(rmp::detail::ref_count("shared.png") == 1);
            CHECK(rmp::detail::live_count() == 1);

            auto *hit = rmp::detail::acquire_named(ResourceKind::IMAGE, "shared.png", 0);
            REQUIRE(hit != nullptr);
            rmp::Image b{ hit };
            CHECK(rmp::detail::ref_count("shared.png") == 2);
            // Still ONE resource, not two. That is the whole point of the cache.
            CHECK(rmp::detail::live_count() == 1);
        }
        CHECK(rmp::detail::live_count() == 0);
    }

    TEST_CASE("copying shares and the last one out releases") {
        rmp::detail::release_all();
        {
            rmp::Image a = make("copy.png");
            CHECK(rmp::detail::ref_count("copy.png") == 1);
            {
                rmp::Image b = a; // copy
                rmp::Image c(a); // copy again
                CHECK(rmp::detail::ref_count("copy.png") == 3);
            }
            CHECK(rmp::detail::ref_count("copy.png") == 1);
            CHECK(rmp::detail::live_count() == 1);
        }
        CHECK(rmp::detail::live_count() == 0);
    }

    TEST_CASE("moving steals and does NOT release") {
        rmp::detail::release_all();
        rmp::Image a = make("move.png");
        CHECK(rmp::detail::ref_count("move.png") == 1);

        rmp::Image b = std::move(a);
        CHECK(b.valid());
        CHECK_FALSE(a.valid()); // NOLINT: checking the moved-from state IS the test
        // One reference, not two and not zero: the count must not change on a move.
        CHECK(rmp::detail::ref_count("move.png") == 1);
    }

    TEST_CASE("self-assignment does not release what it is holding") {
        // The classic way to write this wrong: release, then retain a dangling
        // slot. Holding the new reference before letting the old one go makes it
        // impossible, and this is the test that says it still does.
        rmp::detail::release_all();
        rmp::Image a = make("self.png");
        rmp::Image &alias = a;
        a = alias;
        CHECK(a.valid());
        CHECK(rmp::detail::ref_count("self.png") == 1);
        CHECK(rmp::detail::live_count() == 1);
        rmp::detail::release_all();
    }

    TEST_CASE("assignment releases the old resource") {
        rmp::detail::release_all();
        rmp::Image a = make("first.png");
        rmp::Image b = make("second.png");
        CHECK(rmp::detail::live_count() == 2);

        a = b;
        CHECK(rmp::detail::ref_count("first.png") == 0);
        CHECK(rmp::detail::ref_count("second.png") == 2);
        CHECK(rmp::detail::live_count() == 1);
        rmp::detail::release_all();
    }

    TEST_CASE("move assignment releases the old resource and takes the new one over") {
        rmp::detail::release_all();
        rmp::Image a = make("old.png");
        rmp::Image b = make("new.png");
        a = std::move(b);
        CHECK(a.valid());
        CHECK_FALSE(b.valid()); // NOLINT: checking the moved-from state IS the test
        CHECK(rmp::detail::ref_count("old.png") == 0);
        // Taken over, not shared: one reference, as before the move.
        CHECK(rmp::detail::ref_count("new.png") == 1);
        CHECK(rmp::detail::live_count() == 1);

        // Moved into itself, a handle keeps what it holds -- the same promise
        // as the copy's, above.
        rmp::Image &alias = a;
        a = std::move(alias);
        CHECK(a.valid());
        CHECK(rmp::detail::ref_count("new.png") == 1);
        CHECK(rmp::detail::live_count() == 1);
        rmp::detail::release_all();
    }

    TEST_CASE("different names are different resources") {
        rmp::detail::release_all();
        rmp::Image a = make("a.png");
        rmp::Image b = make("b.png");
        CHECK(rmp::detail::live_count() == 2);
        CHECK(rmp::detail::acquire_named(ResourceKind::IMAGE, "c.png", 0) == nullptr);
        rmp::detail::release_all();
    }

    TEST_CASE("a font's size is part of its key") {
        // The same face at 16 and at 32 is two baked atlases, so it has to be two
        // resources. Keying on the name alone would hand back the wrong one.
        rmp::detail::release_all();
        ::Font zeroed{};
        auto *at16 = rmp::detail::adopt_named(ResourceKind::FONT, "ui.ttf", 16, zeroed);
        rmp::Font a{ at16 };
        CHECK(rmp::detail::acquire_named(ResourceKind::FONT, "ui.ttf", 32) == nullptr);
        CHECK(rmp::detail::acquire_named(ResourceKind::FONT, "ui.ttf", 16) != nullptr);
        rmp::detail::release_all();
    }

    TEST_CASE("release_all leaves the table empty") {
        rmp::detail::release_all();
        rmp::Image a = make("x.png");
        rmp::Image b = make("y.png");
        CHECK(rmp::detail::live_count() == 2);
        rmp::detail::release_all();
        CHECK(rmp::detail::live_count() == 0);
        // And releasing twice is not a double free.
        rmp::detail::release_all();
        CHECK(rmp::detail::live_count() == 0);
    }

} // TEST_SUITE
