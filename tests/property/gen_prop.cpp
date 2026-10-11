// ---------------------------------------------------------------------------
// The generators of tests/property/gen.h, held to what they say they draw --
// and the shape of a property file, for the next one to copy:
//
//   - a TEST_SUITE named "property: <module>", so `-ts=` picks one module;
//   - rmp::test::check(description, [](generated arguments) { RC_ASSERT(...); })
//     from check.h, which counts in doctest's summary and prints RapidCheck's
//     shrunk counterexample and its RC_PARAMS="reproduce=..." when it fails;
//   - the hostile generators of gen.h, never RapidCheck's bare arbitrary<float>,
//     which almost never draws a NaN.
//
// Every property here was seen red: the commit that added this file says how.
// A counterexample a property finds becomes a named doctest case in the
// module's own *_test.cpp, so it is checked on every `rmp test unit` and not
// only when the seed happens to find it again.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/save_internal.h"

#include <doctest.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <vector>

static_assert(rmp::test::gen::SAVE_DEPTH == rmp::save::detail::MAX_DEPTH,
              "gen.h's SAVE_DEPTH is the depth rmp::save reads back");

namespace {

// The size RapidCheck calls nominal: the one a property's last cases run at,
// where a generator that grows with the size draws everything it can.
constexpr int SAMPLE_SIZE = 100;

// `count` values of a generator, from a fixed seed: what a generator draws,
// without a property around it -- so a test can ask whether it ever draws X.
template <class T> std::vector<T> sample(const rc::Gen<T> &gen, int count) {
    rc::Random random(20261010);
    std::vector<T> out;
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        out.push_back(gen(random.split(), SAMPLE_SIZE).value());
    return out;
}

template <class T, class P> bool any(const std::vector<T> &values, P predicate) {
    return std::ranges::any_of(values, predicate);
}

} // namespace

TEST_SUITE("property: gen") {
    TEST_CASE("hostile_float draws every value it promises, and ordinary ones") {
        const std::vector<float> drawn = sample(rmp::test::gen::hostile_float(), 4000);
        CHECK(any(drawn, [](float f) { return std::isnan(f); }));
        CHECK(any(drawn, [](float f) { return std::isinf(f) && f > 0; }));
        CHECK(any(drawn, [](float f) { return std::isinf(f) && f < 0; }));
        CHECK(any(drawn, [](float f) { return f == std::numeric_limits<float>::max(); }));
        CHECK(
            any(drawn, [](float f) { return f == -std::numeric_limits<float>::max(); }));
        CHECK(any(drawn, [](float f) { return std::fpclassify(f) == FP_SUBNORMAL; }));
        CHECK(any(drawn, [](float f) { return f == 0 && std::signbit(f); }));
        CHECK(any(drawn, [](float f) {
            return std::isfinite(f) && f > static_cast<float>(INT_MAX);
        }));
        CHECK(any(drawn, [](float f) {
            return std::isfinite(f) && f < static_cast<float>(INT_MIN);
        }));
        CHECK(any(drawn, [](float f) {
            return std::isfinite(f) && f != 0 && std::fabs(f) < 1e6F;
        }));
    }

    TEST_CASE("hostile_double draws NaN, both infinities and the largest doubles") {
        const std::vector<double> drawn = sample(rmp::test::gen::hostile_double(), 4000);
        CHECK(any(drawn, [](double d) { return std::isnan(d); }));
        CHECK(any(drawn, [](double d) { return std::isinf(d) && d > 0; }));
        CHECK(any(drawn, [](double d) { return std::isinf(d) && d < 0; }));
        CHECK(
            any(drawn, [](double d) { return d == std::numeric_limits<double>::max(); }));
        CHECK(any(drawn, [](double d) { return std::fpclassify(d) == FP_SUBNORMAL; }));
    }

    TEST_CASE("hostile_rect draws a negative size and a NaN corner") {
        const std::vector<Rectangle> drawn = sample(rmp::test::gen::hostile_rect(), 2000);
        CHECK(any(drawn, [](const Rectangle &r) { return r.width < 0; }));
        CHECK(any(drawn, [](const Rectangle &r) { return r.height < 0; }));
        CHECK(any(drawn, [](const Rectangle &r) { return std::isnan(r.x); }));
        CHECK(any(drawn, [](const Rectangle &r) { return std::isnan(r.y); }));
    }

    TEST_CASE(
        "hostile_string carries a NUL, multi-byte UTF-8 and bytes that are not UTF-8") {
        const std::vector<std::string> drawn =
            sample(rmp::test::gen::hostile_string(), 2000);
        CHECK(any(drawn, [](const std::string &s) {
            return s.find('\0') != std::string::npos;
        }));
        CHECK(any(drawn, [](const std::string &s) {
            return s.find("\xF0\x9F\x8E\xAE") != std::string::npos;
        }));
        CHECK(any(drawn, [](const std::string &s) {
            return s.find('\xFF') != std::string::npos;
        }));
        CHECK(any(drawn, [](const std::string &s) { return s.empty(); }));
    }

    TEST_CASE("value() reaches past what a save reads back, and no further") {
        const std::vector<rmp::Value> drawn = sample(rmp::test::gen::value(), 600);
        using Type = rmp::Value::Type;
        CHECK(any(drawn, [](const rmp::Value &v) {
            return rmp::test::gen::depth(v) > rmp::test::gen::SAVE_DEPTH;
        }));
        CHECK(any(drawn, [](const rmp::Value &v) {
            return rmp::test::gen::depth(v) == rmp::test::gen::SAVE_DEPTH;
        }));
        CHECK(std::ranges::all_of(drawn, [](const rmp::Value &v) {
            return rmp::test::gen::depth(v) <= rmp::test::gen::DEEPEST;
        }));
        for (const Type type : { Type::NONE, Type::BOOL, Type::NUMBER, Type::STRING,
                                 Type::LIST, Type::OBJECT }) {
            CAPTURE(static_cast<int>(type));
            CHECK(any(drawn, [type](const rmp::Value &v) { return v.type() == type; }));
        }
        // A NaN reads as the fallback, whichever it is: as_float() gives no NaN.
        CHECK(any(drawn, [](const rmp::Value &v) {
            return v.type() == Type::NUMBER && v.as_float(1) == 1 && v.as_float(2) == 2;
        }));
        CHECK(any(drawn, [](const rmp::Value &v) {
            return v.type() == Type::OBJECT && v.size() > 0 &&
                v.key(0).find('\0') != std::string_view::npos;
        }));
    }

    TEST_CASE("a drawn Value is the tree depth() measures, through a copy") {
        rmp::test::check(
            "a copy has the same type, size and depth, at most DEEPEST",
            [](const rmp::Value &v) {
                // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
                const rmp::Value copy(v); // the copy is what is held
                RC_ASSERT(copy.type() == v.type());
                RC_ASSERT(copy.size() == v.size());
                RC_ASSERT(rmp::test::gen::depth(copy) == rmp::test::gen::depth(v));
                RC_ASSERT(rmp::test::gen::depth(v) <= rmp::test::gen::DEEPEST);
            });
    }
}
