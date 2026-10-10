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

#include "../src/rmp/internal.h"

#include <rmp/app.h>

#include <string>

namespace {

// What happened, in order. The dot says "file state".
struct {
    std::string log;
} trace;

// Types that say when they are built and when they go. A distinct type per
// test, because rmp::global<T>() keys on the type and a shared one would carry
// state between test cases.
template <int N> struct Marker {
    Marker() { trace.log += "+"; }
    ~Marker() { trace.log += "-"; }
    int value = 0;
};

struct Named {
    explicit Named(const char *n = "?") : name(n) {}
    ~Named() { trace.log += name; }
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

// Reaches for another global from inside its own destructor, which is what
// anything running during the teardown does without meaning to.
struct Rebuilder {
    ~Rebuilder() {
        trace.log += "R";
        rmp::global<Marker<6>>().value = 1;
    }
};

struct Fixture {
    Fixture() {
        rmp::app::detail::shutdown_globals();
        trace.log.clear();
    }
    ~Fixture() {
        rmp::app::detail::shutdown_globals();
        trace.log.clear();
    }
};

} // namespace

TEST_SUITE("globals") {
    TEST_CASE("it is built once and the same one comes back") {
        const Fixture fix;
        rmp::global<Marker<1>>().value = 7;
        CHECK(trace.log == "+");

        // Same instance, not a copy: the whole point is that a scene change does
        // not lose what was written here.
        CHECK(rmp::global<Marker<1>>().value == 7);
        CHECK(trace.log == "+");
    }

    TEST_CASE("it is default-constructed, and only when first asked for") {
        const Fixture fix;
        CHECK(trace.log.empty()); // nothing built yet
        CHECK(rmp::global<Marker<2>>().value == 0);
        CHECK(trace.log == "+");
    }

    TEST_CASE("one instance per type, and the type is the whole key") {
        const Fixture fix;
        rmp::global<Marker<3>>().value = 3;
        rmp::global<Marker<4>>().value = 4;

        // No name, no registration, no lookup table: two types are two globals.
        CHECK(rmp::global<Marker<3>>().value == 3);
        CHECK(rmp::global<Marker<4>>().value == 4);
        CHECK(trace.log == "++");
    }

    TEST_CASE("shutdown destroys them, reverse of first use") {
        const Fixture fix;
        rmp::global<First>();
        rmp::global<Second>();
        rmp::global<Third>();
        trace.log.clear();

        rmp::app::detail::shutdown_globals();

        // Reverse, so a global that exists because another one needed it is still
        // there while that one is being taken apart.
        CHECK(trace.log == "321");
    }

    TEST_CASE("asking again after shutdown builds a fresh one") {
        const Fixture fix;
        rmp::global<Marker<5>>().value = 99;
        rmp::app::detail::shutdown_globals();
        trace.log.clear();

        // The pointer was nulled, not just deleted. Getting the old value back
        // here would be a read through a dangling pointer that happened to work.
        CHECK(rmp::global<Marker<5>>().value == 0);
        CHECK(trace.log == "+");
    }

    TEST_CASE("shutdown twice destroys nothing the second time") {
        const Fixture fix;
        rmp::global<First>();
        trace.log.clear();

        rmp::app::detail::shutdown_globals();
        CHECK(trace.log == "1");

        // The registry is cleared as it is drained. Without that, a second
        // shutdown — and there is one on every path that quits twice — would
        // delete an already-deleted object.
        rmp::app::detail::shutdown_globals();
        CHECK(trace.log == "1");
    }

    TEST_CASE("shutdown with nothing registered is a no-op") {
        const Fixture fix;
        rmp::app::detail::shutdown_globals();
        CHECK(trace.log.empty());
    }

    TEST_CASE("a global re-created during the shutdown is refused, and said so") {
        // Anything reached after shutdown_globals() has started -- the UI
        // closing, a destructor of something the resource table owns -- that
        // touches a global<T>() would rebuild it and push a destroyer into a
        // registry nobody drains again. The instance then outlives
        // CloseWindow(), which is the exact teardown order rmp/app.h documents
        // as having cost this project a segfault once.
        const Fixture fix;
        rmp::detail::reset_reports_for_tests();
        rmp::global<Rebuilder>();
        trace.log.clear();

        rmp::app::detail::shutdown_globals();
        // "R" is the destructor running, "+" the global it built on its way
        // out. Both are allowed; what is not allowed is the registration.
        CHECK(trace.log == "R+");
        CHECK(rmp::detail::report_count() == 1);

        // Nothing was added to the registry, so there is nothing left to drain.
        trace.log.clear();
        rmp::app::detail::shutdown_globals();
        CHECK(trace.log.empty());
    }

} // TEST_SUITE
