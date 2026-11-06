// ---------------------------------------------------------------------------
// Properties of rmp::Text and rmp::lang (src/rmp/text.cpp): any table, any
// key, any arguments -- a text is always a text, nothing crashes, and a text
// whose every placeholder has its argument has none of them left.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/text_internal.h"

#include <rmp/text.h>

#include <doctest.h>
#include <rapidcheck.h>

#include <string>
#include <vector>

// covers: rmp::Text::Text [template]
// covers: rmp::Text::Text(std::string_view, const TextOptions &)
// covers: rmp::Text::Text(std::string_view, std::initializer_list<TextArg>, const TextOptions &)
// covers: rmp::Text::args() const
// covers: rmp::Text::context() const
// covers: rmp::Text::id() const
// covers: rmp::Text::key() const
// covers: rmp::Text::str() const
// covers: rmp::Text::translated() const
// covers: rmp::TextArg::TextArg [template]
// covers: rmp::TextArg::TextArg(std::string_view, std::string_view)
// covers: rmp::TextArg::name() const
// covers: rmp::TextArg::number() const
// covers: rmp::TextArg::numeric() const
// covers: rmp::TextArg::text() const
// covers: rmp::lang::available()
// covers: rmp::lang::current()
// covers: rmp::lang::name(std::string_view)
// covers: rmp::lang::set(std::string_view)
// covers: rmp::lang::translate(std::string_view, std::string_view)

namespace {

namespace d = rmp::lang::detail;

struct Clean {
    Clean() { reset(); }
    ~Clean() { reset(); }
    Clean(const Clean &) = delete;
    Clean &operator=(const Clean &) = delete;
    static void reset() {
        d::reset_for_tests();
        d::set_persist(false);
        d::set_system_locale_for_tests("en");
    }
};

} // namespace

TEST_SUITE("property: text") {
    TEST_CASE("any table, any key: a lookup is a text and never a crash") {
        rmp::test::check("lookups of any table in any language", [] {
            const Clean clean;
            const std::string table = *rmp::test::gen::hostile_string();
            const std::string key = *rmp::test::gen::hostile_string();
            const std::string code = *rmp::test::gen::hostile_string();
            d::load_tables({ { {}, table }, { "ru", table } });
            rmp::lang::set(code);
            RC_ASSERT(!rmp::lang::current().empty());
            for (const std::string &c : rmp::lang::available()) (void)rmp::lang::name(c);
            (void)rmp::lang::translate(key, key);
            const rmp::Text runtime{ key };
            RC_ASSERT(!runtime.translated());
            RC_ASSERT(runtime.str() == key); // as it is, always
            const rmp::Text with(key, { .context = "c" });
            RC_ASSERT(with.translated());
            RC_ASSERT(with.context() == "c");
            RC_ASSERT(with.key() == key);
            (void)with.str();
            (void)with.id();
        });
    }

    TEST_CASE("every placeholder given is filled") {
        rmp::test::check("{a} and {b} are gone when both are given", [] {
            const Clean clean;
            const int n = *rc::gen::arbitrary<int>();
            const double x = *rmp::test::gen::hostile_double();
            const std::string word = *rc::gen::container<std::string>(rc::gen::inRange('a', 'z'));
            const rmp::Text t("[{a}] [{b}] [{c}]", { { "a", n }, { "b", x }, { "c", word } });
            const std::string out = t.str();
            RC_ASSERT(out.find("{a}") == std::string::npos);
            RC_ASSERT(out.find("{b}") == std::string::npos);
            RC_ASSERT(out.find("{c}") == std::string::npos);
            RC_ASSERT(t.args().size() == 3U);
            RC_ASSERT(t.args()[0].name() == "a");
            RC_ASSERT(t.args()[0].numeric());
            RC_ASSERT(t.args()[0].number() == static_cast<double>(n));
            RC_ASSERT(t.args()[2].text() == word);
            const rmp::TextArg plain("w", word);
            RC_ASSERT(!plain.numeric());
            RC_ASSERT(plain.text() == word);
            // The id does not move with the values: focus survives a score.
            RC_ASSERT(t.id() == rmp::Text("[{a}] [{b}] [{c}]", { { "a", 0 } }).id());
        });
    }

    TEST_CASE("a plural form is one of the forms written") {
        rmp::test::check("whatever the number and the language", [] {
            const Clean clean;
            const int n = *rc::gen::arbitrary<int>();
            const std::string code =
                *rc::gen::element<std::string>("en", "es", "fr", "ru", "pl", "cs", "ja", "xx");
            rmp::lang::set(code);
            const std::string out = rmp::Text("A|B|C|D{n}", { { "n", n } }).str();
            const std::string tail = std::to_string(n);
            RC_ASSERT(out == "A" || out == "B" || out == "C" || out == "D" + tail);
        });
    }

    TEST_CASE("a literal of any length is its own key") {
        const Clean clean;
        constexpr char LITERAL[] = "Play"; // NOLINT(modernize-avoid-c-arrays): the literal path is the subject
        const rmp::Text t(LITERAL);
        CHECK(t.translated());
        CHECK(t.key() == "Play");
        CHECK(t.str() == "Play");
    }
}
