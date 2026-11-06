// ===========================================================================
// Yarn's values and how they print -- what the compiler (rmp_yarnc too) and
// the machine share.
// ===========================================================================

#include "yarn.h"

#include "../number_internal.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>

namespace rmp::yarn {

namespace {

constexpr float EPSILON_EQUAL = 1e-6f;

bool word_is(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

// --- values ------------------------------------------------------------------

std::string number_text(float n) {
    if (std::isnan(n)) return "NaN";
    if (std::isinf(n)) return n > 0 ? "∞" : "-∞";
    if (n == 0) return "0";
    std::string out = rmp::detail::shortest_number(n, true);
    // %g writes 1e+20 where C# writes 1E+20.
    if (const std::size_t e = out.find('e'); e != std::string::npos) out[e] = 'E';
    return out;
}

std::string Value::str() const {
    switch (kind) {
        case Kind::NUMBER:
            return number_text(number);
        case Kind::BOOL:
            return boolean ? "True" : "False";
        case Kind::STRING:
            return text;
    }
    return {};
}

float Value::as_number() const {
    switch (kind) {
        case Kind::NUMBER:
            return number;
        case Kind::BOOL:
            return boolean ? 1.0f : 0.0f;
        case Kind::STRING:
            return std::strtof(text.c_str(), nullptr);
    }
    return 0;
}

bool Value::as_bool() const {
    switch (kind) {
        case Kind::NUMBER:
            return number != 0;
        case Kind::BOOL:
            return boolean;
        case Kind::STRING:
            return word_is(text, "true");
    }
    return false;
}

bool operator==(const Value &a, const Value &b) {
    if (a.kind == Kind::STRING || b.kind == Kind::STRING) return a.str() == b.str();
    if (a.kind == Kind::BOOL && b.kind == Kind::BOOL) return a.boolean == b.boolean;
    return std::fabs(a.as_number() - b.as_number()) < EPSILON_EQUAL;
}

std::string expand(std::string_view text, std::span<const std::string> substitutions) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '{') {
            const std::size_t close = text.find('}', i + 1);
            if (close != std::string_view::npos) {
                int index = -1;
                const auto inner = text.substr(i + 1, close - i - 1);
                const auto [p, ec] =
                    std::from_chars(inner.data(), inner.data() + inner.size(), index);
                if (ec == std::errc{} && p == inner.data() + inner.size() && index >= 0 &&
                    static_cast<std::size_t>(index) < substitutions.size()) {
                    out += substitutions[static_cast<std::size_t>(index)];
                    i = close + 1;
                    continue;
                }
            }
        }
        out += text[i++];
    }
    return out;
}

} // namespace rmp::yarn
