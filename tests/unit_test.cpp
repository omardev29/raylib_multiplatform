// ---------------------------------------------------------------------------
// The unit tests. doctest, no window, no GPU, milliseconds.
//
//     just test unit                  all of them
//     ./build/unit_test -ts=random    one suite
//     ./build/unit_test -tc="*seed*"  one case
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

#include <rmp/random.h>

#include <cmath>
#include <set>

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
        for (int i = 0; i < 64; i++) seen.insert(rmp::random::value());
        CHECK(seen.size() > 60);
    }

    TEST_CASE("value() stays inside [0, 1)") {
        rmp::random::seed(7);
        for (int i = 0; i < 20000; i++) {
            const float v = rmp::random::value();
            REQUIRE(v >= 0.0f);
            REQUIRE(v < 1.0f);
        }
    }

    TEST_CASE("range(float) covers its ends without leaving them") {
        rmp::random::seed(7);
        float lo = 1e9f;
        float hi = -1e9f;
        for (int i = 0; i < 20000; i++) {
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
        for (int i = 0; i < 5000; i++) {
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
        for (int i = 0; i < 2000; i++) {
            REQUIRE_FALSE(rmp::random::chance(0.0f));
            REQUIRE(rmp::random::chance(1.0f));
        }
    }

    TEST_CASE("chance(p) lands near p") {
        rmp::random::seed(11);
        int hits = 0;
        for (int i = 0; i < 100000; i++) hits += rmp::random::chance(0.25f) ? 1 : 0;
        CHECK(hits > 24000);
        CHECK(hits < 26000);
    }

    TEST_CASE("in_circle stays inside, and does not bunch up in the middle") {
        // The sqrt in the implementation is what makes this true. Without it the
        // points crowd the centre, because a ring at radius r has area ~ r.
        rmp::random::seed(5);
        int outer_half = 0;
        const int samples = 20000;
        for (int i = 0; i < samples; i++) {
            const Vector2 p = rmp::random::in_circle(10.0f);
            const float d = std::sqrt(p.x * p.x + p.y * p.y);
            REQUIRE(d <= 10.0001f);
            // Half the AREA of a disc is outside r/sqrt(2) = 0.7071 r.
            if (d > 7.071f) outer_half++;
        }
        CHECK(outer_half > samples * 0.45);
        CHECK(outer_half < samples * 0.55);
    }

    TEST_CASE("direction() is a unit vector, and points every way") {
        rmp::random::seed(5);
        bool quadrant[4] = { false, false, false, false };
        for (int i = 0; i < 4000; i++) {
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
        for (int i = 0; i < 500; i++) {
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
    }

} // TEST_SUITE
