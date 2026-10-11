// ---------------------------------------------------------------------------
// The e2e script's grammar -- see tests/e2e/script.h. Every rejection is a
// case in tests/e2e_test.cpp, with its line.
// ---------------------------------------------------------------------------

#include "script.h"

#include <raylib.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <utility>

namespace e2e {

namespace {

struct KeyName {
    std::string_view name;
    int key = 0;
};

// raylib's KeyboardKey, every one a desktop keyboard has. Not KEY_NULL, and
// not Android's KEY_BACK, KEY_MENU and the volume keys: rmp::input reads none
// of them from a keyboard.
constexpr auto KEYS = std::to_array<KeyName>({
    { "KEY_APOSTROPHE", KEY_APOSTROPHE },
    { "KEY_COMMA", KEY_COMMA },
    { "KEY_MINUS", KEY_MINUS },
    { "KEY_PERIOD", KEY_PERIOD },
    { "KEY_SLASH", KEY_SLASH },
    { "KEY_ZERO", KEY_ZERO },
    { "KEY_ONE", KEY_ONE },
    { "KEY_TWO", KEY_TWO },
    { "KEY_THREE", KEY_THREE },
    { "KEY_FOUR", KEY_FOUR },
    { "KEY_FIVE", KEY_FIVE },
    { "KEY_SIX", KEY_SIX },
    { "KEY_SEVEN", KEY_SEVEN },
    { "KEY_EIGHT", KEY_EIGHT },
    { "KEY_NINE", KEY_NINE },
    { "KEY_SEMICOLON", KEY_SEMICOLON },
    { "KEY_EQUAL", KEY_EQUAL },
    { "KEY_A", KEY_A },
    { "KEY_B", KEY_B },
    { "KEY_C", KEY_C },
    { "KEY_D", KEY_D },
    { "KEY_E", KEY_E },
    { "KEY_F", KEY_F },
    { "KEY_G", KEY_G },
    { "KEY_H", KEY_H },
    { "KEY_I", KEY_I },
    { "KEY_J", KEY_J },
    { "KEY_K", KEY_K },
    { "KEY_L", KEY_L },
    { "KEY_M", KEY_M },
    { "KEY_N", KEY_N },
    { "KEY_O", KEY_O },
    { "KEY_P", KEY_P },
    { "KEY_Q", KEY_Q },
    { "KEY_R", KEY_R },
    { "KEY_S", KEY_S },
    { "KEY_T", KEY_T },
    { "KEY_U", KEY_U },
    { "KEY_V", KEY_V },
    { "KEY_W", KEY_W },
    { "KEY_X", KEY_X },
    { "KEY_Y", KEY_Y },
    { "KEY_Z", KEY_Z },
    { "KEY_LEFT_BRACKET", KEY_LEFT_BRACKET },
    { "KEY_BACKSLASH", KEY_BACKSLASH },
    { "KEY_RIGHT_BRACKET", KEY_RIGHT_BRACKET },
    { "KEY_GRAVE", KEY_GRAVE },
    { "KEY_SPACE", KEY_SPACE },
    { "KEY_ESCAPE", KEY_ESCAPE },
    { "KEY_ENTER", KEY_ENTER },
    { "KEY_TAB", KEY_TAB },
    { "KEY_BACKSPACE", KEY_BACKSPACE },
    { "KEY_INSERT", KEY_INSERT },
    { "KEY_DELETE", KEY_DELETE },
    { "KEY_RIGHT", KEY_RIGHT },
    { "KEY_LEFT", KEY_LEFT },
    { "KEY_DOWN", KEY_DOWN },
    { "KEY_UP", KEY_UP },
    { "KEY_PAGE_UP", KEY_PAGE_UP },
    { "KEY_PAGE_DOWN", KEY_PAGE_DOWN },
    { "KEY_HOME", KEY_HOME },
    { "KEY_END", KEY_END },
    { "KEY_CAPS_LOCK", KEY_CAPS_LOCK },
    { "KEY_SCROLL_LOCK", KEY_SCROLL_LOCK },
    { "KEY_NUM_LOCK", KEY_NUM_LOCK },
    { "KEY_PRINT_SCREEN", KEY_PRINT_SCREEN },
    { "KEY_PAUSE", KEY_PAUSE },
    { "KEY_F1", KEY_F1 },
    { "KEY_F2", KEY_F2 },
    { "KEY_F3", KEY_F3 },
    { "KEY_F4", KEY_F4 },
    { "KEY_F5", KEY_F5 },
    { "KEY_F6", KEY_F6 },
    { "KEY_F7", KEY_F7 },
    { "KEY_F8", KEY_F8 },
    { "KEY_F9", KEY_F9 },
    { "KEY_F10", KEY_F10 },
    { "KEY_F11", KEY_F11 },
    { "KEY_F12", KEY_F12 },
    { "KEY_LEFT_SHIFT", KEY_LEFT_SHIFT },
    { "KEY_LEFT_CONTROL", KEY_LEFT_CONTROL },
    { "KEY_LEFT_ALT", KEY_LEFT_ALT },
    { "KEY_LEFT_SUPER", KEY_LEFT_SUPER },
    { "KEY_RIGHT_SHIFT", KEY_RIGHT_SHIFT },
    { "KEY_RIGHT_CONTROL", KEY_RIGHT_CONTROL },
    { "KEY_RIGHT_ALT", KEY_RIGHT_ALT },
    { "KEY_RIGHT_SUPER", KEY_RIGHT_SUPER },
    { "KEY_KB_MENU", KEY_KB_MENU },
    { "KEY_KP_0", KEY_KP_0 },
    { "KEY_KP_1", KEY_KP_1 },
    { "KEY_KP_2", KEY_KP_2 },
    { "KEY_KP_3", KEY_KP_3 },
    { "KEY_KP_4", KEY_KP_4 },
    { "KEY_KP_5", KEY_KP_5 },
    { "KEY_KP_6", KEY_KP_6 },
    { "KEY_KP_7", KEY_KP_7 },
    { "KEY_KP_8", KEY_KP_8 },
    { "KEY_KP_9", KEY_KP_9 },
    { "KEY_KP_DECIMAL", KEY_KP_DECIMAL },
    { "KEY_KP_DIVIDE", KEY_KP_DIVIDE },
    { "KEY_KP_MULTIPLY", KEY_KP_MULTIPLY },
    { "KEY_KP_SUBTRACT", KEY_KP_SUBTRACT },
    { "KEY_KP_ADD", KEY_KP_ADD },
    { "KEY_KP_ENTER", KEY_KP_ENTER },
    { "KEY_KP_EQUAL", KEY_KP_EQUAL },
});

constexpr std::string_view VERBS = "press, release, tap, click, shot, expect or end";

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// The words of a line, without its comment. '\r' is a space: a checkout on
// Windows has CRLF endings (.gitattributes: eol=native).
std::vector<std::string_view> words_of(std::string_view line) {
    const std::size_t hash = line.find('#');
    if (hash != std::string_view::npos) line = line.substr(0, hash);
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && is_space(line[i])) ++i;
        std::size_t end = i;
        while (end < line.size() && !is_space(line[end])) ++end;
        if (end > i) out.push_back(line.substr(i, end - i));
        i = end;
    }
    return out;
}

// Digits and nothing else: no sign, no point, no exponent. Nine at most, so
// it cannot overflow an int on its way to being too big.
std::optional<int> number(std::string_view word) {
    if (word.empty() || word.size() > 9) return std::nullopt;
    int value = 0;
    for (const char c : word) {
        if (c < '0' || c > '9') return std::nullopt;
        value = (value * 10) + (c - '0');
    }
    return value;
}

bool is_lower_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}
bool is_word(char c) { return is_lower_word(c) || (c >= 'A' && c <= 'Z'); }

// A file name every system takes, the same on all of them.
bool valid_shot(std::string_view name) {
    return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, is_lower_word);
}

// `Name`, `game::Name`, `(anonymous namespace)::Name` cannot be written, so a
// plain qualified identifier.
bool valid_class(std::string_view name) {
    if (name.empty()) return false;
    std::size_t i = 0;
    while (true) {
        const std::size_t start = i;
        while (i < name.size() && is_word(name[i])) ++i;
        if (i == start || (name[start] >= '0' && name[start] <= '9')) return false;
        if (i == name.size()) return true;
        if (name.substr(i, 2) != "::") return false;
        i += 2;
    }
}

std::string quoted(std::string_view word) { return "`" + std::string(word) + "`"; }

// What holds a key down, or the button: the line and frame that put it there,
// and whether it was a tap (or a click), which lets go on its own a frame later.
struct Down {
    int line = 0;
    int frame = -1;
    bool tap = false;
};

// The parse in progress. A class only so that each verb is a function with
// the state at hand; nothing outside this file sees it.
class Parser {
public:
    explicit Parser(ParseError &error) : _error(error) {}

    bool line(int number, std::string_view text);
    bool finish(int last_line);
    std::vector<Step> take_steps() { return std::move(_steps); }

private:
    bool fail(std::string reason) {
        _error.line = _line;
        _error.reason = std::move(reason);
        return false;
    }

    bool key_step(Verb verb, std::string_view verb_word,
                  const std::vector<std::string_view> &args);
    bool click(const std::vector<std::string_view> &args);
    bool shot(const std::vector<std::string_view> &args);
    bool expect(const std::vector<std::string_view> &args);
    bool end(const std::vector<std::string_view> &args);
    bool no_input_at_the_start(std::string_view verb_word);

    ParseError &_error;
    std::vector<Step> _steps;
    int _line = 0; // the line being read
    int _frame = 0; // its frame
    int _last_frame = 0;
    int _last_frame_line = 0;
    std::map<int, Down> _down; // key -> what holds it
    std::map<int, Down> _released; // key -> the release that let it go last
    Down _mouse; // the last click
    std::map<std::string, int, std::less<>> _shots; // name -> line
    bool _checks = false;
    int _end_line = 0;
};

bool Parser::no_input_at_the_start(std::string_view verb_word) {
    if (_frame > 0) return true;
    return fail(
        std::string(verb_word) +
        " at frame 0: the first frame has no edges -- rmp::input compares it with "
        "itself -- so nothing can be pressed in it; start at frame 1");
}

bool Parser::key_step(Verb verb, std::string_view verb_word,
                      const std::vector<std::string_view> &args) {
    if (args.empty())
        return fail(std::string(verb_word) + " needs a key, such as KEY_SPACE");
    if (args.size() > 1)
        return fail(std::string(verb_word) + " takes one key; " + quoted(args[1]) +
                    " is extra");
    const std::optional<int> key = key_named(args[0]);
    if (!key)
        return fail(quoted(args[0]) + " is not a key: raylib's names, such as KEY_SPACE");
    if (!no_input_at_the_start(verb_word)) return false;
    const std::string name(args[0]);

    // A tap two or more frames ago is over: it went down and came up.
    const auto it = _down.find(*key);
    if (it != _down.end() && it->second.tap && it->second.frame < _frame - 1)
        _down.erase(it);
    const auto held = _down.find(*key);
    const std::string line_of =
        held != _down.end() ? std::to_string(held->second.line) : "";
    if (verb == Verb::RELEASE) {
        if (held == _down.end())
            return fail("release " + name + ", which is not held: nothing pressed it");
        if (held->second.tap) {
            return fail("release " + name + ", which the tap on line " + line_of +
                        " already lets go of");
        }
        if (held->second.frame == _frame) {
            return fail("release " + name + " at the frame it is pressed, on line " +
                        line_of + ": it never goes down -- a tap is one frame");
        }
        _down.erase(held);
        _released[*key] = Down{ _line, _frame, false };
    } else if (const auto up = _released.find(*key);
               up != _released.end() && up->second.frame == _frame) {
        return fail(name + " is released at this frame, on line " +
                    std::to_string(up->second.line) + ": press it from frame " +
                    std::to_string(_frame + 1) + ", or it never comes up");
    } else if (held != _down.end()) {
        if (!held->second.tap)
            return fail(name + " is already held: pressed on line " + line_of);
        if (held->second.frame == _frame)
            return fail(name + " is already down: tapped on line " + line_of);
        return fail(name + " was tapped at frame " + std::to_string(held->second.frame) +
                    " (line " + line_of + ") and comes up at frame " +
                    std::to_string(_frame) + ": press it from frame " +
                    std::to_string(_frame + 1) + ", or it never comes up");
    } else {
        _down[*key] = Down{ _line, _frame, verb == Verb::TAP };
    }
    _steps.push_back(Step{ .line = _line, .frame = _frame, .verb = verb, .key = *key });
    return true;
}

bool Parser::click(const std::vector<std::string_view> &args) {
    if (args.size() < 2) return fail("click needs x and y, in window pixels");
    if (args.size() > 2)
        return fail("click takes x and y; " + quoted(args[2]) + " is extra");
    const std::optional<int> x = number(args[0]);
    const std::optional<int> y = number(args[1]);
    if (!x)
        return fail(quoted(args[0]) + " is not a coordinate: a whole number of pixels");
    if (!y)
        return fail(quoted(args[1]) + " is not a coordinate: a whole number of pixels");
    if (!no_input_at_the_start("click")) return false;
    if (_mouse.frame == _frame) {
        return fail("the button is already down: clicked on line " +
                    std::to_string(_mouse.line));
    }
    if (_mouse.frame == _frame - 1) {
        return fail("the click on line " + std::to_string(_mouse.line) +
                    " lets go at this frame: click again from frame " +
                    std::to_string(_frame + 1) + ", or the button never comes up");
    }
    _mouse = Down{ _line, _frame, true };
    _steps.push_back(
        Step{ .line = _line, .frame = _frame, .verb = Verb::CLICK, .x = *x, .y = *y });
    return true;
}

bool Parser::shot(const std::vector<std::string_view> &args) {
    if (args.empty())
        return fail("shot needs a name: the golden's file name, without .png");
    if (args.size() > 1)
        return fail("shot takes one name; " + quoted(args[1]) + " is extra");
    if (!valid_shot(args[0])) {
        return fail(
            quoted(args[0]) +
            " is not a shot name: lower case letters, digits and _, it is a file name");
    }
    const auto taken = _shots.find(args[0]);
    if (taken != _shots.end()) {
        return fail("shot " + quoted(args[0]) + " is already taken, on line " +
                    std::to_string(taken->second));
    }
    _shots.emplace(std::string(args[0]), _line);
    _checks = true;
    _steps.push_back(Step{ .line = _line,
                           .frame = _frame,
                           .verb = Verb::SHOT,
                           .name = std::string(args[0]) });
    return true;
}

bool Parser::expect(const std::vector<std::string_view> &args) {
    constexpr std::string_view ONLY = "`expect scene <Class>` is the one there is";
    if (args.empty()) return fail("expect what? " + std::string(ONLY));
    if (args[0] != "scene")
        return fail("cannot expect " + quoted(args[0]) + ": " + std::string(ONLY));
    if (args.size() < 2) return fail("expect scene needs a class name");
    if (args.size() > 2)
        return fail("expect scene takes one class; " + quoted(args[2]) + " is extra");
    if (!valid_class(args[1])) return fail(quoted(args[1]) + " is not a class name");
    _checks = true;
    _steps.push_back(Step{ .line = _line,
                           .frame = _frame,
                           .verb = Verb::EXPECT_SCENE,
                           .name = std::string(args[1]) });
    return true;
}

bool Parser::end(const std::vector<std::string_view> &args) {
    if (!args.empty()) return fail("end takes nothing; " + quoted(args[0]) + " is extra");
    if (!_checks) {
        return fail(
            "the script checks nothing: a shot or an expect before `end`, or it is "
            "not a test");
    }
    _end_line = _line;
    _steps.push_back(Step{ .line = _line, .frame = _frame, .verb = Verb::END });
    return true;
}

bool Parser::line(int number_of_line, std::string_view text) {
    _line = number_of_line;
    const std::vector<std::string_view> words = words_of(text);
    if (words.empty()) return true;
    if (_end_line > 0) {
        return fail("nothing may follow `end`, on line " + std::to_string(_end_line));
    }
    const std::optional<int> frame = number(words[0]);
    if (!frame) return fail("expected a frame number, not " + quoted(words[0]));
    if (*frame > MAX_FRAME) {
        return fail("frame " + std::to_string(*frame) + " is past " +
                    std::to_string(MAX_FRAME) + ", ten minutes at 60 Hz: a typo?");
    }
    if (*frame < _last_frame) {
        return fail("frame " + std::to_string(*frame) + " comes before frame " +
                    std::to_string(_last_frame) + " on line " +
                    std::to_string(_last_frame_line) + ": frames never go backwards");
    }
    _frame = *frame;
    _last_frame = *frame;
    _last_frame_line = _line;
    if (words.size() == 1) {
        return fail("frame " + std::to_string(_frame) +
                    " and nothing to do: " + std::string(VERBS));
    }
    const std::string_view verb = words[1];
    const std::vector<std::string_view> args(words.begin() + 2, words.end());
    if (verb == "press") return key_step(Verb::PRESS, verb, args);
    if (verb == "release") return key_step(Verb::RELEASE, verb, args);
    if (verb == "tap") return key_step(Verb::TAP, verb, args);
    if (verb == "click") return click(args);
    if (verb == "shot") return shot(args);
    if (verb == "expect") return expect(args);
    if (verb == "end") return end(args);
    return fail("unknown verb " + quoted(verb) + ": " + std::string(VERBS));
}

bool Parser::finish(int last_line) {
    if (_end_line > 0) return true;
    _line = last_line;
    return fail("no `end`: the last line of a script is `<frame> end`");
}

} // namespace

bool parse(std::string_view text, Script &out, ParseError &error) {
    Parser parser(error);
    int number_of_line = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t stop = text.find('\n', start);
        if (stop == std::string_view::npos) stop = text.size();
        ++number_of_line;
        if (!parser.line(number_of_line, text.substr(start, stop - start))) return false;
        start = stop + 1;
    }
    if (!parser.finish(number_of_line > 0 ? number_of_line : 1)) return false;
    out.steps = parser.take_steps();
    return true;
}

std::optional<int> key_named(std::string_view name) {
    for (const KeyName &k : KEYS) {
        if (k.name == name) return k.key;
    }
    return std::nullopt;
}

std::string key_name(int key) {
    for (const KeyName &k : KEYS) {
        if (k.key == key) return std::string(k.name);
    }
    return "key " + std::to_string(key);
}

std::string readable_type(std::string_view raw) {
    // MSVC writes it out already: "class `anonymous namespace'::OverScene".
    for (const std::string_view kind :
         { std::string_view("class "), std::string_view("struct ") }) {
        if (raw.starts_with(kind)) {
            std::string out(raw.substr(kind.size()));
            constexpr std::string_view MSVC_ANONYMOUS = "`anonymous namespace'";
            const std::size_t at = out.find(MSVC_ANONYMOUS);
            if (at != std::string::npos)
                out.replace(at, MSVC_ANONYMOUS.size(), "(anonymous namespace)");
            return out;
        }
    }
    // The Itanium ABI (GCC, Clang): `9OverScene` at namespace scope,
    // `N4game10TitleSceneE` inside one, `_GLOBAL__N_1` for an anonymous one.
    std::string_view rest = raw;
    const bool nested = rest.size() > 2 && rest.front() == 'N' && rest.back() == 'E';
    if (nested) rest = rest.substr(1, rest.size() - 2);
    std::string out;
    int parts = 0;
    while (!rest.empty()) {
        std::size_t digits = 0;
        while (digits < rest.size() && rest[digits] >= '0' && rest[digits] <= '9')
            ++digits;
        const std::optional<int> length = number(rest.substr(0, digits));
        if (!length || *length == 0 ||
            static_cast<std::size_t>(*length) > rest.size() - digits) {
            return std::string(raw);
        }
        std::string_view part = rest.substr(digits, static_cast<std::size_t>(*length));
        rest.remove_prefix(digits + part.size());
        if (part == "_GLOBAL__N_1") part = "(anonymous namespace)";
        if (!out.empty()) out += "::";
        out += part;
        ++parts;
    }
    if (parts == 0 || (!nested && parts > 1)) return std::string(raw);
    return out;
}

bool names_class(std::string_view full, std::string_view wanted) {
    if (wanted.empty()) return false;
    if (full == wanted) return true;
    return full.size() > wanted.size() + 2 && full.ends_with(wanted) &&
        full.substr(full.size() - wanted.size() - 2, 2) == "::";
}

} // namespace e2e
