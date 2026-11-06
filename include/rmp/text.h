#pragma once

// ---------------------------------------------------------------------------
// rmp::Text and rmp::lang -- the game in more than one language.
//
//     rmp::ui::button("Play");                              // translated
//     rmp::ui::text(rmp::Text("{count} gem|{count} gems", { { "count", gems } }));
//     rmp::ui::button(rmp::Text("Open", { .context = "door" }));
//     rmp::lang::set("es");
//
// Every text the framework is handed is an rmp::Text. A literal written in the
// call becomes one by itself, and it is TRANSLATED: its own words are the key
// looked up in the game's tables (resources/lang.tsv, resources/lang/<code>.tsv,
// [lang] in raylib_multiplatform.toml), and what is not there is shown as
// written. A string made at runtime -- a std::string, TextFormat()'s answer --
// is shown as it is and never looked up: it has no stable key. A value that
// changes goes into a placeholder instead, so the key stays the same:
// rmp::Text("Score: {score}", { { "score", score } }).
//
// Placeholders are {name}; {{ and }} are a brace. A text with a number in it
// can have plural forms, separated by |, in the order of the language's CLDR
// categories: English and Spanish one|other, Russian and Polish
// one|few|many|other, Japanese and Chinese other alone. The first number among
// the arguments chooses. The language is the player's system language, the
// one rmp::lang::set() chose (and saved), or [lang] default; es-MX falls back
// to es, then to the default, then to the text as written.
//
// Right-to-left and complex shaping (Arabic, Hebrew, the Indic scripts) are not
// supported: their text is drawn left to right, letter by letter.
// ---------------------------------------------------------------------------

#include <rmp/config.h>

#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace rmp {

// What else a translated text can say: the context that tells two texts with
// the same words apart ("Open" a door, "Open" a file).
struct TextOptions {
    std::string_view context{};
};

// One placeholder's value: a name and a number or a text.
class TextArg {
public:
    template <class N>
        requires(std::is_arithmetic_v<N> && !std::is_same_v<N, bool> && !std::is_same_v<N, char>)
    TextArg(std::string_view name, N value)
        : _name(name), _number(static_cast<double>(value)), _numeric(true),
          _integer(std::is_integral_v<N>), _single(std::is_same_v<N, float>) {}
    TextArg(std::string_view name, std::string_view value) : _name(name), _text(value) {}

    [[nodiscard]] std::string_view name() const { return _name; }
    [[nodiscard]] bool numeric() const { return _numeric; }
    [[nodiscard]] double number() const { return _number; }
    // The value as it is written into the text.
    [[nodiscard]] std::string text() const;

private:
    std::string _name;
    std::string _text;
    double _number = 0;
    bool _numeric = false;
    bool _integer = false;
    bool _single = false; // a float: written as the float it is, 0.1 and not 0.10000000149
};

// A text the framework draws: translated when it was a literal, as it is when
// it was made at runtime.
class Text {
public:
    Text() = default;

    // A literal: translated, and the key is its own words. A reference to the
    // array, so a literal and a runtime string are told apart by their type.
    template <std::size_t N>
    Text(const char (&literal)[N]) // NOLINT(google-explicit-constructor): a literal is a Text
        : _key(literal, length_of(literal)), _translate(true) {}

    // A string made at runtime -- std::string, std::string_view, TextFormat()'s
    // C string -- shown as it is, never looked up.
    template <class S>
        requires(std::convertible_to<const S &, std::string_view> &&
                 !std::is_array_v<S>)
    Text(const S &runtime) // NOLINT(google-explicit-constructor): every string is a Text
        : _key(std::string_view(runtime)) {}

    // Translated, with a context, placeholders, or both.
    Text(std::string_view key, const TextOptions &options);
    Text(std::string_view key, std::initializer_list<TextArg> args,
         const TextOptions &options = {});

    // What the tables are looked up by: the text as the code wrote it.
    [[nodiscard]] std::string_view key() const { return _key; }
    [[nodiscard]] std::string_view context() const { return _context; }
    [[nodiscard]] bool translated() const { return _translate; }
    [[nodiscard]] std::span<const TextArg> args() const { return _args; }

    // The text to show: translated into the current language, its plural form
    // chosen and its placeholders filled.
    [[nodiscard]] std::string str() const;

    // The identity a widget keeps across languages: the key and its context,
    // never the translation -- so focus and hover survive rmp::lang::set().
    [[nodiscard]] std::string id() const;

private:
    // Up to the first NUL: a char array that is not a literal may hold less.
    template <std::size_t N> static std::size_t length_of(const char (&text)[N]) {
        std::size_t i = 0;
        while (i + 1 < N && text[i] != '\0') ++i;
        return i;
    }

    std::string _key;
    std::string _context;
    std::vector<TextArg> _args;
    bool _translate = false;
};

namespace lang {

// Change the language, everywhere at once: the next frame draws in it. "es",
// "es-MX", "ja". Saved, so the next run starts in it. An unknown code shows
// the default language's texts.
void set(std::string_view code);

// The language in use.
[[nodiscard]] std::string current();

// The languages a language menu offers: [lang] languages, in its order, or,
// with no list, every column and file the tables have.
[[nodiscard]] std::vector<std::string> available();

// A language's own name, for that menu: the table's row "language.name" in
// that language's column, or the code when the table has none.
[[nodiscard]] std::string name(std::string_view code);

// The text `key` in the current language, as rmp::Text would draw it.
[[nodiscard]] std::string translate(std::string_view key, std::string_view context = {});

} // namespace lang

} // namespace rmp
