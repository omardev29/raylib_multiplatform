// ---------------------------------------------------------------------------
// Properties of rmp::Text and rmp::lang (src/rmp/text/text.cpp): any table, any
// key, any arguments -- a text is always a text, nothing crashes, and a text
// whose every placeholder has its argument has none of them left.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/save/number_internal.h"
#include "../../src/rmp/text/text_internal.h"

#include <rmp/text.h>

#include <doctest.h>
#include <rapidcheck.h>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
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
            const std::string word =
                *rc::gen::container<std::string>(rc::gen::inRange('a', 'z'));
            const rmp::Text t("[{a}] [{b}] [{c}]",
                              { { "a", n }, { "b", x }, { "c", word } });
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
            const std::string code = *rc::gen::element<std::string>(
                "en", "es", "fr", "ru", "pl", "cs", "ja", "xx");
            rmp::lang::set(code);
            const std::string out = rmp::Text("A|B|C|D{n}", { { "n", n } }).str();
            const std::string tail = std::to_string(n);
            RC_ASSERT(out == "A" || out == "B" || out == "C" || out == "D" + tail);
        });
    }

    TEST_CASE("a number has to_chars' digits, without to_chars") {
        // shortest_number() stands in for the floating-point std::to_chars,
        // whose tables cost 126 KB of a static release: the same significant
        // digits, the fewest that read back, laid out as %g lays them.
        const auto digits = [](std::string text) {
            text = text.substr(0, text.find_first_of("eE"));
            std::erase_if(text, [](char c) { return c == '-' || c == '.'; });
            const std::size_t first = text.find_first_not_of('0');
            if (first == std::string::npos) return std::string("0");
            text = text.substr(first);
            return text.substr(0, text.find_last_not_of('0') + 1);
        };
        const auto general = [](auto n) {
            std::array<char, 64> buffer{};
            const auto done = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                            n, std::chars_format::general);
            return std::string(buffer.data(), done.ptr);
        };
        const auto same = [&](double x) {
            const std::string out = rmp::detail::shortest_number(x, false);
            if (!std::isfinite(x)) return out == general(x);
            return std::strtod(out.c_str(), nullptr) == x &&
                digits(out) == digits(general(x));
        };
        const auto same_float = [&](float x) {
            const std::string out = rmp::detail::shortest_number(x, true);
            if (!std::isfinite(x)) return out == general(x);
            return std::strtof(out.c_str(), nullptr) == x &&
                digits(out) == digits(general(x));
        };
        rmp::test::check("any double, any float", [&] {
            const double x = *rmp::test::gen::hostile_double();
            RC_ASSERT(same(x));
            RC_ASSERT(same_float(static_cast<float>(x)));
        });
        for (const double x :
             { 0.1, 1e8, 1e21, 123456789.0, 5e-324, -0.0, 2.5, 1.0 / 3 }) {
            CHECK(same(x));
            CHECK(same_float(static_cast<float>(x)));
        }
        // Laid out as C# lays a number out (Yarn's are C#'s): plainly from
        // 1e-4 to below 1e7 for a float and 1e15 for a double. 10 came out
        // as 1e+01 when the layout was %g's at the fewest digits.
        CHECK(rmp::detail::shortest_number(10, true) == "10");
        CHECK(rmp::detail::shortest_number(100, false) == "100");
        CHECK(rmp::detail::shortest_number(1234567, true) == "1234567");
        CHECK(rmp::detail::shortest_number(1e7, true) == "1e+07");
        CHECK(rmp::detail::shortest_number(123456789, true) == "1.2345679e+08");
        CHECK(rmp::detail::shortest_number(1378324, false) == "1378324");
        CHECK(rmp::detail::shortest_number(123456789012345.0, false) ==
              "123456789012345");
        CHECK(rmp::detail::shortest_number(1e15, false) == "1e+15");
        CHECK(rmp::detail::shortest_number(0.1, false) == "0.1");
        CHECK(rmp::detail::shortest_number(0.0001, false) == "0.0001");
        CHECK(rmp::detail::shortest_number(0.00001, false) == "1e-05");
        CHECK(rmp::detail::shortest_number(-2.5, false) == "-2.5");
        CHECK(rmp::detail::shortest_number(1e21, false) == "1e+21");
        CHECK(rmp::detail::shortest_number(0.1F, true) == "0.1");
        CHECK(rmp::detail::shortest_number(0, false) == "0");
    }

    TEST_CASE("a literal of any length is its own key") {
        const Clean clean;
        // NOLINTNEXTLINE(modernize-avoid-c-arrays): the literal path is the subject
        constexpr char LITERAL[] = "Play";
        const rmp::Text t(LITERAL);
        CHECK(t.translated());
        CHECK(t.key() == "Play");
        CHECK(t.str() == "Play");
    }
}
