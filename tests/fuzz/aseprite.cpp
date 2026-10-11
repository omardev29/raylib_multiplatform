// ---------------------------------------------------------------------------
// The Aseprite reader, fuzzed: src/rmp/aseprite.cpp, the bounds-checked walk
// every .aseprite takes before cute_aseprite sees it, and parse_sheet()
// (src/rmp/animation.cpp) behind it.
//
// The input is the bytes of a .aseprite, exactly as rmp::assets::load_data
// hands them over. The oracle is the reader's promise -- it reads a sheet or
// refuses one, never aborts (cute_aseprite asserts on what it cannot read),
// never reads or writes outside what it owns, never leaks, never allocates
// past its budgets (libFuzzer's -rss_limit_mb) and never does UB on the way --
// and, of every sheet it reads:
//
//   - a frame size, at least one frame, and every frame in one row, lasting a
//     finite time longer than nothing;
//   - every tag inside the frames, from <= to;
//   - nothing reported: a read never reports (RMP_REPORT_ONCE);
//   - read exactly when prepare_aseprite() accepts the file, and the copy it
//     hands cute_aseprite walks to itself: what was taken out of the file is
//     not in it any more, and nothing else was changed that a walk would;
//   - every tag played and stepped, short steps and long ones, never shows a
//     frame outside the tag and never leaves the clock at a NaN.
//
// Inputs: tests/fuzz/corpus/aseprite/ (seeded with files of every chunk type
// cute_aseprite reads, the three colour depths and real zlib streams -- fixed,
// dynamic and stored blocks) and tests/fuzz/regressions/aseprite/.
// tests/fuzz/aseprite.dict hands libFuzzer the format's magic numbers.
//
//   bash tools/fuzz.sh replay aseprite      every committed input, once
//   bash tools/fuzz.sh run aseprite 60      a minute of libFuzzer
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../../src/rmp/animation_internal.h"
#include "../../src/rmp/internal.h"

#include <rmp/assets.h>
#include <rmp/object.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace {

using rmp::test::fuzz::require;

// Every promise a sheet the reader accepted keeps.
void check_sheet(const rmp::SheetData &sheet) {
    require(sheet.width > 0 && sheet.height > 0, "a sheet has a frame size");
    require(sheet.frame_count() > 0, "a sheet has a frame");
    for (int i = 0; i < sheet.frame_count(); ++i) {
        const rmp::SheetFrame &frame = sheet.frame(i);
        require(std::isfinite(frame.seconds) && frame.seconds > 0,
                "every frame lasts a finite time longer than nothing");
        require(frame.source.x ==
                        static_cast<float>(i) * static_cast<float>(sheet.width) &&
                    frame.source.y == 0 &&
                    frame.source.width == static_cast<float>(sheet.width) &&
                    frame.source.height == static_cast<float>(sheet.height),
                "the frames are laid out in one row");
    }
    for (int i = 0; i < sheet.tag_count(); ++i) {
        const rmp::SheetTag &tag = sheet.tag(i);
        require(tag.from >= 0 && tag.from <= tag.to && tag.to < sheet.frame_count(),
                "every tag is inside the frames");
    }
}

// Every tag played and stepped: short steps, a frame's worth, and steps
// longer than the whole tag.
void check_clock(rmp::SheetData sheet) {
    const std::vector<rmp::SheetTag> tags = sheet.tags;
    rmp::Sprite sprite;
    sprite.sheet = rmp::SpriteSheet{ rmp::detail::adopt(rmp::detail::ResourceKind::SHEET,
                                                        std::move(sheet)) };
    require(sprite.sheet.valid(), "a parsed sheet is adopted");
    constexpr std::array<float, 5> STEPS{ 0.001f, 0.016f, 0.1f, 1.0f, 1000.0f };
    for (std::size_t t = 0; t < tags.size() && t < 8; ++t) {
        const int found =
            rmp::animation::detail::tag_index(sprite.sheet.raw(), tags[t].name);
        if (found < 0) continue; // a tag with no name cannot be played by one
        const rmp::SheetTag &tag = tags[static_cast<std::size_t>(found)];
        for (const bool loop : { true, false }) {
            sprite.play(tags[t].name, loop);
            for (const float speed : { 1.0f, -3.0f }) {
                sprite.speed = speed;
                for (const float step : STEPS) {
                    rmp::animation::detail::advance(sprite, step);
                    require(sprite.frame_index() >= tag.from &&
                                sprite.frame_index() <= tag.to,
                            "a tag played never shows a frame outside it");
                    require(std::isfinite(sprite.ours.elapsed) &&
                                sprite.ours.elapsed >= 0,
                            "the clock keeps a finite time");
                }
            }
            sprite.stop();
        }
    }
    sprite.sheet = rmp::SpriteSheet{};
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerInitialize(int * /*argc*/, char *** /*argv*/) {
    rmp::test::fuzz::initialize();
    return 0;
}

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> input = rmp::test::fuzz::input({ data, size });
    if (input.empty()) return 0;
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return 0;

    const int reports = rmp::detail::report_count();
    std::vector<unsigned char> copy;
    const bool accepted = rmp::animation::detail::prepare_aseprite(input, copy).empty();
    rmp::SheetData sheet;
    const bool parsed = rmp::animation::detail::parse_sheet(
        input.data(), static_cast<int>(input.size()), &sheet);
    require(rmp::detail::report_count() == reports, "reading a sheet reports nothing");
    require(parsed == accepted, "a sheet is read exactly when the walk accepts its file");
    if (!parsed) return 0;
    std::vector<unsigned char> again;
    require(rmp::animation::detail::prepare_aseprite(copy, again).empty() &&
                again == copy,
            "what the walk hands cute_aseprite walks to itself");
    check_sheet(sheet);
    check_clock(std::move(sheet));
    return 0;
}
