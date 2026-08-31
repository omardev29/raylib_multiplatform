// ===========================================================================
// rmp::global<T>() — the state that outlives a scene change.
//
// One instance per type, built on first use, destroyed by the framework in
// reverse order on the way out. There is nothing to test about the happy path
// that a reader would not assume, so most of what is here is the ORDER and the
// LIFETIME — the two things that were wrong in this framework once already,
// when the resource table was released after CloseWindow().
// ===========================================================================

#include <doctest.h>

#include <rmp/app.h>

#include <string>

namespace {

std::string g_log;

// Types that say when they are built and when they go. A distinct type per
// test, because rmp::global<T>() keys on the type and a shared one would carry
// state between test cases.
template <int N> struct Marker {
    Marker() { g_log += "+"; }
    ~Marker() { g_log += "-"; }
    int value = 0;
};

struct Named {
    explicit Named(const char *n = "?") : name(n) {}
    ~Named() { g_log += name; }
    const char *name;
};

// Named cannot report its own construction order — it has no idea when it was
// built relative to the others — so the tests that care about order build them
// deliberately and read the destruction log.
struct First : Named {
    First() : Named("1") {}
};
struct Second : Named {
    Second() : Named("2") {}
};
struct Third : Named {
    Third() : Named("3") {}
};

struct Fixture {
    Fixture() {
        rmp::app::detail::shutdown_globals();
        g_log.clear();
    }
    ~Fixture() {
        rmp::app::detail::shutdown_globals();
        g_log.clear();
    }
};

} // namespace

TEST_SUITE("globals") {
    TEST_CASE("it is built once and the same one comes back") {
        Fixture fix;
        rmp::global<Marker<1>>().value = 7;
        CHECK(g_log == "+");

        // Same instance, not a copy: the whole point is that a scene change does
        // not lose what was written here.
        CHECK(rmp::global<Marker<1>>().value == 7);
        CHECK(g_log == "+");
    }

    TEST_CASE("it is default-constructed, and only when first asked for") {
        Fixture fix;
        CHECK(g_log.empty()); // nothing built yet
        CHECK(rmp::global<Marker<2>>().value == 0);
        CHECK(g_log == "+");
    }

    TEST_CASE("one instance per type, and the type is the whole key") {
        Fixture fix;
        rmp::global<Marker<3>>().value = 3;
        rmp::global<Marker<4>>().value = 4;

        // No name, no registration, no lookup table: two types are two globals.
        CHECK(rmp::global<Marker<3>>().value == 3);
        CHECK(rmp::global<Marker<4>>().value == 4);
        CHECK(g_log == "++");
    }

    TEST_CASE("shutdown destroys them, reverse of first use") {
        Fixture fix;
        rmp::global<First>();
        rmp::global<Second>();
        rmp::global<Third>();
        g_log.clear();

        rmp::app::detail::shutdown_globals();

        // Reverse, so a global that exists because another one needed it is still
        // there while that one is being taken apart.
        CHECK(g_log == "321");
    }

    TEST_CASE("asking again after shutdown builds a fresh one") {
        Fixture fix;
        rmp::global<Marker<5>>().value = 99;
        rmp::app::detail::shutdown_globals();
        g_log.clear();

        // The pointer was nulled, not just deleted. Getting the old value back
        // here would be a read through a dangling pointer that happened to work.
        CHECK(rmp::global<Marker<5>>().value == 0);
        CHECK(g_log == "+");
    }

    TEST_CASE("shutdown twice destroys nothing the second time") {
        Fixture fix;
        rmp::global<First>();
        g_log.clear();

        rmp::app::detail::shutdown_globals();
        CHECK(g_log == "1");

        // The registry is cleared as it is drained. Without that, a second
        // shutdown — and there is one on every path that quits twice — would
        // delete an already-deleted object.
        rmp::app::detail::shutdown_globals();
        CHECK(g_log == "1");
    }

    TEST_CASE("shutdown with nothing registered is a no-op") {
        Fixture fix;
        rmp::app::detail::shutdown_globals();
        CHECK(g_log.empty());
    }

} // TEST_SUITE
