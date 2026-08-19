#pragma once
// ---------------------------------------------------------------------------
// assets:: — loading from resources/
//
// Put your files in resources/ and load them by name. Which of the two ways
// they arrive is a build detail you do not have to think about:
//
//   - a packed resources.rres next to the executable (what a release ships),
//     optionally AES-encrypted;
//   - the loose files in resources/ (what you get while developing).
//
// assets::Init() picks whichever exists. You never call it: the lifecycle
// macro in <raylib_multiplatform/lifecycle.h> does, before _ready(), and
// Shutdown() after _exit().
//
// Since Init() also teaches raylib itself to read the pack, plain raylib calls
// work too — LoadTexture(RESOURCES_PATH "player.png"), LoadModel, LoadShader.
// The assets:: functions are the shorter spelling, not a requirement.
// See TECHNICAL.md, "Resources", for the two things that stay outside this:
// LoadMusicStream, and files loaded from outside resources/.
//
// Implementation: src/raylib_multiplatform/.
// ---------------------------------------------------------------------------

#include <raylib.h>

namespace assets {

// Detect and open the resource pack, if there is one, and route raylib's own
// file loading through it. Called for you by the lifecycle macro; calling it
// twice is harmless.
void Init();

// Release the pack and unhook raylib's loaders. Called for you after _exit().
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

// How many assets:: loads were asked for, and how many found nothing in the
// pack and nothing on disk either. The entry point reports these to the CI
// boot gate; you are unlikely to need them yourself.
int RequestedLoads();
int FailedLoads();

} // namespace assets
