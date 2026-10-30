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

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rmp {
struct SheetData;
struct SheetSlice;
struct Sprite;
} // namespace rmp

namespace rmp::animation::detail {

// What one .aseprite may make cute_aseprite allocate, and blend. Budgets for
// the whole file, whatever it says: every frame's entry in cute_aseprite's
// table (room for 64 cels, some 6 KB) and its picture, RGBA, plus every cel's
// pixels; and every cel blended into its frame, clipped to it. 256 MB holds a
// sheet of 1024 px frames packed into a 16 384 px row -- the widest texture
// most GPUs take, and all of it in one texture -- with a layer's cels beside
// it; 64M pixels is four full layers of it.
inline constexpr std::int64_t MAX_SHEET_BYTES = std::int64_t{ 1 } << 28;
inline constexpr std::int64_t MAX_BLEND_PIXELS = std::int64_t{ 1 } << 26;

// src/rmp/aseprite.cpp: the bytes of a .aseprite walked chunk by chunk, every
// read bounds-checked, before cute_aseprite sees them. "" and the copy to hand
// cute_aseprite in `out` when it can read the file without a fault -- an
// assert, a read or write outside what it owns, a loop that never ends, a leak
// or a budget above -- or why not, with `out` empty. The copy differs from
// the file only where no pixel depends on it; the file comment says where.
std::string_view prepare_aseprite(std::span<const unsigned char> file,
                                  std::vector<unsigned char> &out);
// The same, and the file's slices -- which cute_aseprite never reads -- in
// `slices`: every one that reads whole and sits on the canvas, at most
// MAX_SLICES, in the file's order. Empty when the file is refused.
std::string_view prepare_aseprite(std::span<const unsigned char> file,
                                  std::vector<unsigned char> &out,
                                  std::vector<SheetSlice> &slices);
inline constexpr std::size_t MAX_SLICES = 256;

// The bytes of a .aseprite or .ase into frames, durations and tags. Returns
// false, and logs why, for a file prepare_aseprite() refuses. The texture is
// NOT filled in: that is the half that needs a GL context.
bool parse_sheet(const void *bytes, int size, SheetData *out);

// The index of a tag by name, or -1. Case sensitive, because the tag names are
// the user's and we do not get to decide that "Walk" and "walk" are the same.
int tag_index(const SheetData &sheet, std::string_view name);

// The frames into one texture, side by side. Needs a GL context; without one
// raylib refuses politely and this comes back with id 0, which is a hole in the
// picture rather than a dead process. So does a file that is not the sheet.
::Texture2D upload_sheet(const void *bytes, int size, const SheetData &sheet);

// One step of the animation clock. Advances by `delta` seconds scaled by
// sprite.speed, crossing as many frames as it owes. A step that is not a
// positive, finite time changes nothing -- and a speed that makes one warns
// once.
void advance(Sprite &sprite, float delta);

} // namespace rmp::animation::detail
