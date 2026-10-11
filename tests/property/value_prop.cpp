// ---------------------------------------------------------------------------
// rmp::Value, held to what rmp/save.h says of it, over every tree
// tests/property/gen.h draws -- NaN and the infinities in its numbers, NUL
// and broken UTF-8 in its strings and keys, members that hold nothing,
// spines deeper than a save reads back:
//
//   - == is a true equality: reflexive with a NaN anywhere (Omar, 2026-10-10),
//     symmetric, and yes exactly when the two say the same thing -- which
//     tests/property/value_model.h writes down a second way, canonical();
//   - a read never changes anything, never says anything, and reads the same
//     through a const Value and through a Ref;
//   - as_int, as_float, as_bool and as_string are total: the value, or the
//     fallback, never undefined behaviour and never a float that is not
//     finite out of a number that is;
//   - key(i), size(), contains() and erase() agree with each other;
//   - a write through a path lands where it reads back and changes nothing
//     else, or changes nothing at all and is said once -- the same for push(),
//     for a Ref assigned from a Ref, and for a Value assigned into itself.
//
// A counterexample one of these finds becomes a named case in
// tests/save_test.cpp. Each was seen red: the commits that added them name
// the defect or the hand mutation.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"
#include "value_model.h"

#include "../../src/rmp/internal.h"

#include <doctest.h>
#include <raylib.h>

#include <algorithm>
#include <bit>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// covers: rmp::Value::Value [template]
// covers: rmp::Value::list()
// covers: rmp::Value::object()
// covers: rmp::Value::type() const
// covers: rmp::Value::version() const
// covers: rmp::operator==(const Value &, const Value &)
// covers: rmp::Value::as_bool(bool) const
// covers: rmp::Value::as_int(int) const
// covers: rmp::Value::as_float(float) const
// covers: rmp::Value::as_string(std::string_view) const
// covers: rmp::Value::size() const
// covers: rmp::Value::key(int) const
// covers: rmp::Value::contains(std::string_view) const
// covers: rmp::Value::erase(std::string_view)
// covers: rmp::Value::push(Value)
// covers: rmp::Value::operator[](int)
// covers: rmp::Value::operator[](int) const
// covers: rmp::Value::operator[](std::string_view)
// covers: rmp::Value::operator[](std::string_view) const
// covers: rmp::Value::Ref::get() const
// covers: rmp::Value::Ref::operator const rmp::Value &() const
// covers: rmp::Value::Ref::as_bool(bool) const
// covers: rmp::Value::Ref::as_int(int) const
// covers: rmp::Value::Ref::as_float(float) const
// covers: rmp::Value::Ref::as_string(std::string_view) const
// covers: rmp::Value::Ref::type() const
// covers: rmp::Value::Ref::size() const
// covers: rmp::Value::Ref::key(int) const
// covers: rmp::Value::Ref::contains(std::string_view) const
// covers: rmp::Value::Ref::operator[](int) const
// covers: rmp::Value::Ref::operator[](std::string_view) const
// covers: rmp::Value::Ref::operator=(Value)
// covers: rmp::Value::Ref::operator=(const Ref &)
// covers: rmp::Value::Ref::operator=(Ref &&)
// covers: rmp::Value::Ref::push(Value)
// covers: rmp::Value::Ref::erase(std::string_view)

namespace {

using rmp::Value;
using Type = Value::Type;
namespace model = rmp::test::model;

// A write that cannot land is said once, through RMP_REPORT_ONCE, which logs
// it: hundreds of cases would print hundreds of lines. Quiet for the length
// of one property, raylib's default after it.
struct QuietLog {
    QuietLog() { SetTraceLogLevel(LOG_ERROR); }
    ~QuietLog() { SetTraceLogLevel(LOG_INFO); }
    QuietLog(const QuietLog &) = delete;
    QuietLog &operator=(const QuietLog &) = delete;
};

// One of 0..count-1, every one as likely at any size of the run: RapidCheck's
// inRange() narrows its range with the size, and the first half of a run
// asked only for the first member -- an erase() that looked at one key only
// survived it (Mull, value.cpp's erase loop counting down).
int pick(int count) {
    return *rc::gen::resize(rmp::test::gen::NOMINAL_SIZE, rc::gen::inRange(0, count));
}

// A key a game might use, one that is already there when there is one, or
// any string at all -- a NUL inside, bytes that are not UTF-8.
std::string draw_key(const Value &at) {
    if (at.type() == Type::OBJECT && at.size() > 0 && pick(3) > 0)
        return std::string(at.key(pick(at.size())));
    return *rc::gen::weightedOneOf<std::string>(
        { { 2, rc::gen::element<std::string>("k", "a", "") },
          { 1, rmp::test::gen::hostile_string() } });
}

// An index that is there, one around the ends of a list, or anywhere an int
// can be.
int draw_index(const Value &at) {
    if (at.type() == Type::LIST && at.size() > 0 && pick(2) > 0) return pick(at.size());
    return *rc::gen::weightedOneOf<int>(
        { { 3,
            rc::gen::resize(rmp::test::gen::NOMINAL_SIZE,
                            rc::gen::inRange(-1, at.size() + 2)) },
          { 1, rc::gen::element(INT_MIN, INT_MIN + 1, -2, INT_MAX - 1, INT_MAX) } });
}

// A path down `v` as a game would write one, `least` steps or more: a key of
// an object or an index of a list that is there, mostly, and now and then a
// step to nowhere -- through a number, past a list's end, a key nobody wrote.
model::Path draw_path(const Value &v, int least) {
    const int steps = least + pick(6 - least);
    model::Path path;
    for (int i = 0; i < steps; ++i) {
        const Value &at = model::read_at(v, path);
        model::Step step;
        step.is_key = at.type() == Type::OBJECT ? pick(4) > 0
            : at.type() == Type::LIST           ? pick(4) == 0
                                                : pick(2) == 0;
        if (step.is_key) {
            step.key = draw_key(at);
        } else {
            step.index = draw_index(at);
        }
        path.push_back(std::move(step));
    }
    return path;
}

// A tree made to be walked: objects of several keys -- one now and then
// holding nothing -- and lists of them, `levels` deep, its leaves gen.h's.
// gen::value() draws at RapidCheck's size, and in the first half of a run its
// trees are mostly leaves and objects of one key.
Value walkable(int levels) {
    const int kind = levels == 0 ? 0 : pick(4);
    if (kind == 0)
        return *rc::gen::resize(rmp::test::gen::NOMINAL_SIZE,
                                rmp::test::gen::detail::leaf());
    const bool list = kind == 1;
    Value node = list ? Value::list() : Value::object();
    const int count = 1 + pick(5);
    for (int i = 0; i < count; ++i) {
        Value child = pick(6) == 0 ? Value() : walkable(levels - 1);
        if (list) {
            node.push(std::move(child));
        } else {
            const std::string key =
                pick(4) > 0 ? "k" + std::to_string(i) : *rmp::test::gen::hostile_string();
            node[key] = std::move(child);
        }
    }
    return node;
}

// A Value of either kind: gen.h's -- spines past what a save holds, NaN,
// NUL -- or one made to be walked.
rc::Gen<Value> any_value() {
    return rc::gen::weightedOneOf<Value>(
        { { 1, rc::gen::arbitrary<Value>() },
          { 1, rc::gen::exec([] { return walkable(3); }) } });
}

// A Value close to `v`: rebuilt member by member -- an object's keys in
// another order, sometimes with a member that holds nothing added -- and,
// when `alter` says so, now and then one thing changed: a leaf for a
// neighbour of it (a NaN of the other sign, the other zero, the next double,
// one character more), a member or an element dropped, a NONE appended.
// Rebuilt only, it says the same thing as `v` -- a NaN in it included --
// and == has to say so; altered, the canonical texts say which it is.
Value perturbed(const Value &v, bool alter) {
    const int change = alter ? *rc::gen::inRange(0, 8) : 8;
    switch (v.type()) {
        case Type::NONE:
            return change == 0 ? Value(0) : Value();
        case Type::BOOL:
            return change == 0 ? Value(!v.as_bool()) : v;
        case Type::NUMBER: {
            const double n = model::number_of(v);
            if (change > 1) return v;
            return *rc::gen::element(Value(-n), Value(std::nextafter(n, 0.0)), Value(0.0),
                                     Value(-0.0),
                                     Value(std::numeric_limits<double>::quiet_NaN()),
                                     Value(-std::numeric_limits<double>::quiet_NaN()));
        }
        case Type::STRING:
            return change == 0 ? Value(std::string(v.as_string()) + "x") : v;
        case Type::LIST: {
            Value out = Value::list();
            const int drop = change == 0 && v.size() > 0 ? v.size() - 1 : -1;
            for (int i = 0; i < v.size(); ++i)
                if (i != drop) out.push(perturbed(v[i], alter));
            if (change == 1) out.push(Value());
            return out;
        }
        case Type::OBJECT: {
            Value out = Value::object();
            const int count = v.size();
            const int start = count > 0 ? *rc::gen::inRange(0, count) : 0;
            const int drop = change == 0 && count > 0 ? start : -1;
            for (int n = 0; n < count; ++n) {
                const int i = (start + n) % count;
                if (i == drop) continue;
                out[v.key(i)] = perturbed(v[v.key(i)], alter);
            }
            // A member that holds nothing, under a key that is not there: no
            // change at all. Under one that is, it clears that member.
            if (change <= 2 || *rc::gen::arbitrary<bool>()) {
                const std::string key = *rmp::test::gen::hostile_string();
                if (alter || !v.contains(key)) out[key] = Value();
            }
            return out;
        }
    }
    return v;
}

// The double as_float has to give: the nearest float, clamped to float's
// range -- as_int clamps to int's -- so a finite number never reads as an
// infinity; an infinity reads as itself.
float expected_float(double n) {
    if (std::isinf(n)) return static_cast<float>(n);
    const double clamped = std::fmax(-static_cast<double>(FLT_MAX),
                                     std::fmin(static_cast<double>(FLT_MAX), n));
    return static_cast<float>(clamped);
}

bool same_float(float a, float b) {
    return std::isnan(a)
        ? std::isnan(b)
        : std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}

// One of every type, around a hostile number and a hostile string -- each
// as likely at any size: through inRange() the first cases of a run were all
// NONE, and an as_float() that clamped an infinity to FLT_MAX survived the
// run (review of P1, hand mutation).
Value any_type(double n, const std::string &s, bool b) {
    const int kind = pick(6);
    switch (kind) {
        case 0:
            return {};
        case 1:
            return { b };
        case 2:
            return { n };
        case 3:
            return { s };
        case 4:
            return Value::list();
        default:
            return Value::object();
    }
}

} // namespace

TEST_SUITE("property: value") {
    TEST_CASE("== is a true equality") {
        rmp::test::check(
            "a Value equals itself and every copy of it, NaN anywhere included", [] {
                const Value v = *any_value();
                RC_ASSERT(v == v);
                // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
                const Value copy = v; // a copy is what is compared
                RC_ASSERT(copy == v);
                RC_ASSERT(v == copy);
                Value root;
                root["x"] = v;
                RC_ASSERT(root["x"] == v);
                RC_ASSERT(v == root["x"]);
            });
        rmp::test::check(
            "== says yes exactly when the canonical texts agree, both ways round", [] {
                const Value v = *any_value();
                const bool alter = *rc::gen::arbitrary<bool>();
                const Value near = perturbed(v, alter);
                if (!alter) RC_ASSERT(model::canonical(v) == model::canonical(near));
                const bool same = model::canonical(v) == model::canonical(near);
                RC_ASSERT((v == near) == same);
                RC_ASSERT((near == v) == same);
                const Value further = perturbed(near, *rc::gen::arbitrary<bool>());
                if (v == near && near == further) RC_ASSERT(v == further);
            });
        rmp::test::check(
            "two numbers are == exactly when they are one number: every NaN one, the "
            "two zeros one",
            [] {
                // Leaf against leaf, at the edges: in a tree, a zero met the
                // other zero too rarely for 200 cases, and an == that told
                // them apart survived the tree property (review of P1, hand
                // mutation).
                const double a = *rmp::test::gen::hostile_double();
                const double b = *rc::gen::weightedOneOf<double>(
                    { { 1, rc::gen::just(a) },
                      { 2, rc::gen::just(-a) },
                      { 1, rc::gen::just(std::nextafter(a, 0.0)) },
                      { 2, rmp::test::gen::hostile_double() } });
                const bool same =
                    model::canonical(Value(a)) == model::canonical(Value(b));
                RC_ASSERT((Value(a) == Value(b)) == same);
                RC_ASSERT((Value(b) == Value(a)) == same);
                Value in_a;
                in_a["n"].push(a);
                Value in_b;
                in_b["n"].push(b);
                RC_ASSERT((in_a == in_b) == same);
            });
    }

    TEST_CASE("a read changes nothing, says nothing, and reads the same through a Ref") {
        rmp::test::check("every read of every node, const and through Refs, agrees and "
                         "leaves no trace",
                         [] {
                             Value v = *any_value();
                             const std::string before = model::exact(v);
                             rmp::detail::reset_reports_for_tests();
                             const Value &as_const = v;
                             const std::string through_const =
                                 model::read_everything(as_const);
                             const std::string through_refs =
                                 model::read_everything_through_refs(v);
                             RC_ASSERT(through_const == through_refs);
                             RC_ASSERT(model::exact(v) == before);
                             RC_ASSERT(rmp::detail::report_count() == 0);
                         });
        rmp::test::check("a Ref down any path names the Value the const path reads, and "
                         "creates nothing",
                         [] {
                             Value v = *any_value();
                             const std::string before = model::exact(v);
                             const model::Path path = draw_path(v, 1);
                             rmp::detail::reset_reports_for_tests();
                             const Value::Ref ref = model::ref_at(v, path);
                             const Value &read = model::read_at(v, path);
                             const Value &named = ref; // operator const Value &
                             RC_ASSERT(&named == &read);
                             RC_ASSERT(&ref.get() == &read);
                             RC_ASSERT(ref.type() == read.type());
                             RC_ASSERT(ref.size() == read.size());
                             RC_ASSERT(ref.as_bool(true) == read.as_bool(true));
                             RC_ASSERT(ref.as_int(-3) == read.as_int(-3));
                             RC_ASSERT(
                                 same_float(ref.as_float(2.5F), read.as_float(2.5F)));
                             RC_ASSERT(ref.as_string("x") == read.as_string("x"));
                             RC_ASSERT(ref.key(0) == read.key(0));
                             RC_ASSERT(ref.contains("k") == read.contains("k"));
                             const Value &as_const = v;
                             const Value &by_key = as_const["k"];
                             const Value &by_index = as_const[0];
                             RC_ASSERT(&static_cast<const Value &>(v["k"]) == &by_key);
                             RC_ASSERT(&static_cast<const Value &>(v[0]) == &by_index);
                             RC_ASSERT(model::exact(v) == before);
                             RC_ASSERT(rmp::detail::report_count() == 0);
                         });
    }

    TEST_CASE("the typed reads are total: the value, or the fallback") {
        rmp::test::check("as_int is the number truncated towards zero and clamped to "
                         "int, else the fallback",
                         [](int fallback, bool b) {
                             const double n = *rmp::test::gen::hostile_double();
                             // The number itself every case, and any type.
                             for (const Value &v : { Value(n), any_type(n, "7", b) }) {
                                 const int got = v.as_int(fallback);
                                 if (v.type() != Type::NUMBER || std::isnan(n)) {
                                     RC_ASSERT(got == fallback);
                                 } else if (n >= static_cast<double>(INT_MAX)) {
                                     RC_ASSERT(got == INT_MAX);
                                 } else if (n <= static_cast<double>(INT_MIN)) {
                                     RC_ASSERT(got == INT_MIN);
                                 } else {
                                     RC_ASSERT(static_cast<double>(got) == std::trunc(n));
                                 }
                             }
                         });
        rmp::test::check("as_float is the nearest float clamped to float's range, the "
                         "fallback for NaN",
                         [](bool b) {
                             const double n = *rmp::test::gen::hostile_double();
                             const float fallback = *rmp::test::gen::hostile_float();
                             // The number itself every case, and any type.
                             for (const Value &v : { Value(n), any_type(n, "7", b) }) {
                                 const float got = v.as_float(fallback);
                                 if (v.type() != Type::NUMBER || std::isnan(n)) {
                                     RC_ASSERT(same_float(got, fallback));
                                 } else {
                                     RC_ASSERT(same_float(got, expected_float(n)));
                                     if (std::isfinite(n)) RC_ASSERT(std::isfinite(got));
                                 }
                             }
                         });
        rmp::test::check(
            "as_bool and as_string give what is stored, the fallback otherwise",
            [](bool b, bool fallback) {
                const std::string s = *rmp::test::gen::hostile_string();
                const std::string other = *rmp::test::gen::hostile_string();
                const Value v = any_type(1, s, b);
                RC_ASSERT(v.as_bool(fallback) == (v.type() == Type::BOOL ? b : fallback));
                RC_ASSERT(v.as_string(other) ==
                          (v.type() == Type::STRING ? std::string_view(s)
                                                    : std::string_view(other)));
            });
    }

    TEST_CASE("key, size, contains and erase agree with each other") {
        rmp::test::check(
            "key(i) names each member that holds something once, in order, and no more",
            [] {
                const Value v = *any_value();
                const int size = v.size();
                RC_ASSERT(size >= 0);
                RC_ASSERT(v.key(-1).empty());
                RC_ASSERT(v.key(size).empty());
                RC_ASSERT(v.key(INT_MAX).empty());
                RC_ASSERT(v.key(INT_MIN).empty());
                if (v.type() != Type::OBJECT) {
                    if (v.type() != Type::LIST) RC_ASSERT(size == 0);
                    RC_ASSERT(v.key(0).empty());
                    RC_ASSERT(!v.contains(""));
                    RC_ASSERT(!v.contains(*rmp::test::gen::hostile_string()));
                    return;
                }
                std::vector<std::string_view> keys;
                for (int i = 0; i < size; ++i) {
                    const std::string_view key = v.key(i);
                    RC_ASSERT(std::ranges::find(keys, key) == keys.end());
                    RC_ASSERT(v.contains(key));
                    RC_ASSERT(v[key].type() != Type::NONE);
                    keys.push_back(key);
                }
                const std::string probe = *rmp::test::gen::hostile_string();
                RC_ASSERT(v.contains(probe) ==
                          (std::ranges::find(keys, probe) != keys.end()));
                RC_ASSERT(v.contains(probe) == (v[probe].type() != Type::NONE));
            });
        rmp::test::check(
            "erase takes exactly that member away and says whether it held something",
            [] {
                const Value drawn = *any_value();
                const std::string key = draw_key(drawn);
                // Sometimes the key holds nothing -- assigned NONE, a member
                // that is not there -- which erase() takes away and says false.
                Value v = drawn;
                if (v.type() == Type::OBJECT && *rc::gen::arbitrary<bool>())
                    v[key] = Value();
                rmp::detail::reset_reports_for_tests();
                Value after = v;
                const bool had = v.contains(key);
                RC_ASSERT(after.erase(key) == had);
                RC_ASSERT(!after.contains(key));
                if (v.type() == Type::OBJECT) {
                    RC_ASSERT(after.size() == v.size() - (had ? 1 : 0));
                    const std::optional<Value> cleared =
                        model::written(v, { model::Step{ .key = key } }, Value());
                    RC_ASSERT(cleared.has_value());
                    RC_ASSERT(model::canonical(after) == model::canonical(*cleared));
                } else {
                    RC_ASSERT(model::exact(after) == model::exact(v));
                }
                RC_ASSERT(rmp::detail::report_count() == 0);
            });
        rmp::test::check("erase through a Ref erases from the Value the path names, and "
                         "creates nothing",
                         [] {
                             const Value v = *any_value();
                             Value after = v;
                             const model::Path path = draw_path(v, 1);
                             const Value &target = model::read_at(v, path);
                             const std::string key = draw_key(target);
                             const bool had = target.contains(key);
                             rmp::detail::reset_reports_for_tests();
                             RC_ASSERT(model::ref_at(after, path).erase(key) == had);
                             if (had) {
                                 Value smaller = target;
                                 smaller.erase(key);
                                 const std::optional<Value> expected =
                                     model::written(v, path, smaller);
                                 RC_ASSERT(expected.has_value());
                                 RC_ASSERT(after == *expected);
                             } else {
                                 RC_ASSERT(model::exact(after) == model::exact(v));
                             }
                             RC_ASSERT(rmp::detail::report_count() == 0);
                         });
    }

    TEST_CASE("a write lands where it reads back, or changes nothing and is said once") {
        const QuietLog quiet;
        rmp::test::check(
            "v[path] = x leaves what the model says, or v as it was and one report",
            [](const Value &x) {
                const Value v = *any_value();
                const model::Path path = draw_path(v, 1);
                const std::optional<Value> expected = model::written(v, path, x);
                rmp::detail::reset_reports_for_tests();
                Value after = v;
                model::ref_at(after, path) = x;
                if (expected) {
                    RC_ASSERT(after == *expected);
                    RC_ASSERT(model::read_at(after, path) == x);
                    RC_ASSERT(rmp::detail::report_count() == 0);
                } else {
                    RC_ASSERT(model::exact(after) == model::exact(v));
                    RC_ASSERT(rmp::detail::report_count() == 1);
                }
            });
        rmp::test::check("a Value assigned into its own subtree is copied first, never "
                         "read half-written",
                         [] {
                             const Value v = *any_value();
                             const model::Path path = draw_path(v, 1);
                             const std::optional<Value> expected =
                                 model::written(v, path, v);
                             rmp::detail::reset_reports_for_tests();
                             Value after = v;
                             model::ref_at(after, path) = after;
                             if (expected) {
                                 RC_ASSERT(after == *expected);
                             } else {
                                 RC_ASSERT(model::exact(after) == model::exact(v));
                             }
                             RC_ASSERT(rmp::detail::report_count() == (expected ? 0 : 1));
                         });
        rmp::test::check("a Ref assigned from a Ref copies the value, never the path, "
                         "lvalue or temporary",
                         [] {
                             const Value v = *any_value();
                             const model::Path to = draw_path(v, 1);
                             const model::Path from = draw_path(v, 1);
                             const std::optional<Value> expected =
                                 model::written(v, to, model::read_at(v, from));
                             const bool temporary = *rc::gen::arbitrary<bool>();
                             rmp::detail::reset_reports_for_tests();
                             Value after = v;
                             if (temporary) {
                                 model::ref_at(after, to) = model::ref_at(after, from);
                             } else {
                                 const Value::Ref source = model::ref_at(after, from);
                                 Value::Ref target = model::ref_at(after, to);
                                 target = source;
                             }
                             if (expected) {
                                 RC_ASSERT(after == *expected);
                             } else {
                                 RC_ASSERT(model::exact(after) == model::exact(v));
                             }
                             RC_ASSERT(rmp::detail::report_count() == (expected ? 0 : 1));
                         });
        rmp::test::check("a Ref assigned to itself changes nothing, wherever it points",
                         [] {
                             const Value v = *any_value();
                             const model::Path path = draw_path(v, 1);
                             rmp::detail::reset_reports_for_tests();
                             Value after = v;
                             Value::Ref ref = model::ref_at(after, path);
                             const Value::Ref &same = ref;
                             ref = same;
                             RC_ASSERT(model::exact(after) == model::exact(v));
                             RC_ASSERT(rmp::detail::report_count() == 0);
                         });
        rmp::test::check("push appends to a list and makes a list of nothing; anything "
                         "else is said once",
                         [](const Value &x) {
                             const Value v = *any_value();
                             const model::Path path = draw_path(v, 0);
                             const Value &target = model::read_at(v, path);
                             std::optional<Value> pushed;
                             if (target.type() == Type::NONE ||
                                 target.type() == Type::LIST) {
                                 Value grown = Value::list();
                                 for (int i = 0; i < target.size(); ++i)
                                     grown.push(target[i]);
                                 grown.push(x);
                                 pushed = model::written(v, path, grown);
                             }
                             rmp::detail::reset_reports_for_tests();
                             Value after = v;
                             if (path.empty()) {
                                 after.push(x);
                             } else {
                                 model::ref_at(after, path).push(x);
                             }
                             if (pushed) {
                                 RC_ASSERT(after == *pushed);
                                 RC_ASSERT(rmp::detail::report_count() == 0);
                             } else {
                                 RC_ASSERT(model::exact(after) == model::exact(v));
                                 RC_ASSERT(rmp::detail::report_count() == 1);
                             }
                         });
    }

    TEST_CASE("a Value made in code holds exactly what it was made from") {
        rmp::test::check(
            "each constructor makes the type it says, holding exactly what it was given",
            [](int i, std::int64_t wide, std::uint64_t big, bool b) {
                const float f = *rmp::test::gen::hostile_float();
                const double d = *rmp::test::gen::hostile_double();
                const std::string s = *rmp::test::gen::hostile_string();
                const auto holds = [](const Value &v, double n) {
                    return v.type() == Type::NUMBER &&
                        model::number_text(model::number_of(v), true) ==
                        model::number_text(n, true);
                };
                RC_ASSERT(holds(Value(i), static_cast<double>(i)));
                RC_ASSERT(holds(Value(wide), static_cast<double>(wide)));
                RC_ASSERT(holds(Value(big), static_cast<double>(big)));
                RC_ASSERT(holds(Value(f), static_cast<double>(f)));
                RC_ASSERT(holds(Value(d), d));
                RC_ASSERT(Value(b).type() == Type::BOOL);
                RC_ASSERT(Value(b).as_bool(!b) == b);
                RC_ASSERT(Value(s).type() == Type::STRING);
                RC_ASSERT(Value(s).as_string() == s);
                RC_ASSERT(Value(std::string_view(s)).as_string() == s);
                RC_ASSERT(Value().type() == Type::NONE);
                RC_ASSERT(Value::list().type() == Type::LIST);
                RC_ASSERT(Value::object().type() == Type::OBJECT);
                RC_ASSERT(Value::list().size() == 0);
                RC_ASSERT(Value::object().size() == 0);
                for (const Value &made : { Value(), Value(b), Value(d), Value(s),
                                           Value::list(), Value::object() }) {
                    RC_ASSERT(made.version() == RMP_SAVE_VERSION);
                }
            });
        rmp::test::check("a copy keeps the version it was read at", [](const Value &v) {
            const int version = *rc::gen::inRange(1, INT_MAX);
            Value read = v;
            rmp::detail::ValueAccess::set_version(read, version);
            // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
            const Value copy = read; // a copy is what is compared
            RC_ASSERT(read.version() == version);
            RC_ASSERT(copy.version() == version);
            RC_ASSERT(v.version() == RMP_SAVE_VERSION);
            RC_ASSERT(copy == v); // == does not compare versions
        });
    }
}
