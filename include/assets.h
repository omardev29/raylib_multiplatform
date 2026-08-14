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

// Load a sound effect (.wav/.ogg/.mp3/.qoa/...). InitAudioDevice() must have
// been called first. The caller owns it and must UnloadSound() it.
Sound LoadSound(const char *name);

// Load a font (.ttf/.otf). fontSize is the baked glyph size.
// The caller owns it and must UnloadFont() it.
Font LoadFont(const char *name, int fontSize);

// Raw bytes for anything else — a level file, a shader, JSON. `size` receives
// the byte count. Free with UnloadFileData().
unsigned char *LoadData(const char *name, int *size);

}  // namespace Assets

// WHAT THE PACK CANNOT DO
//
// 3D models are the one real gap, and it is raylib's rather than ours: LoadModel
// takes a path, not a memory buffer, because it has to resolve the material and
// texture paths a .obj/.gltf refers to. There is no LoadModelFromMemory to build
// on. Load models with plain raylib —
//
//     Model m = ::LoadModel(RESOURCES_PATH "ship.obj");
//
// — and see TECHNICAL.md, because a release built with a resource pack ships
// only the pack, so anything loaded by path needs shipping alongside it.
