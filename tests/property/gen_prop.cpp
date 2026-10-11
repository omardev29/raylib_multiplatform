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

#include <doctest.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// Where a generator that grows with the size draws everything it can.
constexpr int SAMPLE_SIZE = rmp::test::gen::NOMINAL_SIZE;

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

// Hears nothing: the properties shrunk() checks are made to fail, and
// RapidCheck's own listener would print their reproduce strings when the
// program ends, as if a property of the suite had failed.
class Quiet : public rc::detail::TestListener {
public:
    void
    onTestCaseFinished(const rc::detail::CaseDescription & /*description*/) override {}
    void onShrinkTried(const rc::detail::CaseDescription & /*shrink*/,
                       bool /*accepted*/) override {}
    void onTestFinished(const rc::detail::TestMetadata & /*metadata*/,
                        const rc::detail::TestResult & /*result*/) override {}
};

// The counterexample a property that fails is shown with, after shrinking --
// "(it held)" when it did not fail -- from a fixed seed.
template <class Testable> std::string shrunk(Testable &&property) {
    rc::detail::TestParams params;
    params.seed = 20261010;
    params.maxSuccess = 200;
    Quiet quiet;
    const rc::detail::TestResult result = rc::detail::checkProperty(
        rc::detail::toProperty(std::forward<Testable>(property)),
        rc::detail::TestMetadata{}, params, quiet);
    rc::detail::FailureResult failure;
    if (!result.match(failure) || failure.counterExample.empty()) return "(it held)";
    return failure.counterExample.front().second;
}

bool holds_nan(const rmp::Value &v) {
    if (v.type() == rmp::Value::Type::NUMBER)
        return std::isnan(rmp::detail::ValueAccess::number(v));
    const bool object = v.type() == rmp::Value::Type::OBJECT;
    for (int i = 0; i < v.size(); ++i)
        if (holds_nan(object ? v[v.key(i)] : v[i])) return true;
    return false;
}

std::string shown(const rmp::Value &v) {
    std::ostringstream out;
    rmp::showValue(v, out);
    return out.str();
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

    TEST_CASE("a Value is shown as it is: every number exactly, NaN and the infinities "
              "by name") {
        CHECK(shown(rmp::Value(std::numeric_limits<double>::max())) ==
              "1.7976931348623157e+308");
        CHECK(shown(rmp::Value(std::numeric_limits<double>::denorm_min())) == "5e-324");
        // 2^53 + 2: a double holds it exactly, a float would show 9.0072e+15.
        CHECK(shown(rmp::Value(9007199254740994.0)) == "9007199254740994");
        CHECK(shown(rmp::Value(0.1)) == "0.1");
        CHECK(shown(rmp::Value(-0.0)) == "-0");
        CHECK(shown(rmp::Value(std::numeric_limits<double>::infinity())) == "inf");
        CHECK(shown(rmp::Value(-std::numeric_limits<double>::infinity())) == "-inf");
        CHECK(shown(rmp::Value(std::numeric_limits<double>::quiet_NaN())) == "nan");
        rmp::Value tree = rmp::Value::object();
        tree[std::string("a\0b", 3)][0] = "\xFF\"";
        CHECK(shown(tree) == "{\"a\\x00b\": [\"\\xff\\x22\"]}");
    }

    TEST_CASE("a counterexample shrinks to the smallest Value that still fails") {
        // A NaN anywhere: whatever tree or spine failed first, the NaN alone.
        CHECK(shrunk([](const rmp::Value &v) { RC_ASSERT(!holds_nan(v)); }) == "(nan)");
        // Past what a save reads back: the shallowest spine that is.
        const std::string deep = shrunk([](const rmp::Value &v) {
            RC_ASSERT(rmp::test::gen::depth(v) <= rmp::test::gen::SAVE_DEPTH);
        });
        CHECK(std::ranges::count(deep, '[') + std::ranges::count(deep, '{') ==
              rmp::test::gen::SAVE_DEPTH + 1);
        // And a property that holds says so.
        CHECK(shrunk([](const rmp::Value &v) {
                  RC_ASSERT(rmp::test::gen::depth(v) >= 0);
              }) == "(it held)");
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
