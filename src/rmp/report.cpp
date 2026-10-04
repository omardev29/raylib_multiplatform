// ---------------------------------------------------------------------------
// report_once: the one place a "say it once" diagnostic lives.
//
// Four modules had grown their own flag for this (input's list of warned
// names, the sprite's `warned` bit, the UI arena's, the UI font's), and three
// more warned every frame because nobody had added a fifth. A table keyed by
// call site is what all of them wanted.
// ---------------------------------------------------------------------------

#include "internal.h"

#include <raylib.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rmp::detail {

namespace {

struct Seen {
    const void *site;
    std::string key; // empty for the unkeyed form
};

// Linear, and that is fine: it holds distinct mistakes, and a game with more
// than a handful of distinct mistakes has a bigger problem than this lookup.
void abort_on_report() { std::abort(); }

// The mistakes said so far, and what [dev] strict does with the first one.
// The dot at every use says "file state".
struct {
    std::vector<Seen> seen;
    bool strict = false;
    StrictHandler on_strict = abort_on_report;
} reports;

bool already(const void *site, const char *key) {
    return std::ranges::any_of(
        reports.seen, [&](const Seen &s) { return s.site == site && s.key == key; });
}

void emit(const void *site, const char *key, const char *fmt, va_list args) {
    if (already(site, key)) return;
    reports.seen.push_back(Seen{ site, key });

    char line[1024];
    std::vsnprintf(line, sizeof line, fmt, args);
    TraceLog(reports.strict ? LOG_ERROR : LOG_WARNING, "%s", line);
    if (reports.strict) {
        TraceLog(LOG_ERROR,
                 "[dev] strict = true: the diagnostic above is fatal in a debug build. "
                 "Fix it, or set strict = false in raylib_multiplatform.toml.");
        reports.on_strict();
    }
}

} // namespace

// C-style variadic on purpose: this is printf into TraceLog, which is one
// itself, and the callers hand over raylib ints and C strings. A parameter
// pack here would only be a longer way to reach vsnprintf.
// NOLINTNEXTLINE(modernize-avoid-variadic-functions)
void report_once(const void *site, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(site, "", fmt, args);
    va_end(args);
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions)
void report_once_keyed(const void *site, const char *key, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(site, key != nullptr ? key : "", fmt, args);
    va_end(args);
}

void set_strict(bool on) { reports.strict = on; }
bool strict() { return reports.strict; }

StrictHandler set_strict_handler(StrictHandler handler) {
    StrictHandler previous = reports.on_strict;
    reports.on_strict = handler != nullptr ? handler : abort_on_report;
    return previous;
}

int report_count() { return static_cast<int>(reports.seen.size()); }
void reset_reports_for_tests() { reports.seen.clear(); }

} // namespace rmp::detail
