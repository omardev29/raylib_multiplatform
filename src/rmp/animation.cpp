// ---------------------------------------------------------------------------
// Sprite sheets from Aseprite files, and the animation clock.
//
// Two halves that are deliberately separable:
//
//   PARSING turns the bytes of a .aseprite into a SheetData -- frames, their
//   durations, and the tags. It needs no GPU, which is the whole reason
//   tests/animation_test.cpp can exist and can check the exact millisecond a
//   frame changes on. cute_aseprite does the reading, and only of what
//   src/rmp/aseprite.cpp has walked first and accepted: on its own it asserts
//   -- abort() -- on a file broken past its header.
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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rmp {

namespace animation::detail {

namespace {

// Aseprite 1.3's "Ping-pong Reverse": last frame first, then back. cute_aseprite's
// enum stops at ping-pong, and 3 is still inside its range, so it is read as a number.
constexpr int PING_PONG_REVERSE = 3;

// What cute_aseprite reads of a file prepare_aseprite() accepted, or null --
// and why not, logged, when the file is refused. Freed with
// cute_aseprite_free().
ase_t *load(std::span<const unsigned char> file, std::vector<SheetSlice> &slices) {
    std::vector<unsigned char> safe;
    const std::string_view why = prepare_aseprite(file, safe, slices);
    if (!why.empty()) {
        // A file is the game's to fix, and the load says which one: the asset
        // layer logs its name next, and counts it in failed_loads().
        TraceLog(LOG_WARNING, "SHEET: refused: %.*s", static_cast<int>(why.size()),
                 why.data());
        return nullptr;
    }
    return cute_aseprite_load_from_memory(safe.data(), static_cast<int>(safe.size()),
                                          nullptr);
}

} // namespace

bool parse_sheet(const void *bytes, int size, SheetData *out) {
    if (bytes == nullptr || size <= 0 || out == nullptr) return false;
    std::vector<SheetSlice> slices;
    ase_t *ase = load(
        { static_cast<const unsigned char *>(bytes), static_cast<std::size_t>(size) },
        slices);
    if (ase == nullptr) return false;

    *out = SheetData{};
    out->slices = std::move(slices);
    out->width = ase->w;
    out->height = ase->h;

    out->frames.resize(
        static_cast<std::size_t>(ase->frame_count > 0 ? ase->frame_count : 0));
    for (int i = 0; i < out->frame_count(); ++i) {
        SheetFrame &frame = out->frames[static_cast<std::size_t>(i)];
        // Laid out in one row, so the source rectangle is just an offset.
        frame.source =
            Rectangle{ static_cast<float>(i * ase->w), 0, static_cast<float>(ase->w),
                       static_cast<float>(ase->h) };
        // Aseprite stores milliseconds per frame. A frame with a duration of 0
        // would be a division by nothing later and an animation that never
        // advances; the file should not contain one, and if it does it is
        // treated as a single tick rather than as a stall.
        const int ms = ase->frames[i].duration_milliseconds;
        frame.seconds = (ms > 0 ? static_cast<float>(ms) : 1.0f) / 1000.0f;
    }

    // prepare_aseprite() has held every tag inside the frames, from <= to.
    out->tags.resize(static_cast<std::size_t>(ase->tag_count > 0 ? ase->tag_count : 0));
    for (int i = 0; i < out->tag_count(); ++i) {
        const ase_tag_t &tag = ase->tags[i];
        SheetTag &ours = out->tags[static_cast<std::size_t>(i)];
        // All of it, whatever its length; a tag the file left unnamed is "".
        ours.name = tag.name != nullptr ? tag.name : "";
        ours.from = tag.from_frame;
        ours.to = tag.to_frame;
        const int way = static_cast<int>(tag.loop_animation_direction);
        ours.ping_pong =
            way == ASE_ANIMATION_DIRECTION_PINGPONG || way == PING_PONG_REVERSE;
        ours.reverse =
            way == ASE_ANIMATION_DIRECTION_BACKWORDS || way == PING_PONG_REVERSE;
    }

    cute_aseprite_free(ase);
    return true;
}

::Texture2D upload_sheet(const void *bytes, int size, const SheetData &sheet) {
    ::Texture2D empty{};
    if (bytes == nullptr || size <= 0) return empty;
    if (sheet.frame_count() <= 0 || sheet.width <= 0 || sheet.height <= 0) return empty;

    // Parsed a second time rather than kept from the first: cute_aseprite owns
    // the pixel buffers and frees them with the ase_t, so holding them across
    // the call would mean either leaking the whole document or copying every
    // frame twice. Parsing a sprite sheet costs microseconds and happens once.
    std::vector<SheetSlice> unused;
    ase_t *ase = load(
        { static_cast<const unsigned char *>(bytes), static_cast<std::size_t>(size) },
        unused);
    if (ase == nullptr) return empty;
    // These bytes, and the sheet they were parsed into: anything else would
    // read frames the file does not have.
    if (ase->w != sheet.width || ase->h != sheet.height ||
        ase->frame_count != sheet.frame_count()) {
        cute_aseprite_free(ase);
        return empty;
    }

    // Inside int: prepare_aseprite() held the frames' pixels to MAX_SHEET_BYTES.
    const int total_w = sheet.width * sheet.frame_count();
    ::Image atlas = GenImageColor(total_w, sheet.height, BLANK);
    for (int i = 0; i < sheet.frame_count(); ++i) {
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

int tag_index(const SheetData &sheet, std::string_view name) {
    if (name.empty()) return -1;
    for (int i = 0; i < sheet.tag_count(); ++i) {
        if (sheet.tag(i).name == name) return i;
    }
    return -1;
}

namespace {

// The time of one lap of a tag: every frame once and, ping-pong, the frames
// between the two ends once more on the way back -- or 0 when a frame of it
// holds for no time, or for no number, and the run stalls there instead.
float lap_of(const SheetData &sheet, int first, int last, bool ping_pong) {
    float lap = 0;
    for (int i = first; i <= last; ++i) {
        const float hold = sheet.frame(i).seconds;
        if (!(hold > 0)) return 0;
        lap += hold;
        if (ping_pong && i > first && i < last) lap += hold;
    }
    return lap;
}

} // namespace

// One frame's worth of clock. Split out of advance() so the test can step it
// without an object, and because the loop below has to be able to run it more
// than once when a frame is shorter than the delta.
void advance(Sprite &sprite, float delta) {
    // `ours` is public: a time written there that is not one starts the
    // frame over, rather than keeping a NaN that no hold is ever less than --
    // whether or not this step goes on to use it.
    if (!(sprite.ours.elapsed >= 0) || !std::isfinite(sprite.ours.elapsed)) {
        sprite.ours.elapsed = 0;
    }
    if (!sprite.sheet.valid()) return;
    const SheetData &sheet = sprite.sheet.raw();
    if (sheet.frame_count() <= 0) return;
    if (sprite.ours.tag < 0 || sprite.ours.tag >= sheet.tag_count()) return;
    if (sprite.ours.done) return;
    // No time, or not a time: nothing to do. A negative delta does not run
    // the clock back -- the sign of speed is the direction.
    if (!(delta > 0) || !std::isfinite(delta) || sprite.speed == 0) return;

    const SheetTag &tag = sheet.tag(sprite.ours.tag);
    const int first = tag.from < 0 ? 0 : tag.from;
    const int last = tag.to >= sheet.frame_count() ? sheet.frame_count() - 1 : tag.to;
    if (last < first) return;

    // The magnitude is the rate and the SIGN IS THE DIRECTION: rmp/object.h
    // has documented `-1 = backwards` since the field existed, and the clock
    // took the absolute value of both -- so -1 ran forwards at normal speed,
    // the one setting of the three that did not do what it said.
    const float owed = sprite.ours.elapsed + delta * std::fabs(sprite.speed);
    if (!std::isfinite(owed)) {
        // A NaN speed made the clock NaN, and every step after it spun
        // through frames: no hold is ever more than NaN. The game's mistake,
        // so it is said once, and the frame holds.
        RMP_REPORT_ONCE("SPRITE: speed %g is not a rate the clock can run at; the frame "
                        "holds until it is",
                        static_cast<double>(sprite.speed));
        return;
    }
    sprite.ours.elapsed = owed;

    // Three things can reverse the run and they compose: the tag's own
    // direction from Aseprite, the leg of a ping-pong we are on, and the sign
    // of speed. An odd number of them means backwards.
    const auto going_back = [&sprite, &tag]() {
        return (tag.reverse != sprite.ours.back) != (sprite.speed < 0);
    };
    const bool bounces = tag.ping_pong && last > first;
    // More steps than a lap takes, ping-pong's included.
    const int lap_steps = (last - first + 1) * 2;

    // A while loop, because a frame can be shorter than the delta -- a 20 ms
    // frame at 30 fps owes two steps, and dropping them makes the animation run
    // slow on a slow machine rather than skipping, which is worse.
    for (int guard = 0; guard < sheet.frame_count() * 4 + 4; ++guard) {
        const int index = sprite.ours.frame < 0
            ? first
            : (sprite.ours.frame >= sheet.frame_count() ? last : sprite.ours.frame);
        const float hold = sheet.frame(index).seconds;
        if (!(hold > 0) || sprite.ours.elapsed < hold) break;
        // Still owing after a lap's worth of steps -- a long pause, a huge
        // speed: a whole lap brings the run back to the frame it is on, so
        // only what is left after the whole laps shows. Without this the
        // guard stopped the loop with the time still owed, and the same
        // frames spun past on every step after.
        if (guard == lap_steps && (bounces || sprite.ours.loop) &&
            sprite.ours.frame >= first && sprite.ours.frame <= last) {
            const float lap = lap_of(sheet, first, last, bounces);
            if (lap > 0 && std::isfinite(lap)) {
                sprite.ours.elapsed = std::fmod(sprite.ours.elapsed, lap);
                if (sprite.ours.elapsed < hold) break;
            }
        }
        sprite.ours.elapsed -= hold;

        // In 64 bits: set_frame() takes any int, and INT_MAX + 1 is undefined.
        const bool backwards = going_back();
        const std::int64_t frame = sprite.ours.frame;
        std::int64_t next = frame + (backwards ? -1 : 1);

        if (next > last || next < first) {
            if (bounces) {
                sprite.ours.back = !sprite.ours.back;
                // Back the other way -- and from a frame set outside the tag,
                // into it at the nearer end: staying put kept it out for good.
                next = std::clamp<std::int64_t>(frame + (going_back() ? -1 : 1), first,
                                                last);
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
        sprite.ours.frame = static_cast<int>(next);
    }
}

} // namespace animation::detail

// ---------------------------------------------------------------------------
// The public half
// ---------------------------------------------------------------------------

void Sprite::play(std::string_view tag, bool loop) {
    if (!sheet.valid()) {
        const std::string named(tag); // %s wants the NUL a view does not promise
        RMP_REPORT_ONCE_KEYED(tag, "SPRITE: play(\"%s\") with no sheet loaded",
                              named.c_str());
        return;
    }
    const SheetData &data = sheet.raw();
    const int found = animation::detail::tag_index(data, tag);
    if (found < 0) {
        // Once per tag name. Sixty warnings a second about the same typo
        // buries whatever else the log was going to say, and the frame that
        // was already showing is a better answer than a blank one.
        const std::string named(tag);
        RMP_REPORT_ONCE_KEYED(tag,
                              "SPRITE: no animation tag \"%s\" in this sheet. It has %d: "
                              "the names are the tags in your .aseprite.",
                              named.c_str(), data.tag_count());
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

std::string_view Sprite::playing() const {
    if (!sheet.valid() || ours.tag < 0) return "";
    const SheetData &data = sheet.raw();
    if (ours.tag >= data.tag_count()) return "";
    return data.tag(ours.tag).name;
}

void Sprite::set_frame(int index) {
    ours.frame = index;
    ours.elapsed = 0;
    ours.done = false;
}

} // namespace rmp
