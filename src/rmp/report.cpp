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
std::vector<Seen> g_seen;
bool g_strict = false;

void abort_on_report() { std::abort(); }
StrictHandler g_on_strict = abort_on_report;

bool already(const void *site, const char *key) {
    return std::ranges::any_of(
        g_seen, [&](const Seen &s) { return s.site == site && s.key == key; });
}

void emit(const void *site, const char *key, const char *fmt, va_list args) {
    if (already(site, key)) return;
    g_seen.push_back(Seen{ site, key });

    char line[1024];
    std::vsnprintf(line, sizeof line, fmt, args);
    TraceLog(g_strict ? LOG_ERROR : LOG_WARNING, "%s", line);
    if (g_strict) {
        TraceLog(LOG_ERROR,
                 "[dev] strict = true: the diagnostic above is fatal in a debug build. "
                 "Fix it, or set strict = false in raylib_multiplatform.toml.");
        g_on_strict();
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

void set_strict(bool on) { g_strict = on; }
bool strict() { return g_strict; }

StrictHandler set_strict_handler(StrictHandler handler) {
    StrictHandler previous = g_on_strict;
    g_on_strict = handler != nullptr ? handler : abort_on_report;
    return previous;
}

int report_count() { return static_cast<int>(g_seen.size()); }
void reset_reports_for_tests() { g_seen.clear(); }

} // namespace rmp::detail
