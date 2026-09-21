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
// the fourteen targets. Since rmp::Image exists, the unqualified name inside
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
