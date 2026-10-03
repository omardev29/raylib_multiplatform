#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. cJSON, owned, and read in the "C" numeric locale.
//
// Two files read JSON -- save.cpp its saves, ldtk.cpp the LDtk projects -- and
// both need the same two things. One copy here, because the second copy of a
// locale fix is the one that drifts: a German interface made saves unreadable
// once, and an LDtk reader without this would read every opacity and pivot of
// a level as 0 on that same machine.
// ---------------------------------------------------------------------------

#include <cJSON.h>

#include <clocale>
#include <cstring>
#include <memory>
#include <string>

namespace rmp::detail {

struct JsonDelete {
    void operator()(cJSON *node) const { cJSON_Delete(node); }
};
using Json = std::unique_ptr<cJSON, JsonDelete>;

struct JsonTextDelete {
    void operator()(char *text) const { cJSON_free(text); }
};
using JsonText = std::unique_ptr<char, JsonTextDelete>;

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
        const char *point = std::localeconv()->decimal_point;
        if (point != nullptr && std::strcmp(point, ".") == 0) return;
        const char *current = std::setlocale(LC_NUMERIC, nullptr);
        saved_ = current != nullptr ? current : "C";
        switched_ = std::setlocale(LC_NUMERIC, "C") != nullptr;
    }
    ~CNumbers() {
        if (switched_) std::setlocale(LC_NUMERIC, saved_.c_str());
    }
    CNumbers(const CNumbers &) = delete;
    CNumbers &operator=(const CNumbers &) = delete;

private:
    std::string saved_;
    bool switched_ = false;
};

} // namespace rmp::detail
