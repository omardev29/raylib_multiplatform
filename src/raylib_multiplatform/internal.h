#pragma once
// ---------------------------------------------------------------------------
// Private to src/raylib_multiplatform/. Deliberately NOT in include/: nothing
// here is part of the template's surface, and a header the user can reach is a
// header the user will end up depending on.
//
// The public surface is include/raylib_multiplatform/assets.h.
// ---------------------------------------------------------------------------

#include <raylib.h>

namespace assets {
namespace detail {

// --- pack.cpp --------------------------------------------------------------
// The .rres container: opening it, closing it, and pulling one entry out.

// Look for RESOURCES_PATH/resources.rres and open its central directory.
// Returns true if a usable pack was opened. Safe to call twice.
bool OpenPack();

// Close it. Safe to call when nothing was open.
void ClosePack();

// True between a successful OpenPack() and ClosePack().
bool PackIsOpen();

// One entry, by its name in the central directory. Returns bytes owned by the
// caller (malloc'd, so UnloadFileData/RL_FREE frees them), or nullptr if the
// name is not packed, does not decrypt, or no pack is open. `size` receives
// the byte count and is zeroed on failure.
unsigned char *PackRead(const char *name, int *size);

// An image, decoded. Images are the one resource rrespacker stores decoded
// (an IMGE chunk) rather than as the original file, so this cannot go through
// PackRead(). Returns a zeroed Image when the name is not packed or does not
// decode; the caller then falls back to the loose file.
Image PackReadImage(const char *name);

// --- loader_hook.cpp -------------------------------------------------------
// Routing raylib's own LoadFileData/LoadFileText through the pack.

void InstallLoaderHook();
void RemoveLoaderHook();

// True when `path` points inside RESOURCES_PATH. Exposed here because it is
// the rule that decides what the hook is responsible for, and that rule is
// worth being able to read in one place.
bool InResourcesDir(const char *path);

// --- assets.cpp ------------------------------------------------------------
// Counters behind assets::RequestedLoads() / FailedLoads(), which the CI boot
// gate reads. Defined in assets.cpp.
extern int requestedLoads;
extern int failedLoads;

} // namespace detail
} // namespace assets
