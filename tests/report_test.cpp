// ---------------------------------------------------------------------------
// report_once: one line per mistake, not one per frame -- and [dev] strict.
//
// Every warning the framework can raise about the game's own code goes through
// this, so the properties here are the properties of all of them: a site says
// its line once, a keyed site once per value, and strict turns the first one
// into a stop instead of a log line nobody reads.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/internal.h"

namespace {

int g_stops = 0;
void count_stop() { g_stops++; }

// A site that reports through the macro, the way every module does.
void warn_from_one_site(int n) { RMP_REPORT_ONCE("test: warning number %d", n); }
void warn_keyed(const char *key) { RMP_REPORT_ONCE_KEYED(key, "test: about %s", key); }

struct Fixture {
    rmp::detail::StrictHandler previous;
    Fixture() {
        rmp::detail::reset_reports_for_tests();
        rmp::detail::set_strict(false);
        g_stops = 0;
        previous = rmp::detail::set_strict_handler(count_stop);
    }
    ~Fixture() {
        rmp::detail::set_strict(false);
        rmp::detail::set_strict_handler(previous);
        rmp::detail::reset_reports_for_tests();
    }
};

} // namespace

TEST_SUITE("report_once") {
    TEST_CASE_FIXTURE(Fixture, "a site reports once however often it is hit") {
        for (int i = 0; i < 60; i++) warn_from_one_site(i);
        CHECK(rmp::detail::report_count() == 1);
    }

    TEST_CASE_FIXTURE(Fixture, "two sites are two lines") {
        warn_from_one_site(1);
        RMP_REPORT_ONCE("test: another place entirely");
        CHECK(rmp::detail::report_count() == 2);
    }

    TEST_CASE_FIXTURE(Fixture, "a keyed site reports once per key") {
        warn_keyed("jump");
        warn_keyed("jump");
        warn_keyed("fire");
        warn_keyed("fire");
        CHECK(rmp::detail::report_count() == 2);
        warn_keyed(nullptr); // a missing key is the empty key, and still one line
        warn_keyed(nullptr);
        CHECK(rmp::detail::report_count() == 3);
    }

    TEST_CASE_FIXTURE(Fixture, "the plain form is not confused with an empty key") {
        // Same address, unkeyed vs keyed with "": they share the entry, which is
        // the honest reading -- there is nothing to tell them apart by.
        static const char kSite = 0;
        rmp::detail::report_once(&kSite, "plain");
        rmp::detail::report_once_keyed(&kSite, "", "keyed empty");
        CHECK(rmp::detail::report_count() == 1);
        rmp::detail::report_once_keyed(&kSite, "x", "keyed x");
        CHECK(rmp::detail::report_count() == 2);
    }

    TEST_CASE_FIXTURE(Fixture, "reset forgets, so the same line can be watched again") {
        warn_from_one_site(1);
        rmp::detail::reset_reports_for_tests();
        CHECK(rmp::detail::report_count() == 0);
        warn_from_one_site(1);
        CHECK(rmp::detail::report_count() == 1);
    }

    TEST_CASE_FIXTURE(Fixture, "strict stops on the first report and only the first") {
        CHECK(!rmp::detail::strict());
        rmp::detail::set_strict(true);
        CHECK(rmp::detail::strict());
        warn_from_one_site(1);
        CHECK(g_stops == 1);
        warn_from_one_site(1); // already said: nothing, not a second stop
        CHECK(g_stops == 1);
        warn_keyed("fire"); // a new line is a new stop
        CHECK(g_stops == 2);
    }

    TEST_CASE_FIXTURE(Fixture, "strict off is a log line and nothing else") {
        warn_from_one_site(1);
        warn_keyed("fire");
        CHECK(g_stops == 0);
        CHECK(rmp::detail::report_count() == 2);
    }

    TEST_CASE_FIXTURE(Fixture, "a null handler is the default abort, not a null call") {
        rmp::detail::StrictHandler h = rmp::detail::set_strict_handler(nullptr);
        CHECK(h == count_stop);
        // Put the counter back before anything reports under strict.
        rmp::detail::StrictHandler d = rmp::detail::set_strict_handler(count_stop);
        CHECK(d != nullptr);
        CHECK(d != count_stop);
    }
}
