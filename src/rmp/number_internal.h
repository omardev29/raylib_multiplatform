#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. Numbers as text, in the "C" numeric locale whatever the
// game set: read and written by the JSON readers (json_internal.h, which
// includes this), and written by rmp::Text and the Yarn machine. Nothing here
// names cJSON, so the host tool rmp_yarnc compiles it too.
// ---------------------------------------------------------------------------

#include <array>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace rmp::detail {

// Numbers go through printf and strtod, and both follow the C library's
// LC_NUMERIC: a game that sets a German locale for its interface would write
// 0,5 -- which is not JSON -- and fail to read 0.5. For the length of one
// conversion, and only when the current locale does not already use '.', the
// numeric locale is "C". (cJSON's own ENABLE_LOCALES was the first attempt; it
// takes one byte of the decimal point, and Pashto's is two.) setlocale() is
// process-wide: JSON read while another thread formats numbers can disturb it
// for those microseconds, which is the price of not touching cJSON.
class CNumbers {
public:
    CNumbers() {
        if (std::localeconv()->decimal_point != nullptr &&
            std::string_view(std::localeconv()->decimal_point) == ".")
            return;
        _saved = std::setlocale(LC_NUMERIC, nullptr) != nullptr
            ? std::setlocale(LC_NUMERIC, nullptr)
            : "C";
        _switched = std::setlocale(LC_NUMERIC, "C") != nullptr;
    }
    ~CNumbers() {
        if (_switched) std::setlocale(LC_NUMERIC, _saved.c_str());
    }
    CNumbers(const CNumbers &) = delete;
    CNumbers &operator=(const CNumbers &) = delete;

private:
    std::string _saved;
    bool _switched = false;
};

// The shortest text that reads back as exactly `n` -- as a float when `single`
// -- in the "C" numeric locale: the fewest significant digits that read back,
// the digits std::to_chars gives, laid out as C# lays them out (Yarn's
// numbers are C#'s): plainly while the exponent is from -4 to 6 for a float
// and to 14 for a double -- 10, 0.0001, 1234567 -- and with an exponent past
// that -- 1e-05, 1.2345679e+08. Not to_chars itself: a floating-point
// to_chars brings libc++'s Ryu tables into every static link, and a number in
// a sentence cost the demo game 126 KB of its linux-x64 release (zig's
// libc++, 1510904 bytes against 1384760).
inline std::string shortest_number(double n, bool single) {
    const CNumbers c_numbers;
    std::array<char, 48> text{};
    if (!std::isfinite(n)) {
        std::snprintf(text.data(), text.size(), "%g", n);
        return text.data();
    }
    const int most = single ? 9 : 17;
    int digits = most;
    for (int d = 1; d <= most; ++d) {
        std::snprintf(text.data(), text.size(), "%.*e", d - 1, n);
        const bool exact = single
            ? std::strtof(text.data(), nullptr) == static_cast<float>(n)
            : std::strtod(text.data(), nullptr) == n;
        if (exact) {
            digits = d;
            break;
        }
    }
    // The exponent of the shortest form, which rounding can have moved.
    std::snprintf(text.data(), text.size(), "%.*e", digits - 1, n);
    std::string scientific = text.data();
    int exponent = 0;
    if (const std::size_t e = scientific.find('e'); e != std::string::npos) {
        // e+08 or e-05: from_chars takes the minus and not the plus. (An
        // integer from_chars, which brings no tables.)
        const std::size_t from = scientific[e + 1] == '+' ? e + 2 : e + 1;
        (void)std::from_chars(scientific.data() + from,
                              scientific.data() + scientific.size(), exponent);
    }
    const int plain_below = single ? 7 : 15;
    if (exponent >= -4 && exponent < plain_below) {
        const int decimals = digits - 1 - exponent;
        std::snprintf(text.data(), text.size(), "%.*f", decimals > 0 ? decimals : 0, n);
        return text.data();
    }
    return scientific; // the fewest digits never end in a 0: nothing to trim
}

} // namespace rmp::detail
