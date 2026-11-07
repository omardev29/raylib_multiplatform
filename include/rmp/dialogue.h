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
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace rmp {

class Value; // rmp/save.h: what save() writes and load() reads

// A value of the Yarn language: a number, a string or a bool.
class DialogueValue {
public:
    // Which of Yarn's three types it holds.
    enum class Kind : std::uint8_t {
        NUMBER, // a number; Yarn has one kind, a float
        STRING, // a piece of text
        BOOL, // true or false
    };

    DialogueValue() = default; // the number 0
    // Each one implicit, so `out = 3;` or `set_variable("$met", true)` reads as Yarn does.
    // NOLINTNEXTLINE(google-explicit-constructor)
    DialogueValue(float number) : _number(number) {} // a number
    // NOLINTNEXTLINE(google-explicit-constructor)
    DialogueValue(int number)
        : _number(static_cast<float>(number)) {} // a number, as a float
    // NOLINTNEXTLINE(google-explicit-constructor)
    DialogueValue(double number)
        : _number(static_cast<float>(number)) {} // a number, as a float
    // NOLINTNEXTLINE(google-explicit-constructor)
    DialogueValue(bool value) : _kind(Kind::BOOL), _bool(value) {} // a bool
    // NOLINTNEXTLINE(google-explicit-constructor)
    DialogueValue(std::string text)
        : _kind(Kind::STRING), _text(std::move(text)) {} // a string
    // A string from a literal: "Mika", not a pointer.
    template <std::size_t N>
    // NOLINTNEXTLINE(google-explicit-constructor,modernize-avoid-c-arrays)
    DialogueValue(const char (&text)[N]) : _kind(Kind::STRING), _text(text) {}

    [[nodiscard]] Kind kind() const { return _kind; } // which type it holds
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
    std::string name; // "wave", "color", "speed"...: what is in the brackets first
    int position = 0; // the first character it covers, in code points of `text`
    int length = 0; // how many it covers; 0 for a self-closing [pause=0.4/]
    std::vector<std::pair<std::string, std::string>>
        properties; // name = value, as written

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
    std::string id; // its #line: id, the key of its translation
    std::string text; // in the current language, without markup
    bool available = true; // false: its condition failed; shown disabled, or not at all
    std::vector<DialogueMarkup> markup; // where the markup was, in `text`
    std::vector<std::string> tags; // its #hashtags, without the #
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

// What a typed callback takes, read off its parameters: a number, a string,
// a bool, or anything (a DialogueValue).
enum class DialogueType : std::uint8_t { NUMBER, STRING, BOOL, ANY };
struct DialogueSignature {
    std::vector<DialogueType> params;
};

template <class T> constexpr DialogueType dialogue_type() {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, bool>) return DialogueType::BOOL;
    else if constexpr (std::is_arithmetic_v<U>) return DialogueType::NUMBER;
    else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>)
        return DialogueType::STRING;
    else {
        static_assert(std::is_same_v<U, DialogueValue>,
                      "a Yarn function or command takes numbers, bools, std::string, "
                      "std::string_view or rmp::DialogueValue");
        return DialogueType::ANY;
    }
}

// A function's argument in the type its parameter asks for.
template <class T> auto dialogue_arg(const DialogueValue &v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, bool>) return v.as_bool();
    else if constexpr (std::is_arithmetic_v<U>) return static_cast<U>(v.as_number());
    else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>)
        return v.as_string(); // a std::string_view parameter reads it while the call lasts
    else return v;
}

// The parameters and the result of a lambda (or any type with one
// operator()), mutable or not.
template <class F> struct DialogueLambda : DialogueLambda<decltype(&F::operator())> {};
template <class C, class R, class... A> struct DialogueLambda<R (C::*)(A...) const> {
    using Result = R;
    using Args = std::tuple<A...>;
    static constexpr std::size_t ARITY = sizeof...(A);
};
template <class C, class R, class... A>
struct DialogueLambda<R (C::*)(A...)> : DialogueLambda<R (C::*)(A...) const> {};

template <class... A>
DialogueSignature dialogue_signature(std::type_identity<std::tuple<A...>> /*unused*/) {
    return { { dialogue_type<A>()... } };
}

template <class Fn, class... A, std::size_t... I>
decltype(auto) dialogue_apply(Fn &fn, std::span<const DialogueValue> in,
                              std::index_sequence<I...> /*unused*/,
                              std::type_identity<std::tuple<A...>> /*unused*/) {
    return fn(dialogue_arg<A>(I < in.size() ? in[I] : DialogueValue{})...);
}

// A function's result as a Yarn value.
template <class R> DialogueValue dialogue_result(R &&r) {
    using U = std::remove_cvref_t<R>;
    static_assert(!std::is_void_v<U>, "a Yarn function returns a value: a number, a "
                                      "bool, a string or an rmp::DialogueValue");
    if constexpr (std::is_same_v<U, bool> || std::is_same_v<U, DialogueValue>) return r;
    else if constexpr (std::is_arithmetic_v<U>) return static_cast<double>(r);
    else return std::string(r);
}
} // namespace detail

// A conversation: a compiled Yarn script, where it is in it, and what it
// remembers. It draws nothing -- the presenters of rmp/ui.h do, or the game.
// Moved, never copied.
class Dialogue {
public:
    Dialogue(); // no script: loaded() is false
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

    // <<name arg arg>> calls `handler`, and the dialogue goes on by itself --
    // or waits, when the handler calls wait() or wait_until(). A command
    // nobody handles waits in has_command().
    //
    // Typed: each word after the name arrives as the parameter asks, and a
    // script that gives the wrong number of words, or a word that is not a
    // number where one goes, is a mistake said once, the command not run:
    //     talk.on_command("walk", [&](std::string who, float speed) { ... });
    //     talk.on_command("shake", [&] { camera.shake(); });
    // Or every word as written: [&](std::span<const std::string> words) { ... }.
    template <class F> void on_command(std::string_view name, F &&handler) {
        using Fn = std::decay_t<F>;
        if constexpr (std::is_invocable_v<Fn &, std::span<const std::string>>) {
            raw_command(name, rmp::Callback<std::span<const std::string>>(
                                  static_cast<F &&>(handler)),
                        std::nullopt);
        } else {
            using L = detail::DialogueLambda<Fn>;
            raw_command(name,
                        [fn = Fn(static_cast<F &&>(handler))](
                            std::span<const std::string> words) mutable {
                            std::vector<DialogueValue> in(words.begin(), words.end());
                            detail::dialogue_apply(
                                fn, in, std::make_index_sequence<L::ARITY>{},
                                std::type_identity<typename L::Args>{});
                        },
                        detail::dialogue_signature(std::type_identity<typename L::Args>{}));
        }
    }

    // name(...) in an expression. Typed: the parameters say what it takes and
    // the result what it gives, and a call with the wrong number or kind of
    // arguments is a mistake said once, never a call:
    //     talk.on_function("has_item", [&](std::string item) -> bool { ... });
    // Or untyped: [&](rmp::DialogueValue &out, std::span<const rmp::DialogueValue> args).
    // Write the result type (-> bool): `rmp yarn ysls` reads it for the editor.
    template <class F> void on_function(std::string_view name, F &&function) {
        using Fn = std::decay_t<F>;
        if constexpr (std::is_invocable_v<Fn &, DialogueValue &,
                                          std::span<const DialogueValue>>) {
            raw_function(name,
                         rmp::Callback<DialogueValue &, std::span<const DialogueValue>>(
                             static_cast<F &&>(function)),
                         std::nullopt);
        } else {
            using L = detail::DialogueLambda<Fn>;
            raw_function(
                name,
                [fn = Fn(static_cast<F &&>(function))](
                    DialogueValue &out, std::span<const DialogueValue> in) mutable {
                    out = detail::dialogue_result(detail::dialogue_apply(
                        fn, in, std::make_index_sequence<L::ARITY>{},
                        std::type_identity<typename L::Args>{}));
                },
                detail::dialogue_signature(std::type_identity<typename L::Args>{}));
        }
    }

    // From inside a command's handler: the dialogue waits after the command,
    // for `seconds`, or until `done()` is true (asked once a frame, from
    // waiting()). <<wait 2>> waits by itself, when the game has no "wait".
    //     talk.on_command("walk", [&](std::string where) {
    //         mika.walk_to(where);
    //         talk.wait_until([&] { return mika.arrived(); });
    //     });
    void wait(float seconds);
    template <class F> void wait_until(F &&done) {
        hold_until(rmp::Callback<bool &>(
            [fn = std::decay_t<F>(static_cast<F &&>(done))](bool &out) mutable {
                out = static_cast<bool>(fn());
            }));
    }
    // Whether a command holds the dialogue: no line, no options, until its
    // wait is over. Asking is what moves it on: once a frame, the presenters
    // do it, or the game's own loop.
    bool waiting();

    // $variables: what the script set, and what the game sets for it.
    [[nodiscard]] DialogueValue variable(std::string_view name) const;
    void set_variable(std::string_view name, const DialogueValue &value);

    // A $variable that IS one of the game's: the script reads it and writes
    // it, and nothing is copied. The script declares it, with its type
    // (<<declare $gold = 0>>); a binding of another type, or of a variable the
    // script does not declare, is a mistake said once. save() leaves bound
    // variables out: they are the game's to save. The variable must outlive
    // the dialogue.
    //     talk.bind("$gold", player.gold);
    void bind(std::string_view name, int &variable);
    void bind(std::string_view name, float &variable);
    void bind(std::string_view name, bool &variable);
    void bind(std::string_view name, std::string &variable);
    // Or through two functions, for a value the game works out.
    void bind(std::string_view name, rmp::Callback<DialogueValue &> get,
              rmp::Callback<const DialogueValue &> set);
    [[nodiscard]] int visits(std::string_view node) const; // visited_count()

    // Everything the conversation remembers -- variables, visits, what was
    // seen once -- into an rmp::Value for rmp::save, and back.
    void save(rmp::Value &out) const;
    void load(const rmp::Value &in);

    // How line groups and node groups choose; LEAST_SEEN unless set.
    void set_saliency(DialogueSaliency saliency);

    // The script's own: for the loaders below.
    explicit Dialogue(std::unique_ptr<detail::DialogueState> state);

private:
    void raw_command(std::string_view name, rmp::Callback<std::span<const std::string>> handler,
                     std::optional<detail::DialogueSignature> signature);
    void raw_function(std::string_view name,
                      rmp::Callback<DialogueValue &, std::span<const DialogueValue>> function,
                      std::optional<detail::DialogueSignature> signature);
    void hold_until(rmp::Callback<bool &> done);
    void bind_kind(std::string_view name, DialogueValue::Kind kind,
                   rmp::Callback<DialogueValue &> get,
                   rmp::Callback<const DialogueValue &> set);

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
