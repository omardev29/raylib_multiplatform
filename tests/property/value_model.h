#pragma once
// ---------------------------------------------------------------------------
// What rmp::Value and rmp::save promise, written a second time in the plainest
// way there is, for the properties (value_prop.cpp, save_prop.cpp) and the
// fuzz targets (tests/fuzz/value_json.cpp and the save_* ones) to hold the
// real code to. Nothing here throws and nothing links RapidCheck: a fuzz
// target includes it too.
//
//   canonical(v)        a text two Values share exactly when they say the same
//                       thing: NONE members dropped, keys sorted, every NaN
//                       one NaN, 0 and -0 one zero -- what == has to agree with
//   exact(v)            a text that changes when anything a game can see does:
//                       key order and the bits of every number
//   nesting(v)          the containers on the deepest path: what a save allows
//                       64 of, as cJSON counts them
//   savable(v)          what rmp::save writes: nested at most 64, no NUL in a
//                       string or a key, every number finite
//   Path, read_at(),    a path of keys and indices as a game writes one, the
//   ref_at(), written() const read of it, the Ref of it, and what a write
//                       through it has to leave (nullopt: it cannot land)
//   read_everything(v)  every read Value and Ref offer, on every node, as one
//                       text -- the const walk and the Ref walk must agree
// ---------------------------------------------------------------------------

#include "../../src/rmp/save_internal.h"

#include <rmp/save.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rmp::test::model {

using Type = rmp::Value::Type;

inline double number_of(const rmp::Value &v) {
    return rmp::detail::ValueAccess::number(v);
}

inline bool is_container(const rmp::Value &v) {
    return v.type() == Type::LIST || v.type() == Type::OBJECT;
}

// The i-th child a game can see: a list's i-th element, an object's i-th
// member that holds something.
inline const rmp::Value &child(const rmp::Value &v, int i) {
    return v.type() == Type::OBJECT ? v[v.key(i)] : v[i];
}

inline std::string with_length(std::string_view text) {
    return std::to_string(text.size()) + ":" + std::string(text);
}

// The bits of a number, NaN folded into one, and zero into one when `zeros`
// is false -- 0 == -0 is IEEE's and stays ==, but the file keeps the sign.
inline std::string number_text(double n, bool zeros) {
    if (std::isnan(n)) return "nan";
    if (n == 0 && !zeros) return "0";
    std::array<char, 24> text{};
    const auto r = std::to_chars(text.data(), text.data() + text.size(),
                                 std::bit_cast<std::uint64_t>(n), 16);
    return { text.data(), r.ptr };
}

inline std::string canonical(const rmp::Value &v) {
    switch (v.type()) {
        case Type::NONE:
            return "n";
        case Type::BOOL:
            return v.as_bool() ? "t" : "f";
        case Type::NUMBER:
            return "#" + number_text(number_of(v), false);
        case Type::STRING:
            return "s" + with_length(v.as_string());
        case Type::LIST: {
            std::string out = "[";
            for (int i = 0; i < v.size(); ++i) out += canonical(v[i]) + ",";
            return out + "]";
        }
        case Type::OBJECT: {
            std::vector<std::pair<std::string, std::string>> members;
            members.reserve(static_cast<std::size_t>(v.size()));
            for (int i = 0; i < v.size(); ++i)
                members.emplace_back(std::string(v.key(i)), canonical(v[v.key(i)]));
            std::ranges::sort(members);
            std::string out = "{";
            for (const auto &[key, value] : members)
                out += with_length(key) + "=" + value + ",";
            return out + "}";
        }
    }
    return "?";
}

inline std::string exact(const rmp::Value &v) {
    switch (v.type()) {
        case Type::NUMBER:
            return "#" + number_text(number_of(v), true);
        case Type::LIST: {
            std::string out = "[";
            for (int i = 0; i < v.size(); ++i) out += exact(v[i]) + ",";
            return out + "]";
        }
        case Type::OBJECT: {
            std::string out = "{";
            for (int i = 0; i < v.size(); ++i)
                out += with_length(v.key(i)) + "=" + exact(v[v.key(i)]) + ",";
            return out + "}";
        }
        default:
            return canonical(v);
    }
}

inline int nesting(const rmp::Value &v) {
    if (!is_container(v)) return 0;
    int deepest = 0;
    for (int i = 0; i < v.size(); ++i) deepest = std::max(deepest, nesting(child(v, i)));
    return 1 + deepest;
}

inline bool holds_nul(const rmp::Value &v) {
    if (v.type() == Type::STRING)
        return v.as_string().find('\0') != std::string_view::npos;
    for (int i = 0; i < v.size(); ++i) {
        if (v.type() == Type::OBJECT && v.key(i).find('\0') != std::string_view::npos)
            return true;
        if (holds_nul(child(v, i))) return true;
    }
    return false;
}

inline bool all_finite(const rmp::Value &v) {
    if (v.type() == Type::NUMBER) return std::isfinite(number_of(v));
    for (int i = 0; i < v.size(); ++i)
        if (!all_finite(child(v, i))) return false;
    return true;
}

inline bool savable(const rmp::Value &v) {
    return nesting(v) <= rmp::save::detail::MAX_DEPTH && !holds_nul(v) && all_finite(v);
}

// ---- paths -----------------------------------------------------------------

struct Step {
    std::string key; // when is_key
    int index = 0; // when not
    bool is_key = true;
};
using Path = std::vector<Step>;

// How RapidCheck shows a path in a counterexample, found by its name.
// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
inline void showValue(const Step &step, std::ostream &os) {
    if (step.is_key) {
        os << "[\"";
        for (const char c : step.key) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte >= 0x7F) {
                constexpr std::string_view HEX = "0123456789abcdef";
                os << "\\x" << HEX[byte >> 4U] << HEX[byte & 0xFU];
            } else {
                os << c;
            }
        }
        os << "\"]";
    } else {
        os << "[" << step.index << "]";
    }
}

inline const rmp::Value &read_at(const rmp::Value &v, const Path &path,
                                 std::size_t from = 0) {
    // A reference into `v` -- or `v` itself, at the end of the path -- which a
    // caller holds no longer than `v`, as with Value::operator[].
    // NOLINTNEXTLINE(bugprone-return-const-ref-from-parameter)
    if (from == path.size()) return v;
    const Step &step = path[from];
    return read_at(step.is_key ? v[step.key] : v[step.index], path, from + 1);
}

// The Ref a game gets by writing v[a][b]... -- a path of at least one step.
// Built by constructing each deeper Ref, never by assigning one: a Ref
// assigned from a Ref copies the VALUE, and `ref = ref[k]` is a write.
inline rmp::Value::Ref deeper(const rmp::Value::Ref &ref, const Path &path,
                              std::size_t from) {
    if (from == path.size()) return ref;
    const Step &step = path[from];
    return deeper(step.is_key ? ref[step.key] : ref[step.index], path, from + 1);
}
inline rmp::Value::Ref ref_at(rmp::Value &v, const Path &path) {
    const Step &first = path.front();
    return deeper(first.is_key ? v[first.key] : v[first.index], path, 1);
}

// What `v[path] = x` has to leave, compared with ==: x where the path leads,
// every container on the way made of nothing when it was nothing, and the
// rest as it was -- or nullopt when the write cannot land: a key into
// anything but nothing or an object, an index into anything but nothing or a
// list, or an index outside 0..size().
inline std::optional<rmp::Value> written(const rmp::Value &at, const Path &path,
                                         const rmp::Value &x, std::size_t from = 0) {
    if (from == path.size()) return x;
    const Step &step = path[from];
    if (step.is_key) {
        if (at.type() != Type::NONE && at.type() != Type::OBJECT) return std::nullopt;
        std::optional<rmp::Value> inner = written(at[step.key], path, x, from + 1);
        if (!inner) return std::nullopt;
        rmp::Value out = rmp::Value::object();
        for (int i = 0; i < at.size(); ++i) {
            if (at.key(i) != step.key) out[at.key(i)] = at[at.key(i)];
        }
        out[step.key] = std::move(*inner);
        return out;
    }
    if (at.type() != Type::NONE && at.type() != Type::LIST) return std::nullopt;
    const int size = at.size();
    if (step.index < 0 || step.index > size) return std::nullopt;
    std::optional<rmp::Value> inner = written(at[step.index], path, x, from + 1);
    if (!inner) return std::nullopt;
    rmp::Value out = rmp::Value::list();
    for (int i = 0; i < size; ++i) out.push(i == step.index ? *inner : at[i]);
    if (step.index == size) out.push(std::move(*inner));
    return out;
}

// ---- every read -------------------------------------------------------------

// The reads a game makes of one node, as text, through anything that offers
// them: a const Value or a Ref.
template <class Node> std::string reads_of(const Node &node) {
    std::string out = std::to_string(static_cast<int>(node.type()));
    out += "|b" + std::to_string(static_cast<int>(node.as_bool(false))) +
        std::to_string(static_cast<int>(node.as_bool(true)));
    out += "|i" + std::to_string(node.as_int(INT_MIN)) + "," +
        std::to_string(node.as_int(7));
    const float f = node.as_float(-1.5F);
    out += "|f" +
        (std::isnan(f) ? std::string("nan")
                       : std::to_string(std::bit_cast<std::uint32_t>(f)));
    out += "|s" + with_length(node.as_string("\x01missing"));
    out += "|n" + std::to_string(node.size());
    out += "|k" + with_length(node.key(-1)) + with_length(node.key(node.size())) +
        with_length(node.key(INT_MAX)) + with_length(node.key(INT_MIN));
    out += "|c" + std::to_string(static_cast<int>(node.contains(""))) +
        std::to_string(static_cast<int>(node.contains("\x01missing")));
    return out;
}

// A glance at what lies outside a node -- nothing, every time -- cheaper than
// every read: through a Ref, each read walks the path from the root again,
// and a spine 66 deep made the full set cost seconds a property.
template <class Node> std::string glance(const Node &node) {
    return std::to_string(static_cast<int>(node.type())) + "," +
        std::to_string(node.as_int(7)) + "," + std::to_string(node.size()) + ";";
}

// Every node of `v`, through the const Value: its reads, then what lies
// outside it (a missing key, the indices around its ends), then each child.
inline std::string read_everything(const rmp::Value &v) {
    std::string out = reads_of(v);
    out += "<" + glance(v["\x01missing"]) + glance(v[-1]) + glance(v[v.size()]) +
        glance(v[INT_MIN]) + glance(v[INT_MAX]) + ">";
    for (int i = 0; i < v.size(); ++i) {
        if (v.type() == Type::OBJECT) {
            out += "{" + with_length(v.key(i)) +
                std::to_string(static_cast<int>(v.contains(v.key(i)))) + "}";
        }
        out += read_everything(child(v, i));
    }
    return out;
}

// The same walk through Refs from a non-const root -- which must read the
// same, and create nothing on the way.
inline std::string read_everything(const rmp::Value::Ref &ref) {
    std::string out = reads_of(ref);
    out += "<" + glance(ref["\x01missing"]) + glance(ref[-1]) + glance(ref[ref.size()]) +
        glance(ref[INT_MIN]) + glance(ref[INT_MAX]) + ">";
    for (int i = 0; i < ref.size(); ++i) {
        if (ref.type() == Type::OBJECT) {
            out += "{" + with_length(ref.key(i)) +
                std::to_string(static_cast<int>(ref.contains(ref.key(i)))) + "}";
            out += read_everything(ref[ref.key(i)]);
        } else {
            out += read_everything(ref[i]);
        }
    }
    return out;
}

// The Ref walk from a root: a Ref needs one step, so the root's own reads are
// its const ones and its children are walked through Refs.
inline std::string read_everything_through_refs(rmp::Value &root) {
    const rmp::Value &as_const = root;
    std::string out = reads_of(as_const);
    out += "<" + glance(root["\x01missing"]) + glance(root[-1]) +
        glance(root[as_const.size()]) + glance(root[INT_MIN]) + glance(root[INT_MAX]) +
        ">";
    for (int i = 0; i < as_const.size(); ++i) {
        if (as_const.type() == Type::OBJECT) {
            const std::string key(as_const.key(i));
            out += "{" + with_length(key) +
                std::to_string(static_cast<int>(as_const.contains(key))) + "}";
            out += read_everything(root[key]);
        } else {
            out += read_everything(root[i]);
        }
    }
    return out;
}

} // namespace rmp::test::model
