// ---------------------------------------------------------------------------
// The e2e driver's two halves that can be checked without playing a game:
// the script's grammar (tests/e2e/script.h) and the pixel comparison
// (tests/e2e/compare.h). Every rejection the grammar makes is here with the
// line it names, because a script that is wrong has to say where before the
// software renderer spends a minute on it; and the comparison is exact, so
// one pixel off has to fail and a size mismatch has to say so.
//
// What a played game shows is tools/examples_build.sh's, in its e2e section.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "e2e/compare.h"
#include "e2e/script.h"

#include <raylib.h>
#include <rmp/input.h>

#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Rejection {
    bool parsed = false;
    int line = 0;
    std::string reason;
};

Rejection rejected(std::string_view text) {
    e2e::Script script;
    e2e::ParseError error;
    const bool parsed = e2e::parse(text, script, error);
    return Rejection{ parsed, error.line, error.reason };
}

// The rejection, at that line, saying that.
void refuses(std::string_view text, int line, std::string_view says) {
    const Rejection got = rejected(text);
    INFO("script: ", std::string(text), "\nreason: ", got.reason);
    CHECK_FALSE(got.parsed);
    CHECK(got.line == line);
    CHECK(got.reason.find(says) != std::string::npos);
}

// One colour everywhere, `width` by `height`.
e2e::Picture flat(int width, int height, unsigned char r, unsigned char g,
                  unsigned char b) {
    e2e::Picture p;
    p.width = width;
    p.height = height;
    for (int i = 0; i < width * height; ++i)
        p.rgba.insert(p.rgba.end(), { r, g, b, 255 });
    return p;
}

std::size_t at(const e2e::Picture &p, int x, int y) {
    return ((static_cast<std::size_t>(y) * static_cast<std::size_t>(p.width)) +
            static_cast<std::size_t>(x)) *
        4;
}

// A picture that has to be there: a missing one fails here, and then compares
// as an empty picture, which matches nothing.
e2e::Picture loaded(const fs::path &path) {
    const std::optional<e2e::Picture> picture = e2e::load_png(path);
    CHECK_MESSAGE(picture.has_value(), path.string(),
                  " is not there, or is not a picture");
    return picture.value_or(e2e::Picture{});
}

// A folder of its own under the system temp directory, removed afterwards.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device device;
        path = fs::temp_directory_path() /
            ("rmp_e2e_test_" + std::to_string(device()) + "_" + std::to_string(device()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
};

class ProbeScene {
public:
    virtual ~ProbeScene() = default;
    ProbeScene() = default;
    ProbeScene(const ProbeScene &) = delete;
    ProbeScene &operator=(const ProbeScene &) = delete;
};

} // namespace

TEST_SUITE("e2e: the script") {
    TEST_CASE("a script is read in order, with comments, blank lines and CRLF") {
        e2e::Script script;
        e2e::ParseError error;
        const std::string_view text = "# pong\r\n"
                                      "\n"
                                      "1 press KEY_W   # up\r\n"
                                      "30 release KEY_W\n"
                                      "31 tap KEY_ENTER\n"
                                      "33 tap KEY_ENTER\n"
                                      "40 click 400 225\n"
                                      "60 shot serve\n"
                                      "60 expect scene PongScene\n"
                                      "61 expect scene game::TitleScene\n"
                                      "90 end\n"
                                      "# after the end, a comment is still nothing\n";
        REQUIRE(e2e::parse(text, script, error));
        REQUIRE(script.steps.size() == 9);
        CHECK(script.steps[0].verb == e2e::Verb::PRESS);
        CHECK(script.steps[0].key == KEY_W);
        CHECK(script.steps[0].line == 3);
        CHECK(script.steps[0].frame == 1);
        CHECK(script.steps[1].verb == e2e::Verb::RELEASE);
        CHECK(script.steps[2].verb == e2e::Verb::TAP);
        CHECK(script.steps[2].key == KEY_ENTER);
        CHECK(script.steps[3].frame == 33);
        CHECK(script.steps[4].verb == e2e::Verb::CLICK);
        CHECK(script.steps[4].x == 400);
        CHECK(script.steps[4].y == 225);
        CHECK(script.steps[5].verb == e2e::Verb::SHOT);
        CHECK(script.steps[5].name == "serve");
        CHECK(script.steps[6].verb == e2e::Verb::EXPECT_SCENE);
        CHECK(script.steps[6].name == "PongScene");
        CHECK(script.steps[7].name == "game::TitleScene");
        CHECK(script.steps[8].verb == e2e::Verb::END);
        CHECK(script.steps[8].frame == 90);
        CHECK(script.steps[8].line == 11);
    }

    TEST_CASE("a tap is over two frames later, and a click too") {
        e2e::Script script;
        e2e::ParseError error;
        CHECK(e2e::parse("1 tap KEY_A\n3 tap KEY_A\n5 press KEY_A\n9 release KEY_A\n"
                         "1000 click 1 1\n1002 click 1 1\n1003 shot a\n1003 end\n",
                         script, error));
        INFO(error.reason);
    }

    TEST_CASE("a parse that fails leaves the script as it was") {
        e2e::Script script;
        e2e::ParseError error;
        REQUIRE(e2e::parse("1 shot a\n1 end\n", script, error));
        REQUIRE(script.steps.size() == 2);
        CHECK_FALSE(e2e::parse("1 shot b\n", script, error));
        CHECK(script.steps.size() == 2);
        CHECK(script.steps[0].name == "a");
    }

    TEST_CASE("no end, anything after it, and a script that checks nothing") {
        refuses("", 1, "no `end`");
        refuses("# only a comment\n", 1, "no `end`");
        refuses("1 tap KEY_A\n2 shot a\n", 2, "no `end`");
        refuses("1 tap KEY_A\n2 shot a", 2, "no `end`");
        refuses("1 shot a\n2 end\n3 shot b\n", 3, "nothing may follow `end`, on line 2");
        refuses("1 shot a\n2 end\n3 end\n", 3, "nothing may follow `end`");
        refuses("1 tap KEY_A\n2 end\n", 2, "checks nothing");
        refuses("1 shot a\n2 end now\n", 2, "end takes nothing; `now` is extra");
    }

    TEST_CASE("the frame number") {
        refuses("press KEY_A\n", 1, "expected a frame number, not `press`");
        refuses("-1 shot a\n", 1, "expected a frame number, not `-1`");
        refuses("+1 shot a\n", 1, "expected a frame number");
        refuses("1.5 shot a\n", 1, "expected a frame number, not `1.5`");
        refuses("1e3 shot a\n", 1, "expected a frame number");
        refuses("9999999999 shot a\n", 1, "expected a frame number");
        refuses("36001 shot a\n", 1, "past 36000");
        refuses("10 shot a\n\n5 end\n", 3, "frame 5 comes before frame 10 on line 1");
        refuses("12\n", 1, "frame 12 and nothing to do");
        refuses("12   # a comment is not something to do\n", 1, "nothing to do");
        refuses("12 hold KEY_A\n", 1, "unknown verb `hold`");
        refuses("12 PRESS KEY_A\n", 1, "unknown verb `PRESS`");
    }

    TEST_CASE("the first gamepad's buttons, beside the keys") {
        e2e::Script script;
        e2e::ParseError error;
        REQUIRE(e2e::parse("1 press GAMEPAD_BUTTON_LEFT_FACE_DOWN\n"
                           "1 press KEY_DOWN\n"
                           "9 release GAMEPAD_BUTTON_LEFT_FACE_DOWN\n"
                           "10 tap GAMEPAD_BUTTON_RIGHT_FACE_DOWN\n"
                           "12 tap GAMEPAD_BUTTON_RIGHT_FACE_DOWN\n"
                           "12 shot a\n12 end\n",
                           script, error));
        INFO(error.reason);
        REQUIRE(script.steps.size() == 7);
        CHECK(script.steps[0].verb == e2e::Verb::PRESS);
        CHECK(script.steps[0].pad);
        CHECK(script.steps[0].key == GAMEPAD_BUTTON_LEFT_FACE_DOWN);
        CHECK_FALSE(script.steps[1].pad);
        CHECK(script.steps[1].key == KEY_DOWN);
        CHECK(script.steps[2].verb == e2e::Verb::RELEASE);
        CHECK(script.steps[2].pad);
        CHECK(script.steps[3].verb == e2e::Verb::TAP);
        CHECK(script.steps[3].key == GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
        CHECK(script.steps[3].pad);
        CHECK_FALSE(script.steps[5].pad); // a shot is no button
    }

    TEST_CASE("a button goes down once and comes up once, as a key does") {
        refuses(
            "1 press GAMEPAD_BUTTON_LEFT_FACE_UP\n2 press GAMEPAD_BUTTON_LEFT_FACE_UP\n",
            2, "GAMEPAD_BUTTON_LEFT_FACE_UP is already held: pressed on line 1");
        refuses("1 release GAMEPAD_BUTTON_MIDDLE\n", 1,
                "release GAMEPAD_BUTTON_MIDDLE, which is not held");
        refuses("1 tap GAMEPAD_BUTTON_RIGHT_FACE_DOWN\n2 tap "
                "GAMEPAD_BUTTON_RIGHT_FACE_DOWN\n",
                2, "GAMEPAD_BUTTON_RIGHT_FACE_DOWN was tapped at frame 1 (line 1)");
        refuses("0 tap GAMEPAD_BUTTON_RIGHT_FACE_DOWN\n", 1, "tap at frame 0");
        refuses("1 press GAMEPAD_BUTTON_UNKNOWN\n", 1,
                "`GAMEPAD_BUTTON_UNKNOWN` is not a key or a gamepad button");
        refuses("1 press GAMEPAD_AXIS_LEFT_X\n", 1, "is not a key or a gamepad button");
        refuses("1 press gamepad_button_middle\n", 1, "is not a key or a gamepad button");
        // A key and a button are two things: holding one says nothing of the
        // other.
        e2e::Script script;
        e2e::ParseError error;
        CHECK(e2e::parse("1 press KEY_ENTER\n1 press GAMEPAD_BUTTON_RIGHT_FACE_DOWN\n"
                         "2 release GAMEPAD_BUTTON_RIGHT_FACE_DOWN\n2 shot a\n2 end\n",
                         script, error));
    }

    TEST_CASE("keys") {
        refuses("12 press\n", 1, "press needs a key");
        refuses("12 tap\n", 1, "tap needs a key");
        refuses("12 release\n", 1, "release needs a key");
        refuses("12 press KEY_A KEY_B\n", 1, "press takes one key; `KEY_B` is extra");
        refuses("12 press SPACE\n", 1, "`SPACE` is not a key");
        refuses("12 press key_space\n", 1, "`key_space` is not a key");
        refuses("12 press KEY_NULL\n", 1, "`KEY_NULL` is not a key");
        refuses("12 press KEY_BACK\n", 1, "`KEY_BACK` is not a key");
        refuses("0 press KEY_A\n", 1, "press at frame 0");
        refuses("0 tap KEY_A\n", 1, "tap at frame 0");
    }

    TEST_CASE("a key goes down once and comes up once") {
        refuses("1 press KEY_A\n2 press KEY_A\n", 2,
                "KEY_A is already held: pressed on line 1");
        refuses("1 press KEY_A\n2 tap KEY_A\n", 2, "already held: pressed on line 1");
        refuses("1 release KEY_A\n", 1, "release KEY_A, which is not held");
        refuses("1 press KEY_A\n2 release KEY_A\n3 release KEY_A\n", 3, "not held");
        refuses("1 press KEY_A\n1 release KEY_A\n", 2,
                "at the frame it is pressed, on line 1: it never goes down");
        refuses("1 tap KEY_A\n1 tap KEY_A\n", 2,
                "KEY_A is already down: tapped on line 1");
        refuses("1 tap KEY_A\n1 press KEY_A\n", 2, "already down: tapped on line 1");
        refuses(
            "1 tap KEY_A\n2 press KEY_A\n", 2,
            "KEY_A was tapped at frame 1 (line 1) and comes up at frame 2: press it from "
            "frame 3");
        refuses("1 tap KEY_A\n2 tap KEY_A\n", 2, "was tapped at frame 1");
        refuses("1 tap KEY_A\n2 release KEY_A\n", 2,
                "which the tap on line 1 already lets go of");
        refuses("1 tap KEY_A\n1 release KEY_A\n", 2, "already lets go of");
        refuses("1 tap KEY_A\n3 release KEY_A\n", 2, "not held");
        refuses("1 press KEY_A\n5 release KEY_A\n5 press KEY_A\n", 3,
                "KEY_A is released at this frame, on line 2: press it from frame 6");
        refuses("1 press KEY_A\n5 release KEY_A\n5 tap KEY_A\n", 3,
                "released at this frame");
    }

    TEST_CASE("click") {
        refuses("1 click\n", 1, "click needs x and y");
        refuses("1 click 10\n", 1, "click needs x and y");
        refuses("1 click 10 20 30\n", 1, "click takes x and y; `30` is extra");
        refuses("1 click x 20\n", 1, "`x` is not a coordinate");
        refuses("1 click 10 -5\n", 1, "`-5` is not a coordinate");
        refuses("1 click 10.5 5\n", 1, "`10.5` is not a coordinate");
        refuses("0 click 10 20\n", 1, "click at frame 0");
        refuses("1 click 1 1\n1 click 2 2\n", 2,
                "the button is already down: clicked on line 1");
        refuses("1 click 1 1\n2 click 2 2\n", 2,
                "the click on line 1 lets go at this frame: click again from frame 3");
    }

    TEST_CASE("type: what a text field takes, in the frame it is typed") {
        e2e::Script script;
        e2e::ParseError error;
        REQUIRE(e2e::parse("5 type \"Ana Ruiz\"   # a space is a character\n"
                           "6 type \"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"\n"
                           "7 tap KEY_BACKSPACE\n"
                           "8 type \"0123456789abcdef\"\n"
                           "9 shot typed\n"
                           "9 end\n",
                           script, error));
        REQUIRE(script.steps.size() == 6);
        CHECK(script.steps[0].verb == e2e::Verb::TYPE);
        CHECK(script.steps[0].frame == 5);
        CHECK(script.steps[0].typed ==
              std::vector<int>{ 'A', 'n', 'a', ' ', 'R', 'u', 'i', 'z' });
        CHECK(script.steps[1].typed == std::vector<int>{ 0xE9, 0x20AC, 0x1F600 });
        CHECK(script.steps[2].key == KEY_BACKSPACE);
        CHECK(script.steps[3].typed.size() == e2e::MAX_TYPED_PER_FRAME);
    }

    TEST_CASE("type") {
        refuses("1 type\n", 1, "type needs the characters");
        refuses("1 type   # nothing\n", 1, "type needs the characters");
        refuses("1 type Ana\n", 1, "type takes one quoted run of characters");
        refuses("1 type \"Ana\n", 1, "type takes one quoted run of characters");
        refuses("1 type \"Ana\" \"Bo\"\n", 1, "a `\"` cannot be typed");
        refuses("1 type \"\"\n", 1, "types nothing");
        refuses("1 type \"a#b\"\n", 1, "type takes one quoted run");
        refuses("1 type \"\xC3\"\n", 1, "is not UTF-8");
        refuses("1 type \"\x80\"\n", 1, "is not UTF-8");
        refuses("1 type \"\xC0\xAF\"\n", 1, "is not UTF-8"); // an overlong '/'
        refuses("1 type \"\xED\xA0\x80\"\n", 1, "is not UTF-8"); // a surrogate
        refuses("1 type \"\xF4\x90\x80\x80\"\n", 1, "is not UTF-8"); // past U+10FFFF
        refuses("1 type \"a\tb\"\n", 1, "holds a control character");
        refuses("1 type \"\xC2\x85\"\n", 1, "holds a control character"); // NEL
        refuses("1 type \"0123456789abcdefg\"\n", 1,
                "is 17 characters, and a frame takes 16 at most");
        refuses("0 type \"a\"\n", 1, "type at frame 0");
        refuses("1 type \"a\"\n1 type \"b\"\n", 2,
                "this frame is already typed into, on line 1");
    }

    TEST_CASE("shot") {
        refuses("1 shot\n", 1, "shot needs a name");
        refuses("1 shot a b\n", 1, "shot takes one name; `b` is extra");
        refuses("1 shot Over\n", 1, "`Over` is not a shot name");
        refuses("1 shot game-over\n", 1, "is not a shot name");
        refuses("1 shot ../escape\n", 1, "is not a shot name");
        refuses("1 shot " + std::string(65, 'a') + "\n", 1, "is not a shot name");
        refuses("1 shot a\n2 shot a\n", 2, "shot `a` is already taken, on line 1");
    }

    TEST_CASE("expect") {
        refuses("1 expect\n", 1, "expect what? `expect scene <Class>`");
        refuses("1 expect score 1\n", 1, "cannot expect `score`");
        refuses("1 expect scene\n", 1, "expect scene needs a class name");
        refuses("1 expect scene A B\n", 1, "expect scene takes one class; `B` is extra");
        refuses("1 expect scene 9Lives\n", 1, "`9Lives` is not a class name");
        refuses("1 expect scene game::\n", 1, "`game::` is not a class name");
        refuses("1 expect scene game:Title\n", 1, "is not a class name");
        refuses("1 expect scene ::Title\n", 1, "is not a class name");
        refuses("1 expect scene (anonymous)::Title\n", 1, "is not a class name");
    }

    TEST_CASE("key names both ways") {
        CHECK(e2e::key_named("KEY_SPACE") == KEY_SPACE);
        CHECK(e2e::key_named("KEY_KP_ENTER") == KEY_KP_ENTER);
        CHECK(e2e::key_named("KEY_Z") == KEY_Z);
        CHECK_FALSE(e2e::key_named("KEY_").has_value());
        CHECK(e2e::key_name(KEY_ENTER) == "KEY_ENTER");
        CHECK(e2e::key_name(9999) == "key 9999");
        CHECK(e2e::pad_button_named("GAMEPAD_BUTTON_LEFT_FACE_UP") ==
              GAMEPAD_BUTTON_LEFT_FACE_UP);
        CHECK(e2e::pad_button_named("GAMEPAD_BUTTON_RIGHT_THUMB") ==
              GAMEPAD_BUTTON_RIGHT_THUMB);
        CHECK_FALSE(e2e::pad_button_named("GAMEPAD_BUTTON_UNKNOWN").has_value());
        CHECK_FALSE(e2e::pad_button_named("KEY_SPACE").has_value());
        CHECK_FALSE(e2e::key_named("GAMEPAD_BUTTON_MIDDLE").has_value());
        CHECK(e2e::pad_button_name(GAMEPAD_BUTTON_MIDDLE_RIGHT) ==
              "GAMEPAD_BUTTON_MIDDLE_RIGHT");
        CHECK(e2e::pad_button_name(99) == "gamepad button 99");
        // Every button rmp::input holds has a name, and only those: one past
        // the last is no button.
        for (int b = GAMEPAD_BUTTON_LEFT_FACE_UP; b <= GAMEPAD_BUTTON_RIGHT_THUMB; ++b) {
            CHECK(e2e::pad_button_named(e2e::pad_button_name(b)) == b);
        }
        CHECK(GAMEPAD_BUTTON_RIGHT_THUMB < rmp::input::detail::DeviceState::PAD_BUTTONS);
    }
}

TEST_SUITE("e2e: scene names") {
    TEST_CASE("the Itanium names a scene has, read back") {
        CHECK(e2e::readable_type("9PongScene") == "PongScene");
        CHECK(e2e::readable_type("N12_GLOBAL__N_19OverSceneE") ==
              "(anonymous namespace)::OverScene");
        CHECK(e2e::readable_type("N4game10TitleSceneE") == "game::TitleScene");
        CHECK(e2e::readable_type("class `anonymous namespace'::OverScene") ==
              "(anonymous namespace)::OverScene");
        CHECK(e2e::readable_type("class game::TitleScene") == "game::TitleScene");
        // What it cannot read comes back as it was.
        CHECK(e2e::readable_type("St6vectorIiSaIiEE") == "St6vectorIiSaIiEE");
        CHECK(e2e::readable_type("4gameX") == "4gameX");
        CHECK(e2e::readable_type("4game5Title") == "4game5Title");
        CHECK(e2e::readable_type("") == "");
    }

    TEST_CASE("this compiler's name for a class in an anonymous namespace") {
        const ProbeScene probe;
        CHECK(e2e::readable_type(typeid(probe).name()) ==
              "(anonymous namespace)::ProbeScene");
    }

    TEST_CASE("a class matches by its whole name or its last parts") {
        CHECK(e2e::names_class("(anonymous namespace)::OverScene", "OverScene"));
        CHECK(e2e::names_class("game::TitleScene", "TitleScene"));
        CHECK(e2e::names_class("game::TitleScene", "game::TitleScene"));
        CHECK(e2e::names_class("PongScene", "PongScene"));
        CHECK_FALSE(e2e::names_class("PongScene", "Scene"));
        CHECK_FALSE(e2e::names_class("game::TitleScene", "itleScene"));
        CHECK_FALSE(e2e::names_class("game::TitleScene", "OverScene"));
        CHECK_FALSE(e2e::names_class("OverScene", ""));
    }
}

TEST_SUITE("e2e: the comparison") {
    TEST_CASE("the same picture is no difference") {
        const e2e::Picture a = flat(8, 4, 10, 20, 30);
        const e2e::Difference d = e2e::compare(a, a, 0);
        CHECK(d.none());
        CHECK(d.pixels == 0);
        CHECK(d.largest == 0);
        CHECK(e2e::fingerprint(a) == e2e::fingerprint(flat(8, 4, 10, 20, 30)));
    }

    TEST_CASE("one pixel off by one is a difference at tolerance 0") {
        const e2e::Picture expected = flat(8, 4, 10, 20, 30);
        e2e::Picture actual = expected;
        actual.rgba[at(actual, 5, 2) + 1] = 21;
        const e2e::Difference d = e2e::compare(expected, actual, 0);
        CHECK_FALSE(d.none());
        CHECK(d.same_size);
        CHECK(d.pixels == 1);
        CHECK(d.largest == 1);
        CHECK(d.first_x == 5);
        CHECK(d.first_y == 2);
        CHECK(e2e::fingerprint(expected) != e2e::fingerprint(actual));
        // The tolerance is a parameter, and its edge is inclusive.
        CHECK(e2e::compare(expected, actual, 1).none());
        CHECK(e2e::compare(expected, actual, 1).largest == 1);
    }

    TEST_CASE("alpha counts") {
        const e2e::Picture expected = flat(2, 2, 0, 0, 0);
        e2e::Picture actual = expected;
        actual.rgba[at(actual, 1, 1) + 3] = 0;
        CHECK(e2e::compare(expected, actual, 0).pixels == 1);
        CHECK(e2e::compare(expected, actual, 0).largest == 255);
    }

    TEST_CASE("a size mismatch is a difference, and nothing else is counted") {
        const e2e::Difference wider =
            e2e::compare(flat(8, 4, 0, 0, 0), flat(9, 4, 0, 0, 0), 255);
        CHECK_FALSE(wider.none());
        CHECK_FALSE(wider.same_size);
        CHECK(wider.pixels == 0);
        CHECK_FALSE(e2e::compare(flat(8, 4, 0, 0, 0), flat(8, 5, 0, 0, 0), 0).same_size);
        CHECK_FALSE(e2e::compare(flat(8, 4, 0, 0, 0), flat(4, 8, 0, 0, 0), 0).same_size);
        e2e::Picture short_of_bytes = flat(8, 4, 0, 0, 0);
        short_of_bytes.rgba.pop_back();
        CHECK_FALSE(e2e::compare(flat(8, 4, 0, 0, 0), short_of_bytes, 0).same_size);
    }

    TEST_CASE("the difference picture: red where it differs, the expected one dimmed "
              "elsewhere") {
        const e2e::Picture expected = flat(3, 2, 200, 100, 40);
        e2e::Picture actual = expected;
        actual.rgba[at(actual, 2, 1)] = 0;
        const e2e::Picture diff = e2e::difference_picture(expected, actual, 0);
        REQUIRE(diff.width == 3);
        REQUIRE(diff.height == 2);
        CHECK(diff.rgba[at(diff, 2, 1)] == 255);
        CHECK(diff.rgba[at(diff, 2, 1) + 1] == 0);
        CHECK(diff.rgba[at(diff, 2, 1) + 2] == 0);
        CHECK(diff.rgba[at(diff, 0, 0)] == 50);
        CHECK(diff.rgba[at(diff, 0, 0) + 1] == 25);
        CHECK(diff.rgba[at(diff, 0, 0) + 2] == 10);
        CHECK(diff.rgba[at(diff, 0, 0) + 3] == 255);

        // A size mismatch: as large as the larger, red where only one has pixels.
        const e2e::Picture grown =
            e2e::difference_picture(expected, flat(4, 2, 200, 100, 40), 0);
        CHECK(grown.width == 4);
        CHECK(grown.rgba[at(grown, 3, 0)] == 255);
        CHECK(grown.rgba[at(grown, 0, 0)] == 50);
    }

    TEST_CASE("a PNG reads back exactly what was written") {
        const TempDir dir;
        e2e::Picture picture = flat(5, 3, 1, 2, 3);
        picture.rgba[at(picture, 4, 2)] = 254;
        picture.rgba[at(picture, 0, 1) + 3] = 128;
        REQUIRE(e2e::write_png(picture, dir.path / "nested" / "p.png"));
        CHECK(e2e::compare(picture, loaded(dir.path / "nested" / "p.png"), 0).none());
        CHECK_FALSE(e2e::load_png(dir.path / "missing.png").has_value());
        CHECK_FALSE(e2e::write_png(e2e::Picture{}, dir.path / "empty.png"));
    }

    TEST_CASE("a failure leaves three pictures: expected, actual, diff") {
        const TempDir dir;
        const e2e::Picture expected = flat(6, 4, 40, 80, 120);
        e2e::Picture actual = expected;
        actual.rgba[at(actual, 3, 3) + 2] = 0;
        REQUIRE(e2e::write_failure(expected, actual, dir.path / "out", "serve"));
        CHECK(e2e::compare(loaded(dir.path / "out" / "serve.expected.png"), expected, 0)
                  .none());
        CHECK(e2e::compare(loaded(dir.path / "out" / "serve.actual.png"), actual, 0)
                  .none());
        const e2e::Picture diff = loaded(dir.path / "out" / "serve.diff.png");
        CHECK(e2e::compare(diff, e2e::difference_picture(expected, actual, 0), 0).none());
        REQUIRE(diff.rgba.size() == 6 * 4 * 4);
        CHECK(diff.rgba[at(diff, 3, 3)] == 255);
        // A size mismatch writes all three as well.
        REQUIRE(
            e2e::write_failure(expected, flat(7, 4, 0, 0, 0), dir.path / "out", "wide"));
        CHECK(fs::is_regular_file(dir.path / "out" / "wide.diff.png"));
    }

    TEST_CASE("a raylib image of another format comes out as RGBA") {
        e2e::OwnedImage gray(GenImageColor(3, 2, Color{ 7, 7, 7, 255 }));
        ImageFormat(&gray.image(), PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
        REQUIRE(gray.image().format == PIXELFORMAT_UNCOMPRESSED_GRAYSCALE);
        const e2e::Picture rgba = e2e::picture_of(gray.image());
        CHECK(rgba.width == 3);
        CHECK(rgba.height == 2);
        CHECK(rgba.rgba.size() == 24);
        CHECK(rgba.rgba[0] == 7);
        CHECK(rgba.rgba[2] == 7);
        CHECK(rgba.rgba[3] == 255);
        CHECK(e2e::picture_of(::Image{}).rgba.empty());
    }
}
