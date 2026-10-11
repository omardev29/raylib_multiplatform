#pragma once
// ---------------------------------------------------------------------------
// The generators every property test shares: the values a game can hand the
// framework by mistake, and rmp::Value trees that reach past what a save can
// hold. RapidCheck's own arbitrary<float> almost never draws a NaN, and its
// strings almost never hold a NUL -- exactly the inputs the framework's
// resilience rules are about (no UB on any input, no NaN kept in state, no
// save written that does not read back).
//
//   rmp::test::gen::hostile_float()     NaN, both infinities, the largest and
//   rmp::test::gen::hostile_double()    smallest normals, denormals, -0, values
//                                       past int's range -- or an ordinary one
//   rmp::test::gen::hostile_rect()      four hostile floats, negative sizes too
//   rmp::test::gen::hostile_string()    NUL inside, multi-byte UTF-8, bytes
//                                       that are not UTF-8, or plain text
//   rmp::test::gen::value()             a Value tree, sometimes deeper than
//   rc::Arbitrary<rmp::Value>           rmp::save reads back (MAX_DEPTH);
//                                       it shrinks to a child or to one
//                                       element fewer, so a counterexample is
//                                       the smallest Value that still fails
//
// tests/property/gen_prop.cpp holds each generator to what it says here: a
// generator that quietly stopped drawing NaN would turn every hostile-float
// property green by not looking.
//
// Test code only: RapidCheck throws (RC_ASSERT), which no file under src/rmp/
// may do.
// ---------------------------------------------------------------------------

#include "../../src/rmp/save_internal.h"

#include <rmp/save.h>

#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rmp::test::gen {

// How deep a save may nest: rmp::save::detail::MAX_DEPTH. save_internal.h is
// also where a Value's number is read exactly (rmp::detail::ValueAccess), for
// showValue() below.
inline constexpr int SAVE_DEPTH = rmp::save::detail::MAX_DEPTH;
// How deep value() goes when it reaches past that on purpose.
inline constexpr int DEEPEST = SAVE_DEPTH + 2;
// The size RapidCheck calls nominal (its own constant has a name this
// repository's naming rules refuse): the size a property's last cases run at,
// where a generator that scales with the size draws its whole range.
inline constexpr int NOMINAL_SIZE = 100;

namespace detail {

template <class F> rc::Gen<F> hostile_floating() {
    using Limits = std::numeric_limits<F>;
    // Past int's range both ways, and far past it: a cast of any of these to
    // int is undefined behaviour, which UBSan's float-cast-overflow reports.
    const F past_int = static_cast<F>(2.2e9);
    const F huge = static_cast<F>(1e30);
    return rc::gen::weightedOneOf<F>({
        { 1,
          rc::gen::element<F>(Limits::quiet_NaN(), -Limits::quiet_NaN(),
                              Limits::infinity(), -Limits::infinity(), Limits::max(),
                              -Limits::max(), Limits::min(), -Limits::min(),
                              Limits::denorm_min(), -Limits::denorm_min(), F(0), -F(0),
                              past_int, -past_int, huge, -huge) },
        { 1, rc::gen::arbitrary<F>() },
    });
}

} // namespace detail

inline rc::Gen<float> hostile_float() { return detail::hostile_floating<float>(); }
inline rc::Gen<double> hostile_double() { return detail::hostile_floating<double>(); }

inline rc::Gen<Rectangle> hostile_rect() {
    return rc::gen::build<Rectangle>(rc::gen::set(&Rectangle::x, hostile_float()),
                                     rc::gen::set(&Rectangle::y, hostile_float()),
                                     rc::gen::set(&Rectangle::width, hostile_float()),
                                     rc::gen::set(&Rectangle::height, hostile_float()));
}

inline rc::Gen<std::string> hostile_string() {
    return rc::gen::weightedOneOf<std::string>({
        { 2, rc::gen::string<std::string>() },
        // A NUL inside: what a C string would stop at and a std::string keeps.
        { 1, rc::gen::exec([] {
              auto text = *rc::gen::string<std::string>();
              const auto at = *rc::gen::inRange<std::size_t>(0, text.size() + 1);
              text.insert(at, 1, '\0');
              return text;
          }) },
        // Multi-byte UTF-8 (two, three and four bytes), and bytes no UTF-8 is
        // made of: a lone continuation, a truncated lead, 0xFF.
        { 1, rc::gen::exec([] {
              const auto pieces = *rc::gen::container<std::vector<std::string>>(
                  rc::gen::element<std::string>("a", "\xC3\xA9", "\xE2\x82\xAC",
                                                "\xF0\x9F\x8E\xAE", "\x80", "\xC3",
                                                "\xFF", "\\", "\"", "/", "\n"));
              std::string text;
              for (const std::string &piece : pieces) text += piece;
              return text;
          }) },
    });
}

namespace detail {

// One of the three leaves a Value has besides a container.
inline rc::Gen<rmp::Value> leaf() {
    return rc::gen::oneOf(
        rc::gen::just(rmp::Value()),
        rc::gen::map(rc::gen::arbitrary<bool>(), [](bool b) { return rmp::Value(b); }),
        rc::gen::map(hostile_double(), [](double n) { return rmp::Value(n); }),
        rc::gen::map(hostile_string(),
                     [](const std::string &s) { return rmp::Value(s); }));
}

// A tree at most `levels` deep below its root, at most four children a node.
inline rc::Gen<rmp::Value> tree(int levels) {
    if (levels <= 0) return leaf();
    return rc::gen::exec([levels] {
        const int kind = *rc::gen::inRange(0, 3);
        if (kind == 0) return *leaf();
        const int count = *rc::gen::inRange(0, 5);
        rmp::Value node = kind == 1 ? rmp::Value::list() : rmp::Value::object();
        for (int i = 0; i < count; ++i) {
            rmp::Value child = *tree(levels - 1);
            if (kind == 1) {
                node.push(std::move(child));
            } else {
                const std::string key = *hostile_string();
                node[key] = std::move(child);
            }
        }
        return node;
    });
}

// One path straight down, `levels` containers deep, with a leaf at the bottom:
// what crosses SAVE_DEPTH, which a bushy random tree almost never reaches.
inline rc::Gen<rmp::Value> spine() {
    return rc::gen::exec([] {
        // At the nominal size: inRange scales its range with the run's size,
        // and the early cases of a property would never leave SAVE_DEPTH - 2.
        const int levels =
            SAVE_DEPTH - 2 + *rc::gen::resize(NOMINAL_SIZE, rc::gen::inRange(0, 5));
        rmp::Value inner = *leaf();
        for (int level = 0; level < levels; ++level) {
            rmp::Value outer =
                *rc::gen::arbitrary<bool>() ? rmp::Value::list() : rmp::Value::object();
            if (outer.type() == rmp::Value::Type::LIST) {
                outer.push(std::move(inner));
            } else {
                outer["k"] = std::move(inner);
            }
            inner = std::move(outer);
        }
        return inner;
    });
}

// What a Value shrinks to: each of its children -- a spine unwraps one level
// at a time, down to its leaf -- then itself with one element fewer. Leaves
// stay as they were drawn: a NaN, an infinity, a NUL inside a string are what
// a counterexample is about, not noise in it.
//
// value() shrinks with this alone. RapidCheck's own shrinking of the trees
// above could not do it: weightedOneOf() never trades a spine for a tree, and
// a NaN found at the bottom of a spine was shown 66 levels deep.
inline rc::Seq<rmp::Value> smaller(const rmp::Value &v) {
    const bool object = v.type() == rmp::Value::Type::OBJECT;
    const int count = v.size();
    std::vector<rmp::Value> out;
    for (int i = 0; i < count; ++i) out.push_back(object ? v[v.key(i)] : v[i]);
    for (int skip = 0; skip < count; ++skip) {
        rmp::Value fewer = object ? rmp::Value::object() : rmp::Value::list();
        for (int i = 0; i < count; ++i) {
            if (i == skip) continue;
            if (object) {
                fewer[v.key(i)] = v[v.key(i)];
            } else {
                fewer.push(v[i]);
            }
        }
        out.push_back(std::move(fewer));
    }
    return rc::seq::fromContainer(std::move(out));
}

} // namespace detail

// A Value: two times in three a small tree, else a spine from two levels short
// of what a save reads back to two past it -- often enough that a property of
// 200 cases sees every depth of the spine more than once.
inline rc::Gen<rmp::Value> value() {
    const rc::Gen<rmp::Value> drawn = rc::gen::weightedOneOf<rmp::Value>(
        { { 2, detail::tree(4) }, { 1, detail::spine() } });
    return [drawn](const rc::Random &random, int size) {
        return rc::shrinkable::shrinkRecur(drawn(random, size).value(), &detail::smaller);
    };
}

// How deep a Value nests: 0 for a leaf or an empty container, one more for
// each container on the way down -- the count rmp::save checks against
// MAX_DEPTH.
inline int depth(const rmp::Value &v) {
    int deepest = 0;
    for (int i = 0; i < v.size(); ++i) {
        const rmp::Value &child =
            v.type() == rmp::Value::Type::OBJECT ? v[v.key(i)] : v[i];
        deepest = std::max(deepest, 1 + depth(child));
    }
    return deepest;
}

} // namespace rmp::test::gen

namespace rmp {

// How RapidCheck prints a Value in a counterexample: JSON-like, NaN and the
// infinities by name, a string's bytes escaped where they are not printable.
// RapidCheck finds it by its name, in the Value's namespace.
// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
inline void showValue(const Value &v, std::ostream &os) {
    switch (v.type()) {
        case Value::Type::NONE:
            os << "null";
            return;
        case Value::Type::BOOL:
            os << (v.as_bool() ? "true" : "false");
            return;
        case Value::Type::NUMBER: {
            // The double as stored, in the fewest digits that read back as
            // exactly it: as_float() would show DBL_MAX as inf and a denormal
            // as 0, and a counterexample that is not the value is a lie.
            const double n = rmp::detail::ValueAccess::number(v);
            if (std::isnan(n)) {
                os << "nan";
            } else if (std::isinf(n)) {
                os << (n > 0 ? "inf" : "-inf");
            } else {
                std::array<char, 32> text{};
                const std::to_chars_result written =
                    std::to_chars(text.data(), text.data() + text.size(), n);
                os << std::string_view(
                    text.data(), static_cast<std::size_t>(written.ptr - text.data()));
            }
            return;
        }
        case Value::Type::STRING: {
            os << '"';
            for (const char c : v.as_string()) {
                const auto byte = static_cast<unsigned char>(c);
                if (byte < 0x20 || byte >= 0x7F || c == '"' || c == '\\') {
                    constexpr std::string_view HEX = "0123456789abcdef";
                    os << "\\x" << HEX[byte >> 4U] << HEX[byte & 0xFU];
                } else {
                    os << c;
                }
            }
            os << '"';
            return;
        }
        case Value::Type::LIST:
        case Value::Type::OBJECT: {
            const bool object = v.type() == Value::Type::OBJECT;
            os << (object ? '{' : '[');
            for (int i = 0; i < v.size(); ++i) {
                if (i > 0) os << ", ";
                if (object) {
                    showValue(Value(v.key(i)), os);
                    os << ": ";
                    showValue(v[v.key(i)], os);
                } else {
                    showValue(v[i], os);
                }
            }
            os << (object ? '}' : ']');
            return;
        }
    }
}

} // namespace rmp

namespace rc {

// So a property can take an rmp::Value argument and get value()'s trees.
template <> struct Arbitrary<rmp::Value> {
    static Gen<rmp::Value> arbitrary() { return rmp::test::gen::value(); }
};

} // namespace rc
