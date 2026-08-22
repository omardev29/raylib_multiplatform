#pragma once
// ---------------------------------------------------------------------------
// rmp::assets:: — loading from resources/
//
// Put your files in resources/ and load them by name. Which of the two ways
// they arrive is a build detail you do not have to think about:
//
//   - a packed resources.rres next to the executable (what a release ships),
//     optionally AES-encrypted;
//   - the loose files in resources/ (what you get while developing).
//
// rmp::assets::init() picks whichever exists. You never call it: the lifecycle
// macro in <rmp/app.h> does, before on_ready(), and
// Shutdown() after on_exit().
//
// Since Init() also teaches raylib itself to read the pack, plain raylib calls
// work too — LoadTexture(RESOURCES_PATH "player.png"), LoadModel, LoadShader.
// The rmp::assets:: functions are the shorter spelling, not a requirement.
// See TECHNICAL.md, "Resources", for the two things that stay outside this:
// LoadMusicStream, and files loaded from outside resources/.
//
// Implementation: src/rmp/.
//
// Everything this template adds lives under rmp::. What comes from raylib keeps
// its own name, so you can always tell at a glance which is which.
// ---------------------------------------------------------------------------

#include <raylib.h>

namespace rmp::assets {

// Detect and open the resource pack, if there is one, and route raylib's own
// file loading through it. Called for you by the lifecycle macro; calling it
// twice is harmless.
void init();

// Release the pack and unhook raylib's loaders. Called for you after on_exit().
void shutdown();

// True when assets are being served from a .rres pack.
bool using_pack();

// Load an image by its resource name (e.g. "rabbit.png"). The caller owns the
// returned Image and must UnloadImage() it.
Image load_image(const char *name);

// Convenience: load an image and upload it to the GPU in one step. The caller
// owns the returned Texture2D and must UnloadTexture() it.
Texture2D load_texture(const char *name);

// Load a sound effect (.wav/.ogg/.mp3/.qoa/...). InitAudioDevice() must have
// been called first. The caller owns it and must UnloadSound() it.
Sound load_sound(const char *name);

// Load a font (.ttf/.otf). fontSize is the baked glyph Size.
// The caller owns it and must UnloadFont() it.
Font load_font(const char *name, int font_size);

// Raw bytes for anything else — a level file, a shader, JSON. `Size` receives
// the byte count. Free with UnloadFileData().
unsigned char *load_data(const char *name, int *size);

// How many rmp::assets:: loads were asked for, and how many found nothing in the
// pack and nothing on disk either. The entry point reports these to the CI
// boot gate; you are unlikely to need them yourself.
int requested_loads();
int failed_loads();

} // namespace rmp::assets
