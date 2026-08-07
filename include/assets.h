#pragma once

// Dual-mode resource loading.
//
//   - If a packed "resources.rres" exists under RESOURCES_PATH, assets are
//     loaded from it (rres container, optionally AES-encrypted).
//   - Otherwise assets are loaded as loose files from RESOURCES_PATH.
//
// This lets the same game run while iterating on loose files in development,
// and from an encrypted .rres in production (generated with rrespacker), with
// no code changes. rres packing is a build-time step; at runtime only the free
// rres loader is used.

#include <raylib.h>

namespace Assets {

// Detect and open the resource pack (if present). Call once in _ready() before
// loading any asset.
void Init();

// Release the pack (central directory). Call in _exit().
void Shutdown();

// True when assets are being served from a .rres pack.
bool UsingPack();

// Load an image by its resource name (e.g. "rabbit.png"). The caller owns the
// returned Image and must UnloadImage() it.
Image LoadImage(const char *name);

// Convenience: load an image and upload it to the GPU in one step. The caller
// owns the returned Texture2D and must UnloadTexture() it.
Texture2D LoadTexture(const char *name);

}  // namespace Assets
