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

#include <rmp/assets.h>
#include <rmp/object.h>

#include <fstream>
#include <string>
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
// resource slot is normally what frees them, so a test that parses without
// adopting has to do it itself -- a leaking test is a test that will one day be
// blamed on the thing it is testing.
struct ParsedSheet {
    rmp::SheetData data{};
    ParsedSheet() {
        const std::vector<unsigned char> bytes = fixture_bytes();
        REQUIRE(rmp::animation::detail::parse_sheet(
            bytes.data(), static_cast<int>(bytes.size()), &data));
    }
    ~ParsedSheet() { rmp::animation::detail::free_sheet(&data); }
    ParsedSheet(const ParsedSheet &) = delete;
    ParsedSheet &operator=(const ParsedSheet &) = delete;
};

// A Sprite holding the fixture, without a texture. adopt() is the same seam the
// rres pack uses, so this is not a special path for tests -- it is the ordinary
// one with the GPU half left out.
struct Fixture {
    rmp::Sprite sprite;

    Fixture() {
        // adopt() COPIES the 64-byte payload into the slot, and from then on
        // the slot is the single owner of the frame and tag tables -- freeing
        // them when the last handle goes. So the SheetData is built here and
        // handed over, not kept.
        ParsedSheet owned;
        rmp::SheetData data = owned.data;
        owned.data = rmp::SheetData{}; // the slot owns the tables from here
        auto *slot =
            rmp::detail::adopt(rmp::detail::ResourceKind::SHEET, &data, sizeof(data));
        REQUIRE(slot != nullptr);
        sprite.sheet = rmp::SpriteSheet{ slot };
    }
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
    REQUIRE(sheet.frame_count == 4);

    // The numbers the fixture was written with, in seconds.
    CHECK(sheet.frame(0).seconds == doctest::Approx(0.100));
    CHECK(sheet.frame(1).seconds == doctest::Approx(0.200));
    CHECK(sheet.frame(2).seconds == doctest::Approx(0.100));
    CHECK(sheet.frame(3).seconds == doctest::Approx(0.050));

    SUBCASE("and they are packed side by side in one row") {
        for (int i = 0; i < sheet.frame_count; i++) {
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
    REQUIRE(sheet.tag_count == 3);

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
    CHECK(rmp::animation::detail::tag_index(sheet, nullptr) == -1);
}

TEST_CASE("rubbish is refused rather than half-read") {
    rmp::SheetData sheet{};
    const unsigned char junk[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK_FALSE(rmp::animation::detail::parse_sheet(junk, sizeof(junk), &sheet));
    CHECK_FALSE(rmp::animation::detail::parse_sheet(nullptr, 100, &sheet));
    CHECK_FALSE(rmp::animation::detail::parse_sheet(junk, 0, &sheet));
    CHECK_FALSE(rmp::animation::detail::parse_sheet(junk, sizeof(junk), nullptr));
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
    for (int i = 0; i < 10; i++) step(sprite, 0.100f);
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
        for (int i = 0; i < 100; i++) step(sprite, 0.100f);
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
        for (int i = 0; i < 100; i++) step(sprite, 0.100f);
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
