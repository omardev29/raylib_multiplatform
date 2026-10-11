#pragma once
// ---------------------------------------------------------------------------
// An e2e script: what a player does to a game, frame by frame, and what the
// game must show for it. tests/e2e/<area>/<example>.e2e, one per example
// under examples/<area>/<example>/, played by tests/e2e/driver.cpp.
//
//     # a comment, to the end of the line; blank lines are nothing
//     <frame> press KEY_SPACE       held from this frame on
//     <frame> release KEY_SPACE     up from this frame on
//     <frame> tap KEY_ENTER         down for this one frame, up the next
//     <frame> tap GAMEPAD_BUTTON_LEFT_FACE_DOWN
//                                   the same three for the first gamepad's
//                                   buttons: the d-pad, the face buttons...
//     <frame> click 400 300         the left button, down for this one frame
//                                   at x, y in window pixels; the pointer
//                                   stays there
//     <frame> shot over             the picture this frame drew, against
//                                   tests/e2e/golden/<area>/<example>/over.png
//     <frame> expect scene OverScene
//                                   the scene on top once this frame is over
//                                   (a class name, or its end: `OverScene`
//                                   matches `(anonymous namespace)::OverScene`)
//     <frame> end                   the last frame: the game quits after it
//
// Frames count from 0, the first frame the game runs, at exactly 1/60 s
// each, and never go backwards. Keys and buttons are raylib's names (KEY_A,
// KEY_ENTER, KEY_KP_ENTER, GAMEPAD_BUTTON_RIGHT_FACE_DOWN...). Every rejection
// says the line, and parse() makes them all before a game starts:
//
//   - input at frame 0: the first frame has no edges (rmp::input compares
//     it with itself), so a press there would be held and never pressed;
//   - a key (or a button) pressed twice, released while up or in the frame
//     it went down, or pressed again in the frame it is released or the
//     frame after its tap -- it would never come up, so the second press
//     would be no press at all; the same for a click after a click;
//   - a shot name that is not lower case, digits and `_` (it is a file
//     name), or that is used twice;
//   - no `end`, anything after it, or a script that checks nothing (no
//     `shot`, no `expect`).
// ---------------------------------------------------------------------------

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace e2e {

enum class Verb { PRESS, RELEASE, TAP, CLICK, SHOT, EXPECT_SCENE, END };

// One line of a script that does something.
struct Step {
    int line = 0; // in the file, from 1
    int frame = 0;
    Verb verb = Verb::END;
    int key = 0; // PRESS, RELEASE and TAP: raylib's KeyboardKey...
    bool pad = false; // ...or, when this is true, the first gamepad's GamepadButton
    int x = 0; // CLICK
    int y = 0;
    std::string name; // SHOT: the golden's name; EXPECT_SCENE: the class
};

// Every step in the order written, which is frame order; the last is END.
struct Script {
    std::vector<Step> steps;
};

struct ParseError {
    int line = 0;
    std::string reason;
};

// The longest a script may run. Ten minutes at 60 Hz: a script longer than
// that is a typo in a frame number, and under the software renderer it would
// take an hour to find out.
inline constexpr int MAX_FRAME = 60 * 60 * 10;

// False, with the first mistake in `error`, when the text is not a script;
// `out` is then left as it was.
bool parse(std::string_view text, Script &out, ParseError &error);

// raylib's KeyboardKey for a name such as "KEY_SPACE", or nothing.
std::optional<int> key_named(std::string_view name);
std::string key_name(int key);

// raylib's GamepadButton for a name such as "GAMEPAD_BUTTON_LEFT_FACE_DOWN",
// or nothing.
std::optional<int> pad_button_named(std::string_view name);
std::string pad_button_name(int button);

// A C++ type's name as typeid().name() gives it, the way it is written:
// "N12_GLOBAL__N_19OverSceneE" is "(anonymous namespace)::OverScene".
// What it cannot read -- a template, say -- comes back as it was.
std::string readable_type(std::string_view raw);

// Whether `full`, a readable_type(), is the class `wanted`: the whole name,
// or its last parts after a `::`.
bool names_class(std::string_view full, std::string_view wanted);

} // namespace e2e
