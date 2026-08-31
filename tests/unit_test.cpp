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

#include <rmp/assets.h>
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
        auto *slot = rmp::detail::adopt_named(ResourceKind::IMAGE, name, 0, &zeroed,
                                              sizeof(zeroed));
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
        // slot. Copy-and-swap makes it impossible, and this is the test that says
        // the idiom is still there.
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
        auto *at16 = rmp::detail::adopt_named(ResourceKind::FONT, "ui.ttf", 16, &zeroed,
                                              sizeof(zeroed));
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
