#pragma once

// The insides of rmp/text.h: the tables, the plural rules, the system
// language, and the seams the tests reach.

#include <rmp/text.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rmp::lang::detail {

// One TSV file read into rows of cells. Tab-separated, one row a line (\n,
// \r\n), `#` at the start of a line a comment, blank lines skipped; a cell
// spells a tab, a new line and a backslash \t, \n and \\. Never fails: what
// cannot be read is taken as it is.
std::vector<std::vector<std::string>> parse_tsv(std::string_view text);

// A cell written so parse_tsv() reads it back.
std::string escape_cell(std::string_view cell);

// The tables, merged. `wide` is a table with a column per language (key,
// context, then the codes); `per_language` a table of one language (key,
// context, text), with the code it is for. Later ones win.
struct Source {
    std::string code; // empty: a wide table
    std::string text;
};
void load_tables(const std::vector<Source> &sources);

// The tables again from resources/, as begin_run() and a hot reload read them.
void load_from_resources();

// Once a frame: reads the tables again when a watched one changed (debug, the
// desktop), and hands the UI the font of the language.
void begin_frame();

// The cell for `key` (with `context`) in the language `code`, exactly: no
// fallback. Nothing when the table has no such row or the cell is empty.
std::optional<std::string> cell(std::string_view key, std::string_view context,
                                std::string_view code);

// The languages the tables have, in the order they were read.
std::vector<std::string> table_languages();

// The chain a lookup walks: es-MX, es, then [lang] default -- each once.
std::vector<std::string> fallback_chain(std::string_view code, std::string_view fallback);

// "es_MX.UTF-8", "es-mx", "es_MX@euro" as a code: "es-MX". Empty for "C",
// "POSIX" and what is no language.
std::string normalize(std::string_view locale);

// The CLDR cardinal category of `n` in `code`, as an index into the forms the
// language writes (the order in rmp/text.h), and how many forms it has.
int plural_form(std::string_view code, double n, bool integer);
int plural_count(std::string_view code);

// The same as CLDR names them: "one", "few", "other"... -- for Yarn's [plural]
// markup -- and the ordinal category ("one" for 1st, "two" for 2nd, "few" for
// 3rd, "other"), which only English distinguishes here.
std::string_view plural_category(std::string_view code, double n, bool integer);
std::string_view ordinal_category(std::string_view code, double n);

// Fill {name} from args and choose the plural form; `code` is the language the
// text is in.
std::string format(std::string_view text, std::span<const TextArg> args,
                   std::string_view code);

// The system's language, or what a test says it is.
std::string system_locale();
void set_system_locale_for_tests(std::optional<std::string> locale);

// Whether set() saves the choice (off in the tests), and the language chosen
// at start: saved, then the system's among what the game ships, then default.
void set_persist(bool persist);
std::string starting_language();

// The font [lang] fonts names for `code`, or empty.
std::string font_for(std::string_view code);

// Texts shown that the tables lack (debug, [lang] collect): one row each, and
// written next to the executable at the end of a run.
std::vector<std::string> missing_seen();
void write_seen();

// The pieces rmp::Text and rmp::lang are built from.
std::string number_text_for(double n, bool integer);
std::string translate_with(std::string_view key, std::string_view context,
                           std::span<const TextArg> args);
void set_current(const std::string &code); // and saved, unless set_persist(false)
std::string current_code(); // decided at the first call: starting_language()
std::vector<std::string> split_shipped(); // [lang] languages

// Everything back to nothing loaded, for the tests.
void reset_for_tests();

} // namespace rmp::lang::detail
