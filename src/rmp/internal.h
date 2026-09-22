#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. Deliberately NOT in include/: nothing
// here is part of the template's surface, and a header the user can reach is a
// header the user will end up depending on.
//
// The public surface is include/rmp/assets.h.
// ---------------------------------------------------------------------------

#include <raylib.h>

namespace rmp::assets::detail {

// --- pack.cpp --------------------------------------------------------------
// The .rres container: opening it, closing it, and pulling one entry out.

// Look for RESOURCES_PATH/resources.rres and open its central directory.
// Returns true if a usable pack was opened. Safe to call twice.
bool open_pack();

// Close it. Safe to call when nothing was open.
void close_pack();

// True between a successful open_pack() and close_pack().
bool pack_is_open();

// One entry, by its name in the central directory. Returns bytes owned by the
// caller (malloc'd, so UnloadFileData/RL_FREE frees them), or nullptr if the
// name is not packed, does not decrypt, or no pack is open. `Size` receives
// the byte count and is zeroed on failure.
unsigned char *pack_read(const char *name, int *size);

// An image, decoded. Images are the one resource rrespacker stores decoded
// (an IMGE chunk) rather than as the original file, so this cannot go through
// pack_read(). Returns a zeroed Image when the name is not packed or does not
// decode; the caller then falls back to the loose file.
// `::Image` and not `Image`, and this one cost a link error on exactly ONE of
// the seventeen targets. Since rmp::Image exists, the unqualified name inside
// rmp:: means the counted handle — so this declaration meant rmp::Image in
// assets.cpp (which includes rmp/assets.h) and ::Image in pack.cpp (which does
// not). Two translation units, two different functions, one missing symbol at
// link time on Windows ARM64.
//
// Inside rmp::, a raylib type we shadow is always written with ::.
::Image pack_read_image(const char *name);

// --- loader_hook.cpp -------------------------------------------------------
// Routing raylib's own LoadFileData/LoadFileText through the pack.

void install_loader_hook();
void remove_loader_hook();

// True when `path` points inside RESOURCES_PATH. Exposed here because it is
// the rule that decides what the hook is responsible for, and that rule is
// worth being able to read in one place.
bool in_resources_dir(const char *path);

// --- assets.cpp ------------------------------------------------------------
// Counters behind rmp::assets::requested_loads() / failed_loads(), which the CI boot
// gate reads. Defined in assets.cpp.
extern int g_requested_count;
extern int g_failed_count;

// Where loose files live: the directory that RESOURCES_PATH used to name at
// every call site. It is a runtime value now because the framework is compiled
// ONCE, into the `rmp` library, and linked into the game and into every
// example -- and an example that carries its own resources/ must not read the
// game's. The library starts with the RESOURCES_PATH it was compiled with, and
// the entry point (src/rmp/app.cpp, compiled into each executable with that
// executable's own RESOURCES_PATH) overrides it before anything is loaded.
// Always ends in a separator or is empty, exactly like the macro.
const char *resources_root();
void set_resources_root(const char *root);

} // namespace rmp::assets::detail

namespace rmp::detail {

// --- report.cpp ------------------------------------------------------------
// One diagnostic per call site, for the mistakes the framework notices every
// frame: an action nobody defined, a pop() with nothing under it, a tag that
// is not in the sheet. Sixty copies a second of the same warning is the same
// as no warning at all, and every module used to keep its own "said it
// already" flag. Now there is one table, one function, one rule.
//
// `site` identifies the diagnostic -- the RMP_REPORT_ONCE macro below passes
// the address of a static local, so each expansion is its own site. `key`
// makes it once per site AND value: "no action called \"jump\"" and "no action
// called \"fire\"" are two different mistakes and each one deserves its line.
//
// Printf-style, because the callers pass raylib types and this feeds TraceLog.
// Under [dev] strict = true (debug builds only, see set_strict()) the first
// report aborts, so a warning cannot scroll past in a log nobody reads.
// NOLINTNEXTLINE(modernize-avoid-variadic-functions)
void report_once(const void *site, const char *fmt, ...);
// NOLINTNEXTLINE(modernize-avoid-variadic-functions)
void report_once_keyed(const void *site, const char *key, const char *fmt, ...);

// [dev] strict. Off unless the entry point turns it on from APP_DEV_STRICT,
// which the unit tests never do -- they exercise the warnings on purpose.
void set_strict(bool on);
bool strict();

// What strict does on a report: abort(), unless a test has swapped it for a
// counter. Returns the previous handler so a test can put it back.
using StrictHandler = void (*)();
StrictHandler set_strict_handler(StrictHandler handler);

// Tests. How many distinct diagnostics have fired since the last reset, and
// the way to forget them so the same test can watch one fire again.
int report_count();
void reset_reports_for_tests();

} // namespace rmp::detail

// The site is the address of a static local, so each expansion of the macro is
// one line in the log and not one per frame. Both forms take a printf format.
#define RMP_REPORT_ONCE(...)                                        \
    do {                                                            \
        static const char rmp_report_site_ = 0;                     \
        ::rmp::detail::report_once(&rmp_report_site_, __VA_ARGS__); \
    } while (0)
#define RMP_REPORT_ONCE_KEYED(key, ...)                                          \
    do {                                                                         \
        static const char rmp_report_site_ = 0;                                  \
        ::rmp::detail::report_once_keyed(&rmp_report_site_, (key), __VA_ARGS__); \
    } while (0)
