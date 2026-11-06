#pragma once

// ---------------------------------------------------------------------------
// rmp::Dialogue -- conversations written in Yarn Spinner 3.
//
//     rmp::Dialogue talk = rmp::assets::load_dialogue("wizard.yarn");
//     talk.start("Start");
//     // every frame:
//     if (talk.has_line()) {
//         show(talk.line().speaker, talk.line().text);
//         if (rmp::input::just_pressed("accept")) talk.advance();
//     } else if (talk.has_options()) {
//         for (const rmp::DialogueOption &o : talk.options()) ...;
//         talk.choose(picked);
//     }
//
// The whole of Yarn 3: nodes and their headers, lines with their #line: ids
// and #tags, `Name:` speakers, {$expressions}, [markup], options (->) and
// their conditions, line groups (=>) and node groups (when:) chosen by
// saliency, <<if>>, <<once>>, <<set>>, <<declare>> with types, enums and
// smart variables, <<jump>>, <<detour>> and <<return>>, the built-in
// functions, visit tracking, and the game's own commands and functions.
// Compiled by the framework's own compiler (a port of the official one,
// MIT), when the file is loaded -- or at build time into the game, with
// [dialogue] build = "embedded" -- and run by its machine. Nothing is drawn
// here: rmp::Dialogue is the conversation, and showing it is the game's (or
// the presenters' of rmp::ui).
//
// A line's text is translated like everything else: its #line: id is its key
// in resources/lang.tsv, and the .yarn holds the text in [lang] default.
// `rmp text scan` adds every line to the table.
//
// Dialogue is written in .yarn files only: Yarn reads as a script, and a game
// that needs a value from C++ asks for it with a function or a command.
// ---------------------------------------------------------------------------

#include <rmp/config.h>

// The one header of ours this one includes, and why: on_command() and
// on_function() take an rmp::Callback BY VALUE (rmp/object.h).
#include <rmp/object.h> // rmp::Callback

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rmp {

class Value; // rmp/save.h: what save() writes and load() reads

// A value of the Yarn language: a number, a string or a bool.
class DialogueValue {
public:
    enum class Kind : std::uint8_t { NUMBER, STRING, BOOL };

    DialogueValue() = default;
    // Each one implicit, so `out = 3;` or `set_variable("$met", true)` reads as Yarn does.
    // NOLINTNEXTLINE(google-explicit-constructor)
    DialogueValue(float number) : _number(number) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    DialogueValue(int number) : _number(static_cast<float>(number)) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    DialogueValue(double number) : _number(static_cast<float>(number)) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    DialogueValue(bool value) : _kind(Kind::BOOL), _bool(value) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    DialogueValue(std::string text) : _kind(Kind::STRING), _text(std::move(text)) {}
    template <std::size_t N>
    // NOLINTNEXTLINE(google-explicit-constructor,modernize-avoid-c-arrays)
    DialogueValue(const char (&text)[N]) : _kind(Kind::STRING), _text(text) {}

    [[nodiscard]] Kind kind() const { return _kind; }
    [[nodiscard]] float as_number() const; // a string read as a number; a bool 1 or 0
    [[nodiscard]] bool as_bool() const; // a number not 0; a string "true"
    [[nodiscard]] std::string as_string() const; // as a line shows it: 2, 2.5, True

private:
    Kind _kind = Kind::NUMBER;
    float _number = 0;
    bool _bool = false;
    std::string _text;
};

// A piece of a line's markup: [wave]these words[/wave] is "wave" at the
// place of the first word, for as many characters (code points) as they
// have. Positions are in the line's `text`.
struct DialogueMarkup {
    std::string name;
    int position = 0;
    int length = 0;
    std::vector<std::pair<std::string, std::string>> properties;

    // A property's value, or "" when the markup has none of that name.
    [[nodiscard]] std::string property(std::string_view key) const;
};

// A line of the conversation, ready to show.
struct DialogueLine {
    std::string id; // its #line: id, the key of its translation
    std::string speaker; // who says it ("Mika: ..."), or empty
    std::string
        text; // what is said, in the current language, without the speaker or markup
    std::vector<DialogueMarkup> markup; // where the markup was, in `text`
    std::vector<std::string> tags; // its #hashtags, without the #
};

// One of the choices the player has now.
struct DialogueOption {
    std::string id;
    std::string text; // in the current language, without markup
    bool available = true; // false: its condition failed; shown disabled, or not at all
    std::vector<DialogueMarkup> markup;
    std::vector<std::string> tags;
};

// How a line group (=>) or a node group (when:) picks among what can run.
enum class DialogueSaliency : std::uint8_t {
    FIRST, // the first whose conditions pass
    BEST, // the one with the most conditions that pass
    LEAST_SEEN, // the least seen, the most specific of those: the default
    RANDOM_LEAST_SEEN, // as LEAST_SEEN, a random one among equals
};

namespace detail {
struct DialogueState;
} // namespace detail

class Dialogue {
public:
    Dialogue();
    ~Dialogue();
    Dialogue(Dialogue &&other) noexcept;
    Dialogue &operator=(Dialogue &&other) noexcept;
    Dialogue(const Dialogue &) = delete;
    Dialogue &operator=(const Dialogue &) = delete;

    // Whether a script is loaded and compiled; when not, errors() says why,
    // a line each, as file:line:column: what is wrong.
    [[nodiscard]] bool loaded() const;
    [[nodiscard]] std::span<const std::string> errors() const;
    [[nodiscard]] bool has_node(std::string_view node) const;

    // Run from a node's first line. False when there is no such node.
    bool start(std::string_view node = "Start");
    void stop();
    [[nodiscard]] bool running() const; // started, not yet finished
    [[nodiscard]] std::string node() const; // the node it is in, or empty

    // What it is waiting for: a line to be read, a choice, or a command the
    // game did not handle. Nothing of the three when it is not running.
    [[nodiscard]] bool has_line() const;
    [[nodiscard]] const DialogueLine &line() const; // empty when there is none
    [[nodiscard]] bool has_options() const;
    [[nodiscard]] std::span<const DialogueOption> options() const;
    [[nodiscard]] bool has_command() const;
    [[nodiscard]] std::string_view command() const; // its text, {expressions} filled

    // On, past the line or the command.
    void advance();
    // The option chosen, by its place in options(). When none is available,
    // any number goes on past the options.
    void choose(int index);

    // <<name arg arg>> calls `handler` with the args, and the dialogue goes
    // on by itself. A command nobody handles waits in has_command().
    void on_command(std::string_view name,
                    rmp::Callback<std::span<const std::string>> handler);
    // name(...) in an expression: `function` reads the arguments and writes
    // the result in the value it is handed.
    void
    on_function(std::string_view name,
                rmp::Callback<DialogueValue &, std::span<const DialogueValue>> function);

    // $variables: what the script set, and what the game sets for it.
    [[nodiscard]] DialogueValue variable(std::string_view name) const;
    void set_variable(std::string_view name, const DialogueValue &value);
    [[nodiscard]] int visits(std::string_view node) const; // visited_count()

    // Everything the conversation remembers -- variables, visits, what was
    // seen once -- into an rmp::Value for rmp::save, and back.
    void save(rmp::Value &out) const;
    void load(const rmp::Value &in);

    void set_saliency(DialogueSaliency saliency);

    // The script's own: for the loaders below.
    explicit Dialogue(std::unique_ptr<detail::DialogueState> state);

private:
    std::unique_ptr<detail::DialogueState> _state;
};

namespace assets {
// A .yarn file of resources/, compiled. Its errors, if any, are in
// errors(), said in the log and, in a debug build, on screen.
[[nodiscard]] Dialogue load_dialogue(std::string_view file);
// Several, compiled together, so a jump can go from one to another.
[[nodiscard]] Dialogue load_dialogue(std::span<const std::string_view> files);
} // namespace assets

} // namespace rmp
