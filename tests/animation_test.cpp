// ===========================================================================
// Sprite sheets and the animation clock.
//
// Over a REAL .aseprite: tests/fixtures/anim.aseprite, four 4x4 frames and
// three tags, written by tools/make_aseprite_fixture.py from the published file
// format. Parsing a fake one would be testing the fake.
//
// The frame durations in the fixture are deliberately different -- 100, 200,
// 100 and 50 ms -- because a fixture where every frame lasts the same length
// cannot tell "reads the durations from the file" apart from "assumes a fixed
// frame rate", and reading them is the thing this layer promises.
//
// No window anywhere in here. Parsing and the clock touch nothing but memory;
// only the texture upload needs a GL context, and nothing below asks for one.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/animation_internal.h"
#include "../src/rmp/internal.h"
#include "aseprite_builder.h"

#include <rmp/assets.h>
#include <rmp/object.h>

#include <cute_aseprite.h>

#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<unsigned char> fixture_bytes() {
    const std::string path = std::string(RMP_TEST_FIXTURES) + "anim.aseprite";
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(),
                    "missing " << path << " -- run tools/make_aseprite_fixture.py");
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

// A parsed sheet that frees its own tables. parse_sheet allocates them and the
// tables are vectors and free themselves; there is no texture, because no
// test here uploads one.
struct ParsedSheet {
    rmp::SheetData data{};
    ParsedSheet() {
        const std::vector<unsigned char> bytes = fixture_bytes();
        REQUIRE(rmp::animation::detail::parse_sheet(
            bytes.data(), static_cast<int>(bytes.size()), &data));
    }
};

// A Sprite holding the fixture, without a texture. adopt() is the same seam the
// rres pack uses, so this is not a special path for tests -- it is the ordinary
// one with the GPU half left out.
struct Fixture {
    rmp::Sprite sprite;

    Fixture() {
        // adopt() takes the sheet by value and the slot owns it from then on,
        // tables and all, freeing it when the last handle goes.
        ParsedSheet owned;
        auto *slot =
            rmp::detail::adopt(rmp::detail::ResourceKind::SHEET, std::move(owned.data));
        REQUIRE(slot != nullptr);
        sprite.sheet = rmp::SpriteSheet{ slot };
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    ~Fixture() {
        sprite.sheet = rmp::SpriteSheet{};
        rmp::detail::release_all();
    }
};

void step(rmp::Sprite &sprite, float seconds) {
    rmp::animation::detail::advance(sprite, seconds);
}

} // namespace

// ---------------------------------------------------------------------------
// What is in the file
// ---------------------------------------------------------------------------

TEST_CASE("the frames, their sizes and their durations come from the file") {
    const ParsedSheet owned;
    const rmp::SheetData &sheet = owned.data;

    CHECK(sheet.width == 4);
    CHECK(sheet.height == 4);
    REQUIRE(sheet.frame_count() == 4);

    // The numbers the fixture was written with, in seconds.
    CHECK(sheet.frame(0).seconds == doctest::Approx(0.100));
    CHECK(sheet.frame(1).seconds == doctest::Approx(0.200));
    CHECK(sheet.frame(2).seconds == doctest::Approx(0.100));
    CHECK(sheet.frame(3).seconds == doctest::Approx(0.050));

    SUBCASE("and they are packed side by side in one row") {
        for (int i = 0; i < sheet.frame_count(); ++i) {
            CAPTURE(i);
            CHECK(sheet.frame(i).source.x == doctest::Approx(i * 4));
            CHECK(sheet.frame(i).source.y == doctest::Approx(0));
            CHECK(sheet.frame(i).source.width == doctest::Approx(4));
        }
    }
}

TEST_CASE("the tags are read with their names, ranges and direction") {
    const ParsedSheet owned;
    const rmp::SheetData &sheet = owned.data;
    REQUIRE(sheet.tag_count() == 3);

    CHECK(std::string(sheet.tag(0).name) == "walk");
    CHECK(sheet.tag(0).from == 0);
    CHECK(sheet.tag(0).to == 1);
    CHECK_FALSE(sheet.tag(0).ping_pong);

    CHECK(std::string(sheet.tag(1).name) == "idle");
    CHECK(sheet.tag(1).from == 2);
    CHECK(sheet.tag(1).to == 3);

    CHECK(std::string(sheet.tag(2).name) == "swing");
    CHECK(sheet.tag(2).ping_pong);
}

TEST_CASE("a tag is found by name, exactly, and nothing else is") {
    const ParsedSheet owned;
    const rmp::SheetData &sheet = owned.data;
    CHECK(rmp::animation::detail::tag_index(sheet, "walk") == 0);
    CHECK(rmp::animation::detail::tag_index(sheet, "idle") == 1);
    // Case sensitive, because the names are the user's and we do not get to
    // decide that "Walk" is the same animation as "walk".
    CHECK(rmp::animation::detail::tag_index(sheet, "Walk") == -1);
    CHECK(rmp::animation::detail::tag_index(sheet, "wal") == -1);
    CHECK(rmp::animation::detail::tag_index(sheet, "") == -1);
    // A view, not a C string: what is asked for is exactly its characters,
    // with or without a NUL after them.
    CHECK(rmp::animation::detail::tag_index(sheet, std::string_view("walking", 4)) == 0);
    CHECK(rmp::animation::detail::tag_index(sheet, std::string_view("walk\0", 5)) == -1);
}

TEST_CASE("rubbish is refused rather than half-read") {
    rmp::SheetData sheet{};
    const std::array<unsigned char, 8> junk{ 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK_FALSE(rmp::animation::detail::parse_sheet(junk.data(), junk.size(), &sheet));
    CHECK_FALSE(rmp::animation::detail::parse_sheet(nullptr, 100, &sheet));
    CHECK_FALSE(rmp::animation::detail::parse_sheet(junk.data(), 0, &sheet));
    CHECK_FALSE(rmp::animation::detail::parse_sheet(junk.data(), junk.size(), nullptr));
}

// ---------------------------------------------------------------------------
// The clock, and the exact boundary
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "play starts on the tag's first frame") {
    sprite.play("walk");
    CHECK(sprite.frame_index() == 0);
    CHECK(std::string(sprite.playing()) == "walk");
    CHECK_FALSE(sprite.finished());

    SUBCASE("and a different tag starts on ITS first frame") {
        sprite.play("idle");
        CHECK(sprite.frame_index() == 2);
        CHECK(std::string(sprite.playing()) == "idle");
    }
}

TEST_CASE_FIXTURE(Fixture, "play takes a view: the tag is its characters and no more") {
    // Not NUL-terminated on purpose: "walk" out of the middle of a longer
    // string. A strcmp() under the API would read on into "ing" and miss.
    const std::string source = "walking";
    sprite.play(std::string_view(source).substr(0, 4));
    CHECK(sprite.playing() == "walk");
    sprite.play(std::string("idle"));
    CHECK(sprite.playing() == "idle");
}

TEST_CASE_FIXTURE(Fixture, "playing() is the sheet's own name, and a NUL follows it") {
    // The contract every string_view the framework returns keeps: it views a
    // string the framework holds, so the character after it is a terminator.
    sprite.play("swing");
    const std::string_view now = sprite.playing();
    REQUIRE(now == "swing");
    CHECK(std::string_view(now.data(), now.size() + 1).back() == '\0');
    CHECK(now.data() == sprite.sheet.raw().tag(2).name.data());

    SUBCASE("and so is the empty answer, when nothing plays") {
        const rmp::Sprite idle;
        const std::string_view none = idle.playing();
        CHECK(none.empty());
        REQUIRE(none.data() != nullptr);
        CHECK(std::string_view(none.data(), none.size() + 1).back() == '\0');
    }
}

TEST_CASE_FIXTURE(Fixture, "the frame changes on the exact millisecond and not before") {
    // THE test in this file. Frame 0 lasts 100 ms, so at 99 it is still frame 0
    // and at 100 it is frame 1 -- and the difference between those two is a
    // whole class of "the animation is one frame behind" bugs.
    sprite.play("walk");

    step(sprite, 0.099f);
    CHECK(sprite.frame_index() == 0);

    step(sprite, 0.001f);
    CHECK(sprite.frame_index() == 1);

    SUBCASE("and the second frame holds for its own 200, not for another 100") {
        step(sprite, 0.199f);
        CHECK(sprite.frame_index() == 1);
        step(sprite, 0.001f);
        CHECK(sprite.frame_index() == 0); // looped back
    }
}

TEST_CASE_FIXTURE(Fixture, "one long step crosses as many frames as it owes") {
    // A frame shorter than the delta is normal -- 50 ms at 30 fps -- and
    // dropping the extra steps makes the animation run slow on a slow machine,
    // which is worse than skipping.
    sprite.play("idle"); // frames 2 (100 ms) and 3 (50 ms)
    step(sprite, 0.150f); // past both
    CHECK(sprite.frame_index() == 2); // wrapped round to the start of the tag
}

TEST_CASE_FIXTURE(Fixture, "a looping tag comes back to its first frame") {
    sprite.play("walk", true);
    for (int i = 0; i < 10; ++i) step(sprite, 0.100f);
    CHECK_FALSE(sprite.finished());
    // Still inside the tag, wherever in it.
    CHECK(sprite.frame_index() >= 0);
    CHECK(sprite.frame_index() <= 1);
}

TEST_CASE_FIXTURE(Fixture, "a tag that does not loop stops ON the last frame") {
    // Not past it. An animation that ended on a blank frame is the classic way
    // a death animation disappears instead of holding its last pose.
    sprite.play("walk", false);
    step(sprite, 0.100f);
    CHECK(sprite.frame_index() == 1);
    CHECK_FALSE(sprite.finished());

    step(sprite, 0.200f);
    CHECK(sprite.frame_index() == 1); // held, not advanced off the end
    CHECK(sprite.finished());

    SUBCASE("and it stays there however long you wait") {
        for (int i = 0; i < 100; ++i) step(sprite, 0.100f);
        CHECK(sprite.frame_index() == 1);
        CHECK(sprite.finished());
    }
}

TEST_CASE_FIXTURE(Fixture, "ping-pong turns round at the ends instead of wrapping") {
    sprite.play("swing"); // frames 0..3, ping-pong
    CHECK(sprite.frame_index() == 0);
    step(sprite, 0.100f); // frame 0 lasts 100
    CHECK(sprite.frame_index() == 1);
    step(sprite, 0.200f); // frame 1 lasts 200
    CHECK(sprite.frame_index() == 2);
    step(sprite, 0.100f);
    CHECK(sprite.frame_index() == 3);
    step(sprite, 0.050f); // the end: turn round rather than wrap to 0
    CHECK(sprite.frame_index() == 2);
    step(sprite, 0.100f);
    CHECK(sprite.frame_index() == 1);
}

TEST_CASE_FIXTURE(Fixture, "speed scales the clock, and zero freezes it") {
    SUBCASE("twice as fast covers twice as much") {
        sprite.play("walk");
        sprite.speed = 2;
        step(sprite, 0.050f); // half of frame 0's 100 ms, doubled
        CHECK(sprite.frame_index() == 1);
    }
    SUBCASE("zero is frozen, and not slow") {
        sprite.play("walk");
        sprite.speed = 0;
        for (int i = 0; i < 100; ++i) step(sprite, 0.100f);
        CHECK(sprite.frame_index() == 0);
    }
    SUBCASE("and one is the file's own timing") {
        sprite.play("walk");
        sprite.speed = 1;
        step(sprite, 0.100f);
        CHECK(sprite.frame_index() == 1);
    }
}

TEST_CASE_FIXTURE(Fixture, "a tag that is not there leaves the frame alone") {
    sprite.play("walk");
    step(sprite, 0.100f);
    REQUIRE(sprite.frame_index() == 1);

    sprite.play("nonexistent");
    CHECK(sprite.frame_index() == 1); // unchanged, not blank
    CHECK(std::string(sprite.playing()) == "walk");

    SUBCASE("and it keeps animating the one it was on") {
        step(sprite, 0.200f);
        CHECK(sprite.frame_index() == 0);
    }
    SUBCASE("and asking again does not warn again") {
        // report_once is what stops sixty identical warnings a second burying
        // whatever else the log was going to say: one line per tag name.
        const int said = rmp::detail::report_count();
        sprite.play("nonexistent");
        sprite.play("nonexistent");
        CHECK(rmp::detail::report_count() == said);
        sprite.play("also_nonexistent"); // a different typo is a different line
        CHECK(rmp::detail::report_count() == said + 1);
    }
}

TEST_CASE_FIXTURE(Fixture, "playing the same tag again does not restart it") {
    sprite.play("walk");
    step(sprite, 0.100f);
    REQUIRE(sprite.frame_index() == 1);
    // Called every frame from an _update, which is how a game writes it:
    // `sprite.play(moving ? "walk" : "idle")`. Restarting on every call would
    // freeze the animation on its first frame.
    sprite.play("walk");
    CHECK(sprite.frame_index() == 1);
}

TEST_CASE_FIXTURE(Fixture, "stop and set_frame are the escape hatch") {
    sprite.play("walk");
    sprite.stop();
    CHECK(sprite.finished());
    step(sprite, 1.0f);
    CHECK(sprite.frame_index() == 0);

    sprite.set_frame(3);
    CHECK(sprite.frame_index() == 3);
    CHECK_FALSE(sprite.finished()); // set_frame un-stops it
}

TEST_CASE("a sprite with no sheet at all does nothing rather than crashing") {
    rmp::Sprite sprite;
    sprite.play("walk");
    step(sprite, 1.0f);
    CHECK(sprite.frame_index() == 0);
    CHECK(std::string(sprite.playing()).empty());
}

// ---------------------------------------------------------------------------
// A negative speed, which rmp/object.h has always documented as `-1 = backwards`
// ---------------------------------------------------------------------------

namespace {

// A sheet built in memory rather than parsed from the fixture. Every tag in
// anim.aseprite is TWO frames long, and over two frames forwards and backwards
// land on the same frame every single step -- so a test written against it
// passes whichever direction the clock runs, which is not a test. Three is the
// smallest number of frames that can tell them apart.
struct ThreeFrames {
    rmp::Sprite sprite;

    explicit ThreeFrames(bool ping_pong = false) {
        rmp::SheetData data{};
        data.width = 4;
        data.height = 4;
        data.frames.resize(3);
        for (int i = 0; i < 3; ++i) {
            data.frames[static_cast<std::size_t>(i)].source =
                Rectangle{ static_cast<float>(i * 4), 0, 4, 4 };
            data.frames[static_cast<std::size_t>(i)].seconds = 0.100f;
        }
        data.tags.resize(1);
        data.tags[0].name = "run";
        data.tags[0].from = 0;
        data.tags[0].to = 2;
        data.tags[0].ping_pong = ping_pong;
        // The slot owns the sheet, tables and all, from here.
        auto *slot =
            rmp::detail::adopt(rmp::detail::ResourceKind::SHEET, std::move(data));
        REQUIRE(slot != nullptr);
        sprite.sheet = rmp::SpriteSheet{ slot };
    }
    ~ThreeFrames() {
        sprite.sheet = rmp::SpriteSheet{};
        rmp::detail::release_all();
    }
    ThreeFrames(const ThreeFrames &) = delete;
    ThreeFrames &operator=(const ThreeFrames &) = delete;
};

} // namespace

TEST_CASE("a negative speed plays the tag backwards") {
    // rmp/object.h has said `2 = twice as fast, 0 = frozen, -1 = backwards`
    // since the field existed, and the clock took the absolute value -- so -1
    // played FORWARDS at normal speed, the one setting of the three that did
    // not do what it said.
    ThreeFrames sheet;
    rmp::Sprite &sprite = sheet.sprite;
    sprite.play("run", true); // frames 0, 1, 2 at 100 ms each
    sprite.speed = -1;
    REQUIRE(sprite.frame_index() == 0);

    // Off the front of the tag, so a LOOPING one comes round to its last frame.
    step(sprite, 0.100f);
    CHECK(sprite.frame_index() == 2);
    step(sprite, 0.100f);
    CHECK(sprite.frame_index() == 1);
    step(sprite, 0.100f);
    CHECK(sprite.frame_index() == 0);
    CHECK_FALSE(sprite.finished());

    SUBCASE("and the magnitude still scales the clock") {
        sprite.set_frame(2);
        sprite.speed = -2;
        step(sprite, 0.050f); // half of a 100 ms frame, doubled
        CHECK(sprite.frame_index() == 1);
    }
}

TEST_CASE("backwards and not looping finishes ON the first frame") {
    // The mirror of the forward rule, and for the same reason: it holds the
    // frame it ran out at rather than falling off the front onto a blank one.
    ThreeFrames sheet;
    rmp::Sprite &sprite = sheet.sprite;
    sprite.play("run", false);
    sprite.speed = -1;
    sprite.set_frame(2);

    step(sprite, 0.100f);
    CHECK(sprite.frame_index() == 1);
    CHECK_FALSE(sprite.finished());

    step(sprite, 0.100f);
    CHECK(sprite.frame_index() == 0);
    CHECK_FALSE(sprite.finished());

    step(sprite, 0.100f);
    CHECK(sprite.frame_index() == 0); // held, not advanced off the front
    CHECK(sprite.finished());

    SUBCASE("and it stays there however long you wait") {
        for (int i = 0; i < 100; ++i) step(sprite, 0.100f);
        CHECK(sprite.frame_index() == 0);
        CHECK(sprite.finished());
    }
}

TEST_CASE("a negative speed turns a ping-pong round the other way first") {
    ThreeFrames sheet(true);
    rmp::Sprite &sprite = sheet.sprite;
    sprite.play("run");
    sprite.speed = -1;
    sprite.set_frame(1); // in the middle, where the two directions differ

    step(sprite, 0.100f);
    CHECK(sprite.frame_index() == 0);
    step(sprite, 0.100f); // the front: turn round rather than wrap
    CHECK(sprite.frame_index() == 1);
    step(sprite, 0.100f);
    CHECK(sprite.frame_index() == 2);
}

// ===========================================================================
// Files broken past the header.
//
// src/rmp/aseprite.cpp walks every chunk of a .aseprite before cute_aseprite
// reads it, and refuses what cute_aseprite would assert on, read or write
// outside what it owns for, go round forever on or leak. Before that walk, a
// file that passed the 128-byte header check went straight to cute_aseprite,
// and every case below that is refused was an abort, an out-of-bounds access,
// a hang or a leak -- tests/fuzz/aseprite.cpp found 315 such files in its first
// 90 seconds (tests/fuzz/regressions/aseprite/ keeps one of each kind). The
// files are written byte by byte by tests/aseprite_builder.h.
// ===========================================================================

namespace {

namespace ase = rmp::test::aseprite;

// RGBA, red in the low byte: the order a file stores a pixel's bytes in.
constexpr std::uint32_t RGBA_RED = 0xFF0000FFU;
constexpr std::uint32_t RGBA_GREEN = 0xFF00FF00U;

// Two 4x4 RGBA frames of one layer, a raw cel each, and one tag: the file the
// cases below break, one thing at a time.
ase::File two_frames() {
    ase::File file;
    file.frames.resize(2);
    file.frames[0].chunks = { ase::layer(), ase::tags({ { 0, 1, 0, "walk" } }),
                              ase::raw_cel(0, 0, 0, 4, 4, ase::fill(16, 4, RGBA_RED)) };
    file.frames[1].chunks = { ase::raw_cel(0, 0, 0, 4, 4, ase::fill(16, 4, RGBA_GREEN)) };
    return file;
}

bool reads(const std::vector<unsigned char> &bytes, rmp::SheetData &out) {
    return rmp::animation::detail::parse_sheet(bytes.data(),
                                               static_cast<int>(bytes.size()), &out);
}

bool reads(const std::vector<unsigned char> &bytes) {
    rmp::SheetData sheet;
    return reads(bytes, sheet);
}

// Why the walk refuses `bytes`, or "" when it does not.
std::string refused(const std::vector<unsigned char> &bytes) {
    std::vector<unsigned char> copy;
    return std::string(rmp::animation::detail::prepare_aseprite(bytes, copy));
}

// What the walk hands cute_aseprite.
std::vector<unsigned char> prepared(const std::vector<unsigned char> &bytes) {
    std::vector<unsigned char> copy;
    const std::string_view why = rmp::animation::detail::prepare_aseprite(bytes, copy);
    REQUIRE_MESSAGE(why.empty(), "refused: " << why);
    return copy;
}

// Every frame's pixels, RGBA, as cute_aseprite composes them from what the
// walk hands it -- the pictures upload_sheet() puts in the texture.
std::vector<std::vector<unsigned char>>
pictures(const std::vector<unsigned char> &bytes) {
    const std::vector<unsigned char> copy = prepared(bytes);
    ase_t *read = cute_aseprite_load_from_memory(copy.data(),
                                                 static_cast<int>(copy.size()), nullptr);
    REQUIRE(read != nullptr);
    std::vector<std::vector<unsigned char>> out;
    for (int f = 0; f < read->frame_count; ++f) {
        std::vector<unsigned char> frame;
        for (int i = 0; i < read->w * read->h; ++i) {
            const ase_color_t &c = read->frames[f].pixels[i];
            frame.insert(frame.end(), { c.r, c.g, c.b, c.a });
        }
        out.push_back(std::move(frame));
    }
    cute_aseprite_free(read);
    return out;
}

// A little-endian field written over the file at `at`.
void poke16(std::vector<unsigned char> &bytes, std::size_t at, unsigned value) {
    bytes[at] = static_cast<unsigned char>(value & 0xFFU);
    bytes[at + 1] = static_cast<unsigned char>((value >> 8) & 0xFFU);
}

void poke32(std::vector<unsigned char> &bytes, std::size_t at, std::uint32_t value) {
    poke16(bytes, at, value & 0xFFFFU);
    poke16(bytes, at + 2, value >> 16);
}

// Where the first frame's first chunk starts: after the file's header and the
// frame's.
constexpr std::size_t FIRST_CHUNK = 128 + 16;

} // namespace

TEST_CASE("the file every broken case starts from is read, pixels and all") {
    const ase::File file = two_frames();
    rmp::SheetData sheet;
    REQUIRE(reads(file.bytes(), sheet));
    CHECK(sheet.frame_count() == 2);
    CHECK(sheet.tag_count() == 1);
    const auto frames = pictures(file.bytes());
    REQUIRE(frames.size() == 2);
    CHECK(frames[0] == ase::fill(16, 4, RGBA_RED));
    CHECK(frames[1] == ase::fill(16, 4, RGBA_GREEN));
}

TEST_CASE("the sheets the framework ships are handed to cute_aseprite as they are") {
    // Byte for byte: nothing in them is taken out, and so nothing the walk
    // does can move a pixel of the platformer, whose e2e script compares its
    // frames exactly (tests/e2e/games/07_platformer.e2e).
    const std::string platformer =
        std::string(RMP_TEST_FIXTURES) + "../../examples/games/07_platformer/resources/";
    for (const std::string &path :
         { std::string(RMP_TEST_FIXTURES) + "anim.aseprite", platformer + "bat.aseprite",
           platformer + "coin.aseprite", platformer + "flag.aseprite",
           platformer + "player.aseprite", platformer + "walker.aseprite" }) {
        CAPTURE(path);
        std::ifstream file(path, std::ios::binary);
        REQUIRE(file.good());
        const std::vector<unsigned char> bytes{ std::istreambuf_iterator<char>(file),
                                                std::istreambuf_iterator<char>() };
        CHECK(prepared(bytes) == bytes);
    }
}

TEST_CASE("upload_sheet draws only the sheet its bytes were read into") {
    // No GPU here, so every answer is the empty texture -- what matters is
    // that none of them reads past what cute_aseprite made: a sheet of one
    // frame more than the file read ase->frames[2] of two, under ASan.
    const std::vector<unsigned char> bytes = two_frames().bytes();
    const int size = static_cast<int>(bytes.size());
    rmp::SheetData sheet;
    REQUIRE(reads(bytes, sheet));
    CHECK(rmp::animation::detail::upload_sheet(bytes.data(), size, sheet).id == 0);

    rmp::SheetData more = sheet;
    more.frames.push_back(more.frames.back());
    CHECK(rmp::animation::detail::upload_sheet(bytes.data(), size, more).id == 0);
    rmp::SheetData wider = sheet;
    wider.width = 8;
    CHECK(rmp::animation::detail::upload_sheet(bytes.data(), size, wider).id == 0);

    std::vector<unsigned char> broken = bytes;
    poke16(broken, 128 + 4, 0);
    CHECK(rmp::animation::detail::upload_sheet(broken.data(), size, sheet).id == 0);
    CHECK(rmp::animation::detail::upload_sheet(nullptr, size, sheet).id == 0);
    CHECK(rmp::animation::detail::upload_sheet(bytes.data(), 0, sheet).id == 0);
    CHECK(rmp::animation::detail::upload_sheet(bytes.data(), size, rmp::SheetData{}).id ==
          0);
}

TEST_CASE("the header is checked as it always was") {
    std::vector<unsigned char> bytes = two_frames().bytes();
    SUBCASE("the magic number") {
        poke16(bytes, 4, 0xA5E1);
        CHECK(refused(bytes) == "not an Aseprite file");
    }
    SUBCASE("the size it says") {
        bytes.push_back(0);
        CHECK(refused(bytes) == "a file that is not the size it says");
    }
    SUBCASE("no frames, no width, no height") {
        for (const std::size_t at : { 6U, 8U, 10U }) {
            std::vector<unsigned char> broken = bytes;
            poke16(broken, at, 0);
            CHECK(refused(broken) == "a sheet of no frames or no size");
        }
        poke16(bytes, 6, 0);
    }
    SUBCASE("a colour depth that is not 8, 16 or 32") {
        poke16(bytes, 12, 24);
        CHECK(refused(bytes) == "a colour depth Aseprite does not write");
    }
    SUBCASE("less than a header") {
        bytes.resize(127);
        CHECK(refused(bytes) == "not an Aseprite file");
    }
    CHECK_FALSE(reads(bytes));
}

TEST_CASE("a frame without its magic number is refused") {
    // cute_aseprite asserted: `magic == 0xF1FA`.
    std::vector<unsigned char> bytes = two_frames().bytes();
    poke16(bytes, 128 + 4, 0xF1FB);
    CHECK(refused(bytes) == "a frame without its magic number");
    CHECK_FALSE(reads(bytes));
}

TEST_CASE("a file cut short, or a chunk that is not the size it says, is refused") {
    const std::vector<unsigned char> whole = two_frames().bytes();
    SUBCASE("cut inside the last chunk, the size it says made to match") {
        // cute_aseprite read on past the end of the file.
        std::vector<unsigned char> bytes(whole.begin(), whole.end() - 10);
        poke32(bytes, 0, static_cast<std::uint32_t>(bytes.size()));
        CHECK(refused(bytes) == "a chunk that is not the size it says");
    }
    SUBCASE("cut between frames") {
        const ase::File one = [] {
            ase::File file = two_frames();
            file.frames.pop_back();
            return file;
        }();
        std::vector<unsigned char> bytes = one.bytes();
        poke16(bytes, 6, 2); // two frames, and one in the file
        CHECK(refused(bytes) == "a file cut short");
    }
    SUBCASE("a chunk longer than the file") {
        std::vector<unsigned char> bytes = whole;
        poke32(bytes, FIRST_CHUNK, 100000);
        CHECK(refused(bytes) == "a chunk that is not the size it says");
    }
    SUBCASE("a chunk shorter than its own six bytes") {
        // 3 - 6 wrapped round in cute_aseprite's unsigned arithmetic, to a
        // skip of four billion bytes.
        std::vector<unsigned char> bytes = whole;
        poke32(bytes, FIRST_CHUNK, 3);
        CHECK(refused(bytes) == "a chunk that is not the size it says");
    }
    SUBCASE("a frame that counts more chunks than it has") {
        std::vector<unsigned char> bytes = whole;
        poke32(bytes, 128 + 12, 40);
        CHECK_FALSE(refused(bytes).empty());
    }
    SUBCASE("a layer chunk one byte longer than what it holds") {
        // cute_aseprite asserted: `size_read == chunk_size`.
        ase::File file = two_frames();
        file.frames[0].chunks[0].body.push_back(0);
        CHECK(refused(file.bytes()) == "a chunk that is not the size it says");
    }
}

TEST_CASE("a raw cel with more pixels than its chunk holds is refused") {
    // cute_aseprite copied w x h pixels from wherever the chunk started,
    // whatever the chunk held: past it, and past the end of the file.
    ase::File file = two_frames();
    file.frames[1].chunks[0] =
        ase::raw_cel(0, 0, 0, 40, 40, ase::fill(16, 4, RGBA_GREEN));
    CHECK(refused(file.bytes()) == "a chunk that is not the size it says");
    CHECK_FALSE(reads(file.bytes()));
}

TEST_CASE("a cel of no size is refused") {
    // A buffer of 0 bytes, which malloc may hand back as null, copied into.
    ase::File file = two_frames();
    file.frames[1].chunks[0] = ase::raw_cel(0, 0, 0, 0, 4, {});
    CHECK(refused(file.bytes()) == "a cel of no size");
}

TEST_CASE("a cel of a layer the file does not have is refused") {
    // cute_aseprite took layers[index] for any index of 65 536, and its
    // table has 64.
    for (const int layer : { 1, 63, 64, 9728, 65535 }) {
        CAPTURE(layer);
        ase::File file = two_frames();
        file.frames[1].chunks[0] =
            ase::raw_cel(layer, 0, 0, 4, 4, ase::fill(16, 4, RGBA_GREEN));
        CHECK(refused(file.bytes()) == "a cel of a layer the file does not have");
    }
}

TEST_CASE("a layer is nested under a layer that is there, or refused") {
    // cute_aseprite wrote the layer into layer_stack[level] -- 64 of them, on
    // its stack -- and took its parent from the slot a level up, written or
    // not.
    for (const unsigned level : { 1U, 5U, 63U, 64U, 768U, 65535U }) {
        CAPTURE(level);
        ase::File file = two_frames();
        file.frames[0].chunks[0] =
            ase::layer({ .level = static_cast<std::uint16_t>(level) });
        CHECK(refused(file.bytes()) == "a layer nested under no layer");
    }
    SUBCASE("and a group's children, one level down, are read") {
        ase::File file = two_frames();
        file.frames[0].chunks.insert(file.frames[0].chunks.begin(),
                                     ase::layer({ .type = 1, .name = "group" }));
        file.frames[0].chunks[1] = ase::layer({ .level = 1, .name = "child" });
        // The cels are on layer 1 now.
        file.frames[0].chunks[3] =
            ase::raw_cel(1, 0, 0, 4, 4, ase::fill(16, 4, RGBA_RED));
        file.frames[1].chunks[0] =
            ase::raw_cel(1, 0, 0, 4, 4, ase::fill(16, 4, RGBA_GREEN));
        CHECK(refused(file.bytes()).empty());
        CHECK(pictures(file.bytes())[1] == ase::fill(16, 4, RGBA_GREEN));
    }
}

TEST_CASE("64 layers and 64 cels a frame are read, and a 65th is refused") {
    // cute_aseprite asserted on the 65th of either.
    ase::File file = two_frames();
    auto &chunks = file.frames[0].chunks;
    for (int i = 1; i < 64; ++i) chunks.insert(chunks.begin(), ase::layer());
    // Layer 63 is the last layer of 64; frame 1 gets 64 cels.
    file.frames[1].chunks.clear();
    for (int i = 0; i < 64; ++i) {
        file.frames[1].chunks.push_back(
            ase::raw_cel(i, 0, 0, 4, 4, ase::fill(16, 4, RGBA_GREEN)));
    }
    CHECK(refused(file.bytes()).empty());

    SUBCASE("a 65th layer") {
        chunks.insert(chunks.begin(), ase::layer());
        CHECK(refused(file.bytes()) == "more than 64 layers");
    }
    SUBCASE("a 65th cel") {
        file.frames[1].chunks.push_back(
            ase::raw_cel(0, 0, 0, 4, 4, ase::fill(16, 4, RGBA_RED)));
        CHECK(refused(file.bytes()) == "more than 64 cels in a frame");
    }
}

TEST_CASE("layer flags and layer kinds Aseprite does not write are refused") {
    SUBCASE("flags past the reference layer's") {
        // Outside cute_aseprite's enum: loading one is undefined behaviour,
        // which UBSan reported.
        ase::File file = two_frames();
        file.frames[0].chunks[0] = ase::layer({ .flags = 0x80 });
        CHECK(refused(file.bytes()) == "layer flags Aseprite does not write");
        file.frames[0].chunks[0] = ase::layer({ .flags = 0x7F });
        CHECK(refused(file.bytes()).empty());
    }
    SUBCASE("a tilemap layer, which has a tileset after its name") {
        ase::File file = two_frames();
        ase::Chunk tilemap = ase::layer({ .type = 2 });
        tilemap.body.insert(tilemap.body.end(), 4, 0);
        file.frames[0].chunks[0] = tilemap;
        CHECK(refused(file.bytes()) ==
              "a tilemap layer, which cute_aseprite cannot read");
    }
    SUBCASE("a kind past the tilemap") {
        ase::File file = two_frames();
        file.frames[0].chunks[0] = ase::layer({ .type = 3 });
        CHECK(refused(file.bytes()) == "a layer of a kind Aseprite does not write");
    }
    SUBCASE("layer UUIDs, sixteen bytes after the name") {
        ase::File file = two_frames();
        file.flags |= 4;
        file.frames[0].chunks[0].body.insert(file.frames[0].chunks[0].body.end(), 16, 7);
        CHECK(refused(file.bytes()) == "layer UUIDs, which cute_aseprite cannot read");
    }
}

TEST_CASE("cels of a kind cute_aseprite cannot read are refused") {
    ase::File file = two_frames();
    ase::Bytes tilemap = ase::cel_head(0, 0, 0, 3);
    tilemap.u16(1).u16(1).u16(32).u32(0).u32(0).u32(0).u32(0).zeros(10).raw(
        ase::zlib(ase::fill(1, 4, 0), ase::Deflate::FIXED));
    file.frames[1].chunks[0] = { ase::CEL, tilemap.data };
    CHECK(refused(file.bytes()) == "a tilemap cel, which cute_aseprite cannot read");

    file.frames[1].chunks[0] = { ase::CEL, ase::cel_head(0, 0, 0, 4).data };
    CHECK(refused(file.bytes()) == "a cel of a kind Aseprite does not write");
}

TEST_CASE("linked cels: a link is followed to its frame, and a broken one refused") {
    ase::File file = two_frames();
    file.frames.push_back({ 100, { ase::linked_cel(0, 0) } });
    REQUIRE(refused(file.bytes()).empty());
    CHECK(pictures(file.bytes())[2] == ase::fill(16, 4, RGBA_RED));

    SUBCASE("to a frame the file does not have") {
        file.frames[2].chunks[0] = ase::linked_cel(0, 3);
        CHECK(refused(file.bytes()) == "a linked cel to a frame the file does not have");
        file.frames[2].chunks[0] = ase::linked_cel(0, 65535);
        CHECK(refused(file.bytes()) == "a linked cel to a frame the file does not have");
    }
    SUBCASE("to a frame with nothing on its layer") {
        // cute_aseprite asserted that it found one.
        file.frames[0].chunks.insert(file.frames[0].chunks.begin(), ase::layer());
        file.frames[2].chunks[0] = ase::linked_cel(1, 0);
        CHECK(refused(file.bytes()) ==
              "a linked cel to a frame with nothing on its layer");
    }
    SUBCASE("to itself") {
        // cute_aseprite followed it forever.
        file.frames[2].chunks[0] = ase::linked_cel(0, 2);
        CHECK(refused(file.bytes()) == "linked cels that link round in a circle");
    }
    SUBCASE("round a circle of three") {
        file.frames[0].chunks[2] = ase::linked_cel(0, 1);
        file.frames[1].chunks[0] = ase::linked_cel(0, 2);
        file.frames[2].chunks[0] = ase::linked_cel(0, 0);
        CHECK(refused(file.bytes()) == "linked cels that link round in a circle");
    }
    SUBCASE("along a chain, to the cel at its end") {
        file.frames[1].chunks[0] = ase::linked_cel(0, 0);
        file.frames[2].chunks[0] = ase::linked_cel(0, 1);
        CHECK(refused(file.bytes()).empty());
        CHECK(pictures(file.bytes())[2] == ase::fill(16, 4, RGBA_RED));
    }
    SUBCASE("along two chains that meet: each followed once") {
        file.frames[1].chunks[0] = ase::linked_cel(0, 0);
        file.frames[2].chunks[0] = ase::linked_cel(0, 1);
        file.frames.push_back({ 100, { ase::linked_cel(0, 1) } });
        file.frames.push_back({ 100, { ase::linked_cel(0, 2) } });
        CHECK(refused(file.bytes()).empty());
        for (const auto &frame : pictures(file.bytes())) {
            CHECK(frame == ase::fill(16, 4, RGBA_RED));
        }
    }
}

TEST_CASE("a tag is inside the frames, and its direction one Aseprite writes") {
    SUBCASE("a tag past the last frame") {
        // Read as it stood: play() started on frame 5 of 2, a blank.
        ase::File file = two_frames();
        file.frames[0].chunks[1] = ase::tags({ { 0, 5, 0, "walk" } });
        CHECK(refused(file.bytes()) == "a tag outside the frames");
    }
    SUBCASE("a tag that ends before it starts") {
        ase::File file = two_frames();
        file.frames[0].chunks[1] = ase::tags({ { 1, 0, 0, "walk" } });
        CHECK(refused(file.bytes()) == "a tag outside the frames");
    }
    SUBCASE("a direction past ping-pong reverse") {
        // Outside cute_aseprite's enum: UBSan reported the load.
        for (const int direction : { 4, 7, 255 }) {
            ase::File file = two_frames();
            file.frames[0].chunks[1] = ase::tags({ { 0, 1, direction, "walk" } });
            CHECK(refused(file.bytes()) == "a tag direction Aseprite does not write");
        }
    }
    SUBCASE("255 tags, and not 256") {
        // cute_aseprite asserted at 256.
        std::vector<ase::Tag> many(255, { 0, 1, 0, "t" });
        ase::File file = two_frames();
        file.frames[0].chunks[1] = ase::tags(many);
        CHECK(refused(file.bytes()).empty());
        many.push_back({ 0, 1, 0, "t" });
        file.frames[0].chunks[1] = ase::tags(many);
        CHECK(refused(file.bytes()) == "more than 255 tags");
    }
    SUBCASE("two tags chunks") {
        // cute_aseprite kept the second and lost the names of the first.
        ase::File file = two_frames();
        file.frames[1].chunks.push_back(ase::tags({ { 0, 0, 0, "again" } }));
        CHECK(refused(file.bytes()) == "two tags chunks");
    }
}

TEST_CASE("ping-pong reverse, Aseprite 1.3's fourth direction, is read as both") {
    // Read as forward before: cute_aseprite's enum has no name for 3, and the
    // parse matched the names it has.
    ase::File file = two_frames();
    file.frames[0].chunks[1] = ase::tags({ { 0, 1, 3, "bounce" }, { 0, 1, 1, "back" } });
    rmp::SheetData sheet;
    REQUIRE(reads(file.bytes(), sheet));
    CHECK(sheet.tag(0).ping_pong);
    CHECK(sheet.tag(0).reverse);
    CHECK_FALSE(sheet.tag(1).ping_pong);
    CHECK(sheet.tag(1).reverse);
}

TEST_CASE("a sheet past the budgets is refused before anything is allocated") {
    SUBCASE("a canvas of 65 535 x 65 535") {
        // 4 x w x h overflowed cute_aseprite's int: UB, then a malloc of
        // whatever it came to.
        ase::File file = two_frames();
        file.width = 65535;
        file.height = 65535;
        CHECK(refused(file.bytes()) == "bigger than a sheet may be");
    }
    SUBCASE("frames whose pictures add up past it") {
        ase::File file;
        file.width = 2048;
        file.height = 2048;
        file.frames.resize(17, { 100, {} }); // 16 MB a frame
        file.frames[0].chunks = { ase::layer() };
        CHECK(refused(file.bytes()) == "bigger than a sheet may be");
        file.frames.resize(15);
        CHECK(refused(file.bytes()).empty());
    }
    SUBCASE("a compressed cel that would make more pixels than that") {
        ase::File file = two_frames();
        file.frames[1].chunks[0] = ase::compressed_cel(
            0, 0, 0, 65535, 65535, ase::zlib({}, ase::Deflate::FIXED));
        CHECK(refused(file.bytes()) == "bigger than a sheet may be");
    }
    SUBCASE("cels whose blending adds up past it") {
        // A 256 x 256 cel on each of 64 layers, and 16 more frames that link
        // all 64: 68M pixels blended out of a file of 150 KB.
        ase::File file;
        file.width = 256;
        file.height = 256;
        file.frames.resize(17, { 100, {} });
        const std::vector<unsigned char> stream =
            ase::zlib(ase::fill(256 * 256, 4, RGBA_RED), ase::Deflate::FIXED);
        for (int l = 0; l < 64; ++l) file.frames[0].chunks.push_back(ase::layer());
        for (int l = 0; l < 64; ++l) {
            file.frames[0].chunks.push_back(
                ase::compressed_cel(l, 0, 0, 256, 256, stream));
        }
        for (std::size_t f = 1; f < file.frames.size(); ++f) {
            for (int l = 0; l < 64; ++l)
                file.frames[f].chunks.push_back(ase::linked_cel(l, 0));
        }
        CHECK(refused(file.bytes()) == "bigger than a sheet may be");
        file.frames.resize(15);
        CHECK(refused(file.bytes()).empty());
    }
}

// ---------------------------------------------------------------------------
// Compressed cels: the deflate stream decoded the way cute_aseprite decodes
// it, before cute_aseprite does -- every code, every length, every distance.
// ---------------------------------------------------------------------------

namespace {

// two_frames(), with frame 1's cel a compressed one of 4x4 pixels.
ase::File compressed(const std::vector<unsigned char> &stream, int w = 4, int h = 4) {
    ase::File file = two_frames();
    file.frames[1].chunks[0] = ase::compressed_cel(0, 0, 0, w, h, stream);
    return file;
}

// Fixed codes, every pixel a literal: 4x4 RGBA in 64 literals.
ase::Stream literals_of(const std::vector<unsigned char> &pixels) {
    ase::Stream stream;
    stream.fixed_block(true);
    stream.literals(pixels);
    return stream;
}

// The literal/length and distance code lengths of a dynamic block that can
// write anything: 9 bits a byte, 2 for the end, 9 a length, 5 a distance.
std::vector<int> any_literal() {
    std::vector<int> lengths(286, 9);
    lengths[256] = 2;
    return lengths;
}
std::vector<int> any_distance() { return std::vector<int>(30, 5); }

} // namespace

TEST_CASE("a compressed cel is read: fixed codes, dynamic codes, runs") {
    const std::vector<unsigned char> green = ase::fill(16, 4, RGBA_GREEN);
    SUBCASE("fixed codes, with runs one pixel back") {
        const ase::File file = compressed(ase::zlib(green, ase::Deflate::FIXED));
        CHECK(pictures(file.bytes())[1] == green);
    }
    SUBCASE("dynamic codes") {
        ase::Stream stream;
        stream.dynamic_block(true, any_literal(), any_distance());
        stream.literals(std::span(green).first(4));
        stream.match(60, 4);
        stream.end();
        CHECK(pictures(compressed(stream.zlib()).bytes())[1] == green);
    }
    SUBCASE("raylib's sdefl, on a cel big enough for dynamic codes to pay") {
        const std::vector<unsigned char> stripes = [] {
            std::vector<unsigned char> out;
            for (int i = 0; i < 64 * 64; ++i) {
                const std::vector<unsigned char> px =
                    ase::fill(1, 4, (i / 7) % 3 == 0 ? RGBA_RED : RGBA_GREEN);
                out.insert(out.end(), px.begin(), px.end());
            }
            return out;
        }();
        const std::vector<unsigned char> stream =
            ase::zlib(stripes, ase::Deflate::DYNAMIC);
        REQUIRE(ase::first_block_type(stream) == 2);
        ase::File file = compressed(stream, 64, 64);
        file.width = 64;
        file.height = 64;
        file.frames[0].chunks[2] = ase::raw_cel(0, 0, 0, 1, 1, ase::fill(1, 4, RGBA_RED));
        CHECK(pictures(file.bytes())[1] == stripes);
    }
    SUBCASE("two blocks, the first not the last") {
        ase::Stream stream;
        stream.fixed_block(false);
        stream.literals(std::span(green).first(32));
        stream.end();
        stream.dynamic_block(true, any_literal(), any_distance());
        stream.literals(std::span(green).subspan(32));
        stream.end();
        CHECK(pictures(compressed(stream.zlib()).bytes())[1] == green);
    }
}

TEST_CASE("a compressed cel that is not zlib's deflate is refused") {
    // cute_aseprite asserted on each.
    std::vector<unsigned char> stream =
        ase::zlib(ase::fill(16, 4, RGBA_GREEN), ase::Deflate::FIXED);
    for (const unsigned method : { 0x79U, 0x88U, 0x07U }) {
        stream[0] = static_cast<unsigned char>(method);
        CHECK(refused(compressed(stream).bytes()) ==
              "a compressed cel that is not zlib's deflate");
    }
    stream[0] = 0x78;
    stream[1] = 0x20 | 0x9C; // a preset dictionary
    CHECK(refused(compressed(stream).bytes()) ==
          "a compressed cel that is not zlib's deflate");
}

TEST_CASE("a stored deflate block is refused: cute_aseprite cannot read one") {
    // Its own check of the block's length counts the Adler-32 after it and
    // always fails: the cel was read as uninitialised memory, every time.
    const std::vector<unsigned char> stream =
        ase::zlib(ase::fill(16, 4, RGBA_GREEN), ase::Deflate::STORED);
    CHECK(refused(compressed(stream).bytes()) ==
          "a stored deflate block, which cute_aseprite cannot read");
    CHECK_FALSE(reads(compressed(stream).bytes()));
}

TEST_CASE("a compressed cel must decompress to exactly its pixels") {
    const std::vector<unsigned char> green = ase::fill(16, 4, RGBA_GREEN);
    const std::string broken = "a compressed cel that does not decompress to its pixels";
    SUBCASE("one byte short") {
        ase::Stream stream =
            literals_of(std::vector<unsigned char>(green.begin(), green.end() - 1));
        stream.end();
        CHECK(refused(compressed(stream.zlib()).bytes()) == broken);
    }
    SUBCASE("one byte over") {
        ase::Stream stream = literals_of(green);
        stream.literals(std::array<unsigned char, 1>{ 9 });
        stream.end();
        CHECK(refused(compressed(stream.zlib()).bytes()) == broken);
    }
    SUBCASE("a run past the cel's end") {
        ase::Stream stream =
            literals_of(std::vector<unsigned char>(green.begin(), green.begin() + 4));
        stream.match(61, 4);
        stream.end();
        CHECK(refused(compressed(stream.zlib()).bytes()) == broken);
    }
    SUBCASE("a run from before the first byte") {
        ase::Stream stream =
            literals_of(std::vector<unsigned char>(green.begin(), green.begin() + 3));
        stream.match(61, 4);
        stream.end();
        CHECK(refused(compressed(stream.zlib()).bytes()) == broken);
    }
    SUBCASE("cut short: no end, no checksum") {
        // cute_aseprite asserted that there were bits left to read.
        std::vector<unsigned char> stream = ase::zlib(green, ase::Deflate::FIXED);
        for (std::size_t cut = 1; cut < stream.size() - 2; ++cut) {
            CAPTURE(cut);
            const std::vector<unsigned char> shorter(
                stream.begin(), stream.end() - static_cast<std::ptrdiff_t>(cut));
            CHECK(refused(compressed(shorter).bytes()) == broken);
        }
    }
    SUBCASE("the length symbols past 285 and the distances past 29") {
        // cute_aseprite read them as a length of 0, and a distance of 0 -- a
        // byte copied onto itself, never written.
        ase::Stream lengths =
            literals_of(std::vector<unsigned char>(green.begin(), green.begin() + 4));
        lengths.symbol(286);
        CHECK(refused(compressed(lengths.zlib()).bytes()) == broken);

        ase::Stream distances =
            literals_of(std::vector<unsigned char>(green.begin(), green.begin() + 4));
        distances.symbol(257); // a length of 3
        distances.bits(0x1F,
                       5); // distance code 31, the five bits written high first: all ones
        CHECK(refused(compressed(distances.zlib()).bytes()) == broken);
    }
    SUBCASE("a dynamic block's header cut short") {
        // One byte of data -- the last block, dynamic -- and the checksum.
        const std::vector<unsigned char> stream{ 0x78, 0x9C, 0x05, 0, 0, 0, 0 };
        CHECK(refused(compressed(stream).bytes()) == broken);
    }
    SUBCASE("a block type that does not exist") {
        ase::Stream stream;
        stream.bits(1, 1);
        stream.bits(3, 2);
        stream.bits(0, 32);
        CHECK(refused(compressed(stream.zlib()).bytes()) == broken);
    }
}

TEST_CASE("a dynamic block's header is held to the codes it spells") {
    const std::vector<unsigned char> green = ase::fill(16, 4, RGBA_GREEN);
    const std::string broken = "a compressed cel that does not decompress to its pixels";
    const std::vector<int> literal = any_literal();
    const std::vector<int> distance = any_distance();
    // The header as the code lengths would be spelled, one by one.
    std::vector<std::pair<int, int>> spelled;
    spelled.reserve(literal.size() + distance.size());
    for (const int length : literal) spelled.emplace_back(length, 0);
    for (const int length : distance) spelled.emplace_back(length, 0);

    SUBCASE("spelled one by one, it reads") {
        ase::Stream stream;
        stream.dynamic_block(true, literal, distance, spelled);
        stream.literals(green);
        stream.end();
        CHECK(pictures(compressed(stream.zlib()).bytes())[1] == green);
    }
    SUBCASE("spelled with repeats, it reads") {
        // 16 repeats the length before, 17 and 18 repeat zeros: no literal
        // from 100 to 199 is written, and the stream uses none.
        std::vector<int> sparse = literal;
        std::fill(sparse.begin() + 100, sparse.begin() + 200, 0);
        std::vector<std::pair<int, int>> repeats;
        const auto spell = [&repeats](const std::vector<int> &lengths) {
            std::size_t i = 0;
            while (i < lengths.size()) {
                std::size_t run = 1;
                while (i + run < lengths.size() && lengths[i + run] == lengths[i]) ++run;
                if (lengths[i] == 0 && run >= 11) {
                    const std::size_t take = std::min<std::size_t>(run, 138);
                    repeats.emplace_back(18, static_cast<int>(take - 11));
                    i += take;
                } else if (lengths[i] == 0 && run >= 3) {
                    const std::size_t take = std::min<std::size_t>(run, 10);
                    repeats.emplace_back(17, static_cast<int>(take - 3));
                    i += take;
                } else if (run >= 4) {
                    repeats.emplace_back(lengths[i], 0);
                    const std::size_t take = std::min<std::size_t>(run - 1, 6);
                    repeats.emplace_back(16, static_cast<int>(take - 3));
                    i += 1 + take;
                } else {
                    repeats.emplace_back(lengths[i], 0);
                    ++i;
                }
            }
        };
        // The literal and distance lengths spelled as one run, as they are
        // sent: a repeat may cross from one into the other.
        std::vector<int> both = sparse;
        both.insert(both.end(), distance.begin(), distance.end());
        spell(both);
        ase::Stream stream;
        stream.dynamic_block(true, sparse, distance, repeats);
        stream.literals(green);
        stream.end();
        CHECK(pictures(compressed(stream.zlib()).bytes())[1] == green);
    }
    SUBCASE("a repeat that runs past the last length") {
        // cute_aseprite wrote on past its 320 lengths, on its stack.
        std::vector<std::pair<int, int>> past = spelled;
        past.resize(past.size() - 5);
        past.emplace_back(18, 127); // 138 zeros
        ase::Stream stream;
        stream.dynamic_block(true, literal, distance, past);
        CHECK(refused(compressed(stream.zlib()).bytes()) == broken);
    }
    SUBCASE("a repeat of the length before the first") {
        // cute_aseprite read lengths[-1].
        std::vector<std::pair<int, int>> first = spelled;
        first[0] = { 16, 0 };
        first.erase(first.begin() + 1, first.begin() + 3);
        ase::Stream stream;
        stream.dynamic_block(true, literal, distance, first);
        CHECK(refused(compressed(stream.zlib()).bytes()) == broken);
    }
    SUBCASE("bits no code of the table starts with") {
        // Only literal codes 0..255 and the end, 9 and 2 bits: a code of
        // two ones followed by anything is in no table. cute_aseprite
        // asserted on it.
        std::vector<int> few(257, 9);
        few[256] = 2;
        ase::Stream stream;
        stream.dynamic_block(true, few, { 0 });
        stream.bits(0x7F, 7);
        CHECK(refused(compressed(stream.zlib()).bytes()) == broken);
    }
    SUBCASE("a distance from a table with no codes") {
        // cute_aseprite read the entry before the table.
        std::vector<int> few(258, 9);
        few[256] = 2;
        ase::Stream stream;
        stream.dynamic_block(true, few, { 0 });
        stream.literals(std::span(green).first(4));
        stream.symbol(257);
        stream.bits(0, 16);
        CHECK(refused(compressed(stream.zlib()).bytes()) == broken);
    }
}

// ---------------------------------------------------------------------------
// What the walk takes out of the copy it hands cute_aseprite: chunks no pixel
// depends on, which cute_aseprite asserts on, writes past its tables for or
// leaks -- and nothing the picture needs.
// ---------------------------------------------------------------------------

TEST_CASE("user data is not handed to cute_aseprite, and the sheet reads the same") {
    ase::File file = two_frames();
    SUBCASE("the sprite's own, before any layer, as Aseprite 1.3 writes it") {
        // cute_aseprite asserted: user data with nothing before it to own it.
        file.frames[0].chunks.insert(file.frames[0].chunks.begin(),
                                     ase::user_data("sprite"));
    }
    SUBCASE("a tag's, with text") {
        // cute_aseprite never freed a tag's text.
        file.frames[0].chunks.insert(file.frames[0].chunks.begin() + 2,
                                     ase::user_data("walk"));
    }
    SUBCASE("with properties, which cute_aseprite does not know") {
        ase::Chunk props = ase::user_data("");
        ase::Bytes more;
        more.u32(8).u32(0); // an empty map of properties
        props.body[0] |= 4;
        props.body.insert(props.body.end(), more.data.begin(), more.data.end());
        file.frames[0].chunks.insert(file.frames[0].chunks.begin() + 1, props);
    }
    CHECK(refused(file.bytes()).empty());
    CHECK(pictures(file.bytes()) == pictures(two_frames().bytes()));
}

TEST_CASE("slices, colour profiles and cel extras are not handed over either") {
    ase::File file = two_frames();
    SUBCASE("two slice chunks: cute_aseprite freed only the first's name") {
        file.frames[0].chunks.push_back(ase::slice("hitbox", 1));
        file.frames[1].chunks.push_back(ase::slice("hurtbox", 2));
    }
    SUBCASE("an ICC profile longer than its chunk: cute_aseprite copied on past it") {
        ase::Chunk icc = ase::icc_profile(64);
        icc.body.resize(icc.body.size() - 60);
        file.frames[0].chunks.push_back(icc);
    }
    SUBCASE("two ICC profiles: cute_aseprite leaked the first") {
        file.frames[0].chunks.push_back(ase::icc_profile(8));
        file.frames[1].chunks.push_back(ase::icc_profile(8));
    }
    SUBCASE("a cel extra after a frame's 64th cel: cute_aseprite wrote the 65th") {
        for (int i = 1; i < 64; ++i)
            file.frames[0].chunks.insert(file.frames[0].chunks.begin(), ase::layer());
        file.frames[1].chunks.clear();
        for (int i = 0; i < 64; ++i)
            file.frames[1].chunks.push_back(
                ase::raw_cel(i, 0, 0, 4, 4, ase::fill(16, 4, RGBA_GREEN)));
        file.frames[1].chunks.push_back(ase::cel_extra());
    }
    CHECK(refused(file.bytes()).empty());
    CHECK(pictures(file.bytes())[1] == ase::fill(16, 4, RGBA_GREEN));
}

TEST_CASE("a chunk cute_aseprite does not know is left for it to step over") {
    // External files, Aseprite 1.3: cute_aseprite skips it by its size.
    ase::File file = two_frames();
    ase::Bytes external;
    external.u32(1).zeros(8).u32(1).u8(0).zeros(7).string("palette.aseprite");
    file.frames[0].chunks.insert(file.frames[0].chunks.begin(),
                                 { ase::EXTERNAL_FILES, external.data });
    const std::vector<unsigned char> bytes = file.bytes();
    CHECK(prepared(bytes) == bytes);
    CHECK(pictures(bytes) == pictures(two_frames().bytes()));
}

TEST_CASE("blend modes are 0 in the copy: cute_aseprite draws them all as normal") {
    // And printed a warning to stdout for every layer of another mode.
    ase::File file = two_frames();
    file.frames[0].chunks[0] = ase::layer({ .blend = 3 });
    const std::vector<unsigned char> bytes = file.bytes();
    const std::vector<unsigned char> copy = prepared(bytes);
    REQUIRE(copy.size() == bytes.size());
    const std::size_t blend = FIRST_CHUNK + 6 + 10;
    CHECK(bytes[blend] == 3);
    CHECK(copy[blend] == 0);
    CHECK(pictures(bytes) == pictures(two_frames().bytes()));
}

TEST_CASE("an indexed sheet's palette is read, and its colours' names do not leak") {
    // Aseprite writes the new palette chunk, then -- for a palette of 256
    // colours or fewer with no alpha -- the old one, which wrote over the
    // new one's names in cute_aseprite: every name leaked.
    const std::vector<ase::PaletteEntry> colours{ { 0, 0, 0, 255, "Black" },
                                                  { 255, 0, 0, 255, "Red" },
                                                  { 0, 0, 255, 255, "" } };
    ase::File file;
    file.depth = 8;
    file.transparent = 0;
    file.frames.resize(1);
    const std::vector<unsigned char> indices{ 0, 1, 2, 1, 1, 2, 1, 0,
                                              2, 2, 2, 2, 0, 0, 1, 1 };
    file.frames[0].chunks = { ase::palette(3, 0, colours), ase::old_palette(0, colours),
                              ase::layer(), ase::raw_cel(0, 0, 0, 4, 4, indices) };
    REQUIRE(refused(file.bytes()).empty());
    const std::vector<unsigned char> picture = pictures(file.bytes())[0];
    for (std::size_t i = 0; i < indices.size(); ++i) {
        CAPTURE(i);
        const std::array<unsigned char, 4> expected = indices[i] == 0
            ? std::array<unsigned char, 4>{ 0, 0, 0, 0 }
            : indices[i] == 1 ? std::array<unsigned char, 4>{ 255, 0, 0, 255 }
                              : std::array<unsigned char, 4>{ 0, 0, 255, 255 };
        CHECK(std::equal(expected.begin(), expected.end(), picture.begin() + 4 * i));
    }

    SUBCASE("an old palette chunk alone, as Aseprite before 1.1 wrote it, is read") {
        file.frames[0].chunks.erase(file.frames[0].chunks.begin());
        CHECK(pictures(file.bytes())[0] == picture);
    }
    SUBCASE("an old palette chunk of a later frame over named colours is refused") {
        file.frames.push_back({ 100, { ase::old_palette(0, colours) } });
        CHECK(refused(file.bytes()) == "a named palette colour written over");
    }
    SUBCASE("a palette chunk written over named colours is refused") {
        file.frames[0].chunks[1] = ase::palette(3, 0, colours);
        CHECK(refused(file.bytes()) == "a named palette colour written over");
    }
    SUBCASE("a named colour past the palette's size is refused") {
        file.frames[0].chunks[0] = ase::palette(1, 0, colours);
        file.frames[0].chunks.erase(file.frames[0].chunks.begin() + 1);
        CHECK(refused(file.bytes()) == "a named palette colour past the palette's size");
    }
    SUBCASE("a palette past 1024 colours, or a colour outside them") {
        // cute_aseprite asserted, or wrote entries[-1].
        file.frames[0].chunks[0] = ase::palette(1025, 0, colours);
        CHECK(refused(file.bytes()) == "a palette past 1024 colours");
        file.frames[0].chunks[0] = ase::palette(3, 0xFFFFFFFFU, colours);
        CHECK(refused(file.bytes()) == "a palette colour past the 1024 there can be");
        file.frames[0].chunks[0] = ase::palette(1024, 1022, colours);
        CHECK(refused(file.bytes()) == "a palette colour past the 1024 there can be");
    }
    SUBCASE("an RGBA sheet's palette is not handed over at all") {
        ase::File rgba = two_frames();
        rgba.frames[0].chunks.insert(
            rgba.frames[0].chunks.begin(),
            { ase::palette(1025, 7, colours), ase::palette(3, 0, colours) });
        CHECK(refused(rgba.bytes()).empty());
    }
}

// ---------------------------------------------------------------------------
// The clock, held to any speed, any step and any frame a game sets.
// ---------------------------------------------------------------------------

namespace {

// A sheet built in memory with the holds given, every frame in one tag.
struct Holds {
    rmp::Sprite sprite;

    explicit Holds(const std::vector<float> &seconds, bool ping_pong = false) {
        rmp::SheetData data{};
        data.width = 4;
        data.height = 4;
        for (std::size_t i = 0; i < seconds.size(); ++i) {
            data.frames.push_back(
                { Rectangle{ static_cast<float>(i * 4), 0, 4, 4 }, seconds[i] });
        }
        data.tags.push_back(
            { "run", 0, static_cast<int>(seconds.size()) - 1, ping_pong, false });
        sprite.sheet = rmp::SpriteSheet{ rmp::detail::adopt(
            rmp::detail::ResourceKind::SHEET, std::move(data)) };
        REQUIRE(sprite.sheet.valid());
    }
    ~Holds() {
        sprite.sheet = rmp::SpriteSheet{};
        rmp::detail::release_all();
    }
    Holds(const Holds &) = delete;
    Holds &operator=(const Holds &) = delete;
};

} // namespace

TEST_CASE(
    "a speed that is not a number, or no finite one, holds the frame and warns once") {
    // A NaN speed made the clock NaN, and a NaN clock is never less than a
    // frame's hold: every step after it spun the guard's worth of frames.
    for (const float speed : { std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity() }) {
        CAPTURE(speed);
        rmp::detail::reset_reports_for_tests();
        Holds sheet({ 0.125f, 0.125f, 0.125f });
        rmp::Sprite &sprite = sheet.sprite;
        sprite.play("run");
        step(sprite, 0.125f);
        REQUIRE(sprite.frame_index() == 1);
        sprite.speed = speed;
        const int said = rmp::detail::report_count();
        for (int i = 0; i < 10; ++i) step(sprite, 0.0625f);
        CHECK(sprite.frame_index() == 1);
        CHECK(sprite.ours.elapsed == 0);
        CHECK(rmp::detail::report_count() == said + 1);
        sprite.speed = 1;
        step(sprite, 0.125f);
        CHECK(sprite.frame_index() == 2);
    }
}

TEST_CASE("a step that is not a positive finite time changes nothing, and says nothing") {
    for (const float delta : { -0.5f, -0.0f, std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity() }) {
        CAPTURE(delta);
        Holds sheet({ 0.125f, 0.125f, 0.125f });
        rmp::Sprite &sprite = sheet.sprite;
        sprite.play("run");
        step(sprite, 0.0625f);
        const int said = rmp::detail::report_count();
        step(sprite, delta);
        CHECK(sprite.frame_index() == 0);
        CHECK(sprite.ours.elapsed == 0.0625f);
        CHECK(rmp::detail::report_count() == said);
    }
}

TEST_CASE("a clock somebody wrote a NaN into starts its frame over") {
    // `ours` is public, so a game can write anything there.
    Holds sheet({ 0.125f, 0.125f, 0.125f });
    rmp::Sprite &sprite = sheet.sprite;
    sprite.play("run");
    sprite.ours.elapsed = std::numeric_limits<float>::quiet_NaN();
    step(sprite, 0.0625f);
    CHECK(sprite.ours.elapsed == 0.0625f);
    CHECK(sprite.frame_index() == 0);
    sprite.ours.elapsed = -5;
    step(sprite, 0.125f);
    CHECK(sprite.frame_index() == 1);
}

TEST_CASE("a step many laps long lands where the laps say") {
    // Three frames of 1/8 s, a lap of 3/8: 1000 s is 2666 laps and 1/4 s,
    // which is frame 2. The guard stopped the old loop after 16 frames, on
    // frame 1, with 998 s still owed: sixteen frames a step for the next five
    // hundred steps.
    Holds sheet({ 0.125f, 0.125f, 0.125f });
    rmp::Sprite &sprite = sheet.sprite;
    sprite.play("run");
    step(sprite, 1000.0f);
    CHECK(sprite.frame_index() == 2);
    CHECK(sprite.ours.elapsed == 0);
    step(sprite, 0.125f);
    CHECK(sprite.frame_index() == 0);

    SUBCASE("and a ping-pong lap goes there and back") {
        // 0 1 2 1: a lap of 4/8 s. 1000 s is 2000 laps exactly.
        Holds bounce({ 0.125f, 0.125f, 0.125f }, true);
        rmp::Sprite &pong = bounce.sprite;
        pong.play("run");
        step(pong, 1000.0f + 0.375f);
        CHECK(pong.frame_index() == 1);
        step(pong, 0.125f);
        CHECK(pong.frame_index() == 0);
    }
}

TEST_CASE("a frame set past either end of int comes back into the tag") {
    // frame + 1 at INT_MAX was signed overflow, which UBSan reported.
    for (const int frame : { INT_MAX, INT_MIN, -1, 3, 1000 }) {
        for (const float speed : { 1.0f, -1.0f }) {
            for (const bool ping_pong : { false, true }) {
                CAPTURE(frame);
                CAPTURE(speed);
                CAPTURE(ping_pong);
                Holds sheet({ 0.125f, 0.125f, 0.125f }, ping_pong);
                rmp::Sprite &sprite = sheet.sprite;
                sprite.play("run");
                sprite.speed = speed;
                sprite.set_frame(frame);
                step(sprite, 0.125f);
                CHECK(sprite.frame_index() >= 0);
                CHECK(sprite.frame_index() <= 2);
            }
        }
    }
}

TEST_CASE("a ping-pong tag brings back a frame set outside it") {
    // It turned round and tried the frame on the other side -- outside too
    // -- and stayed where it was: on frame 10 of a three-frame tag, a blank,
    // for good.
    Holds sheet({ 0.125f, 0.125f, 0.125f }, true);
    rmp::Sprite &sprite = sheet.sprite;
    sprite.play("run");
    sprite.set_frame(10);
    step(sprite, 0.125f);
    CHECK(sprite.frame_index() == 2);
    step(sprite, 0.125f);
    CHECK(sprite.frame_index() == 1);
}
