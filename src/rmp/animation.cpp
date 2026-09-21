// ---------------------------------------------------------------------------
// Sprite sheets from Aseprite files, and the animation clock.
//
// Two halves that are deliberately separable:
//
//   PARSING turns the bytes of a .aseprite into a SheetData -- frames, their
//   durations, and the tags. It needs no GPU, which is the whole reason
//   tests/animation_test.cpp can exist and can check the exact millisecond a
//   frame changes on.
//
//   UPLOADING packs the frames into one texture. It needs a GL context, so it
//   is the half that does nothing headless. A sheet loaded without a window
//   comes back with every frame, every duration and every tag, and an empty
//   texture -- which is a hole in the picture rather than a dead process, the
//   same answer every other missing resource gives.
//
// The frames go into ONE texture in a single row, so a hundred enemies drawn
// from the same sheet are one texture bind.
// ---------------------------------------------------------------------------

#include <rmp/assets.h>
#include <rmp/object.h>

#include "animation_internal.h"
#include "internal.h"

#include <cute_aseprite.h>

#include <cstring>

namespace rmp {

namespace {

// Aseprite's own maximum is 256 tags; ours is 64, and a sheet with more than
// that loses the extras rather than overrunning the array. Said out loud in a
// warning, because silently dropping half of somebody's animations is the kind
// of thing that gets blamed on Aseprite.
void copy_name(char *into, const char *from) {
    if (from == nullptr) {
        into[0] = '\0';
        return;
    }
    std::size_t i = 0;
    for (; from[i] != '\0' && i + 1 < static_cast<std::size_t>(kMaxTagName); i++) {
        into[i] = from[i];
    }
    into[i] = '\0';
}

} // namespace

namespace animation::detail {

// cute_aseprite ASSERTS on a malformed file rather than returning null, and an
// assert is abort() -- so a corrupt or truncated .aseprite in a shipped game
// would take the process down. That is not an acceptable answer to a bad asset;
// every other loader here comes back empty and leaves a hole in the picture.
//
// So the header is checked first, cheaply, against what the format guarantees.
// It catches everything that is not an Aseprite file at all, which is the case
// that actually happens: the wrong file in resources/, a truncated download, a
// .png renamed. A file that passes this and is still corrupt deeper in is
// cute_aseprite's business and it will still assert -- said plainly rather than
// claimed away.
bool looks_like_aseprite(const unsigned char *bytes, int size) {
    if (size < 128) return false; // the header alone is 128 bytes
    const auto read16 = [bytes](int at) {
        return static_cast<int>(bytes[at]) | (static_cast<int>(bytes[at + 1]) << 8);
    };
    const auto read32 = [bytes](int at) {
        return static_cast<unsigned>(bytes[at]) |
            (static_cast<unsigned>(bytes[at + 1]) << 8) |
            (static_cast<unsigned>(bytes[at + 2]) << 16) |
            (static_cast<unsigned>(bytes[at + 3]) << 24);
    };

    if (read16(4) != 0xA5E0) return false; // the magic
    if (read32(0) != static_cast<unsigned>(size)) return false; // the declared size

    const int frames = read16(6);
    const int w = read16(8);
    const int h = read16(10);
    const int depth = read16(12);
    if (frames <= 0 || w <= 0 || h <= 0) return false;
    if (depth != 8 && depth != 16 && depth != 32) return false;
    return true;
}

bool parse_sheet(const void *bytes, int size, SheetData *out) {
    if (bytes == nullptr || size <= 0 || out == nullptr) return false;
    if (!looks_like_aseprite(static_cast<const unsigned char *>(bytes), size)) {
        return false;
    }

    ase_t *ase = cute_aseprite_load_from_memory(bytes, size, nullptr);
    if (ase == nullptr) return false;

    *out = SheetData{};
    out->width = ase->w;
    out->height = ase->h;

    out->frame_count =
        ase->frame_count < kMaxSheetFrames ? ase->frame_count : kMaxSheetFrames;
    if (ase->frame_count > kMaxSheetFrames) {
        TraceLog(LOG_WARNING,
                 "SHEET: %d frames and the limit is %d; the extras are dropped",
                 ase->frame_count, kMaxSheetFrames);
    }
    auto *frames = new SheetFrame[static_cast<std::size_t>(
        out->frame_count > 0 ? out->frame_count : 1)];
    out->frames = frames;
    for (int i = 0; i < out->frame_count; i++) {
        // Laid out in one row, so the source rectangle is just an offset.
        frames[i].source =
            Rectangle{ static_cast<float>(i * ase->w), 0, static_cast<float>(ase->w),
                       static_cast<float>(ase->h) };
        // Aseprite stores milliseconds per frame. A frame with a duration of 0
        // would be a division by nothing later and an animation that never
        // advances; the file should not contain one, and if it does it is
        // treated as a single tick rather than as a stall.
        const int ms = ase->frames[i].duration_milliseconds;
        frames[i].seconds = (ms > 0 ? static_cast<float>(ms) : 1.0f) / 1000.0f;
    }

    out->tag_count = ase->tag_count < kMaxSheetTags ? ase->tag_count : kMaxSheetTags;
    if (ase->tag_count > kMaxSheetTags) {
        TraceLog(LOG_WARNING,
                 "SHEET: %d tags and the limit is %d; the extras are dropped",
                 ase->tag_count, kMaxSheetTags);
    }
    auto *tags =
        new SheetTag[static_cast<std::size_t>(out->tag_count > 0 ? out->tag_count : 1)];
    out->tags = tags;
    for (int i = 0; i < out->tag_count; i++) {
        const ase_tag_t &tag = ase->tags[i];
        copy_name(tags[i].name, tag.name);
        tags[i].from = tag.from_frame;
        tags[i].to = tag.to_frame;
        tags[i].ping_pong =
            tag.loop_animation_direction == ASE_ANIMATION_DIRECTION_PINGPONG;
        tags[i].reverse =
            tag.loop_animation_direction == ASE_ANIMATION_DIRECTION_BACKWORDS;
    }

    cute_aseprite_free(ase);
    return true;
}

::Texture2D upload_sheet(const void *bytes, int size, const SheetData &sheet) {
    ::Texture2D empty{};
    if (sheet.frame_count <= 0 || sheet.width <= 0 || sheet.height <= 0) return empty;

    // Parsed a second time rather than kept from the first: cute_aseprite owns
    // the pixel buffers and frees them with the ase_t, so holding them across
    // the call would mean either leaking the whole document or copying every
    // frame twice. Parsing a sprite sheet costs microseconds and happens once.
    ase_t *ase = cute_aseprite_load_from_memory(bytes, size, nullptr);
    if (ase == nullptr) return empty;

    const int total_w = sheet.width * sheet.frame_count;
    ::Image atlas = GenImageColor(total_w, sheet.height, BLANK);
    for (int i = 0; i < sheet.frame_count; i++) {
        ::Image one{};
        one.data = ase->frames[i].pixels;
        one.width = ase->w;
        one.height = ase->h;
        one.mipmaps = 1;
        one.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        const Rectangle src{ 0, 0, static_cast<float>(ase->w),
                             static_cast<float>(ase->h) };
        const Rectangle dst{ static_cast<float>(i * sheet.width), 0,
                             static_cast<float>(sheet.width),
                             static_cast<float>(sheet.height) };
        ImageDraw(&atlas, one, src, dst, WHITE);
        // `one` is NOT unloaded: its pixels belong to the ase_t.
    }

    ::Texture2D texture = LoadTextureFromImage(atlas);
    UnloadImage(atlas);
    cute_aseprite_free(ase);
    return texture;
}

void free_sheet(SheetData *sheet) {
    if (sheet == nullptr) return;
    if (sheet->texture.id != 0) UnloadTexture(sheet->texture);
    delete[] sheet->frames;
    delete[] sheet->tags;
    sheet->frames = nullptr;
    sheet->tags = nullptr;
    sheet->frame_count = 0;
    sheet->tag_count = 0;
}

int tag_index(const SheetData &sheet, const char *name) {
    if (name == nullptr || name[0] == '\0') return -1;
    for (int i = 0; i < sheet.tag_count; i++) {
        if (std::strcmp(sheet.tag(i).name, name) == 0) return i;
    }
    return -1;
}

// One frame's worth of clock. Split out of advance() so the test can step it
// without an object, and because the loop below has to be able to run it more
// than once when a frame is shorter than the delta.
void advance(Sprite &sprite, float delta) {
    if (!sprite.sheet.valid()) return;
    const SheetData &sheet = sprite.sheet.raw();
    if (sheet.frame_count <= 0) return;
    if (sprite.ours.tag < 0 || sprite.ours.tag >= sheet.tag_count) return;
    if (sprite.ours.done) return;
    if (sprite.speed == 0 || delta == 0) return;

    const SheetTag &tag = sheet.tag(sprite.ours.tag);
    const int first = tag.from < 0 ? 0 : tag.from;
    const int last = tag.to >= sheet.frame_count ? sheet.frame_count - 1 : tag.to;
    if (last < first) return;

    sprite.ours.elapsed += delta * (sprite.speed < 0 ? -sprite.speed : sprite.speed);

    // A while loop, because a frame can be shorter than the delta -- a 20 ms
    // frame at 30 fps owes two steps, and dropping them makes the animation run
    // slow on a slow machine rather than skipping, which is worse.
    for (int guard = 0; guard < kMaxSheetFrames * 4; guard++) {
        const int index = sprite.ours.frame < 0
            ? first
            : (sprite.ours.frame >= sheet.frame_count ? last : sprite.ours.frame);
        const float hold = sheet.frame(index).seconds;
        if (hold <= 0 || sprite.ours.elapsed < hold) break;
        sprite.ours.elapsed -= hold;

        const bool backwards = tag.reverse != sprite.ours.back;
        int next = sprite.ours.frame + (backwards ? -1 : 1);

        if (next > last || next < first) {
            if (tag.ping_pong && last > first) {
                sprite.ours.back = !sprite.ours.back;
                next = sprite.ours.frame + (sprite.ours.back != tag.reverse ? -1 : 1);
                if (next > last || next < first) next = sprite.ours.frame;
            } else if (sprite.ours.loop) {
                next = backwards ? last : first;
            } else {
                // Stops ON the last frame, not past it, and says so. A
                // non-looping animation that ended on a blank frame is the
                // classic way a death animation disappears.
                sprite.ours.frame = backwards ? first : last;
                sprite.ours.done = true;
                sprite.ours.elapsed = 0;
                return;
            }
        }
        sprite.ours.frame = next;
    }
}

} // namespace animation::detail

// ---------------------------------------------------------------------------
// The public half
// ---------------------------------------------------------------------------

void Sprite::play(const char *tag, bool loop) {
    if (!sheet.valid()) {
        RMP_REPORT_ONCE_KEYED(tag, "SPRITE: play(\"%s\") with no sheet loaded",
                              tag != nullptr ? tag : "");
        return;
    }
    const SheetData &data = sheet.raw();
    const int found = animation::detail::tag_index(data, tag);
    if (found < 0) {
        // Once per tag name. Sixty warnings a second about the same typo
        // buries whatever else the log was going to say, and the frame that
        // was already showing is a better answer than a blank one.
        RMP_REPORT_ONCE_KEYED(tag,
                              "SPRITE: no animation tag \"%s\" in this sheet. It has %d: "
                              "the names are the tags in your .aseprite.",
                              tag != nullptr ? tag : "", data.tag_count);
        return;
    }
    if (found == ours.tag && !ours.done) return; // already playing it

    ours.tag = found;
    ours.loop = loop;
    ours.done = false;
    ours.back = false;
    ours.elapsed = 0;
    ours.frame = data.tag(found).reverse ? data.tag(found).to : data.tag(found).from;
}

void Sprite::stop() {
    ours.done = true;
    ours.elapsed = 0;
}

bool Sprite::finished() const { return ours.done; }

const char *Sprite::playing() const {
    if (!sheet.valid() || ours.tag < 0) return "";
    const SheetData &data = sheet.raw();
    if (ours.tag >= data.tag_count) return "";
    return data.tag(ours.tag).name;
}

void Sprite::set_frame(int index) {
    ours.frame = index;
    ours.elapsed = 0;
    ours.done = false;
}

} // namespace rmp
