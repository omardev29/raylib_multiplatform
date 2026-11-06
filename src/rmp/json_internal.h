#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. cJSON, owned, and read in the "C" numeric locale
// (CNumbers, in number_internal.h).
//
// Two files read JSON -- save.cpp its saves, ldtk.cpp the LDtk projects -- and
// both need the same two things. One copy of each, because the second copy of
// a locale fix is the one that drifts: a German interface made saves unreadable
// once, and an LDtk reader without this would read every opacity and pivot of
// a level as 0 on that same machine.
// ---------------------------------------------------------------------------

#include "number_internal.h" // CNumbers

#include <cJSON.h>

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

} // namespace rmp::detail
