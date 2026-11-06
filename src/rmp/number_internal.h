#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. Numbers as text, in the "C" numeric locale whatever the
// game set: read and written by the JSON readers (json_internal.h, which
// includes this), and written by rmp::Text and the Yarn machine. Nothing here
// names cJSON, so the host tool rmp_yarnc compiles it too.
// ---------------------------------------------------------------------------

#include <array>
#include <clocale>
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
// -- in the "C" numeric locale: the digits std::to_chars gives, the fewest
// that read back, laid out as printf's %g lays them (1378324, where to_chars'
// general writes 1.378324e+06; C# writes 1378324 too). Not to_chars itself: a
// floating-point to_chars brings libc++'s Ryu tables into every static link,
// and a number in a sentence cost the demo game 126 KB of its linux-x64
// release (zig's libc++, 1510904 bytes against 1384760).
inline std::string shortest_number(double n, bool single) {
    const CNumbers c_numbers;
    std::array<char, 40> text{};
    const int most = single ? 9 : 17;
    for (int digits = 1; digits <= most; ++digits) {
        std::snprintf(text.data(), text.size(), "%.*g", digits, n);
        const bool exact = single
            ? std::strtof(text.data(), nullptr) == static_cast<float>(n)
            : std::strtod(text.data(), nullptr) == n;
        if (exact) break;
    }
    return text.data();
}

} // namespace rmp::detail
