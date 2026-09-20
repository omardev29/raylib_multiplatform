#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. The half of the animation layer that needs no GPU.
//
// tests/animation_test.cpp includes this directly, which is what lets the frame
// clock be checked at the exact millisecond a frame changes on: parsing a
// .aseprite and stepping the clock touch nothing but memory, and only the
// texture upload needs a window.
// ---------------------------------------------------------------------------

#include <raylib.h> // Texture2D

namespace rmp {
struct SheetData;
struct Sprite;
} // namespace rmp

namespace rmp::animation::detail {

// The bytes of a .aseprite or .ase into frames, durations and tags. Returns
// false if cute_aseprite refused it. The texture is NOT filled in: that is the
// half that needs a GL context.
bool parse_sheet(const void *bytes, int size, SheetData *out);

// The index of a tag by name, or -1. Case sensitive, because the tag names are
// the user's and we do not get to decide that "Walk" and "walk" are the same.
int tag_index(const SheetData &sheet, const char *name);

// The frames into one texture, side by side. Needs a GL context; without one
// raylib refuses politely and this comes back with id 0, which is a hole in the
// picture rather than a dead process.
::Texture2D upload_sheet(const void *bytes, int size, const SheetData &sheet);

// Frees the tables a parsed sheet owns, and its texture. Called from the
// resource table when the last handle to a sheet goes away.
void free_sheet(SheetData *sheet);

// One step of the animation clock. Advances by `delta` seconds scaled by
// sprite.speed, crossing as many frames as it owes.
void advance(Sprite &sprite, float delta);

} // namespace rmp::animation::detail
