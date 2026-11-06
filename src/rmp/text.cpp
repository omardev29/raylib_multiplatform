// ===========================================================================
// rmp::Text and rmp::lang -- see include/rmp/text.h.
//
// The tables are read once, at the first text drawn (or at start), from
// resources/: [lang] file, a table with a column per language, and every
// [lang] folder/<code>.tsv, one per language. A row's key is its context and
// its text; a lookup walks es-MX, es, [lang] default, and ends at the text as
// the code wrote it. Translated every time it is drawn, so rmp::lang::set()
// changes every screen at once.
// ===========================================================================

#include "internal.h"
#include "text_internal.h"

#include <rmp/assets.h>
#include <rmp/config.h>
#include <rmp/save.h>
#include <rmp/text.h>

#include <raylib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif
#if defined(__ANDROID__)
#include <android/configuration.h>
#include <android_native_app_glue.h>
extern "C" struct android_app *GetAndroidApp(void);
#endif
#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif
#if defined(_WIN32)
// windows.h is not included: it declares CloseWindow() and DrawText() as
// raylib does. The one function this needs, as kernel32 exports it.
extern "C" __declspec(dllimport) int __stdcall GetUserDefaultLocaleName(wchar_t *name,
                                                                        int size);
#endif

namespace rmp::lang::detail {
namespace {

// The separator between a row's context and its key: the one gettext uses,
// which no text contains.
constexpr char CONTEXT_MARK = '\x04';
constexpr std::string_view SAVE_SLOT = "rmp_lang";

std::string row_key(std::string_view key, std::string_view context) {
    std::string out(context);
    out += CONTEXT_MARK;
    out += key;
    return out;
}

struct Watched {
    int index = -1;
    unsigned generation = 0;
};

struct {
    bool loaded = false;
    std::vector<std::string> codes; // the languages the tables have
    std::unordered_map<std::string, std::vector<std::string>> rows; // by code index
    std::string current;
    bool chosen = false; // current has been decided
    bool persist = true;
    std::optional<std::string> test_locale;
    std::vector<Watched> watched;
    std::set<std::string> missing; // row keys shown that no table has
    std::string font; // the font begin_frame() last handed the UI
} lang;

int code_index(std::string_view code) {
    for (std::size_t i = 0; i < lang.codes.size(); ++i) {
        if (lang.codes[i] == code) return static_cast<int>(i);
    }
    return -1;
}

int add_code(std::string_view code) {
    const int at = code_index(code);
    if (at >= 0) return at;
    lang.codes.emplace_back(code);
    return static_cast<int>(lang.codes.size()) - 1;
}

void put(std::string_view key, std::string_view context, int column, std::string text) {
    if (key.empty() || column < 0) return;
    std::vector<std::string> &row = lang.rows[row_key(key, context)];
    if (row.size() <= static_cast<std::size_t>(column)) {
        row.resize(static_cast<std::size_t>(column) + 1);
    }
    row[static_cast<std::size_t>(column)] = std::move(text);
}

void read_wide(std::string_view text) {
    const auto rows = parse_tsv(text);
    if (rows.empty()) return;
    // The header: key, context, then one code a column.
    std::vector<int> columns;
    for (std::size_t c = 2; c < rows[0].size(); ++c) {
        const std::string code = normalize(rows[0][c]);
        columns.push_back(code.empty() ? -1 : add_code(code));
    }
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto &row = rows[r];
        if (row.empty()) continue;
        const std::string_view context = row.size() > 1 ? std::string_view(row[1]) : "";
        for (std::size_t c = 2; c < row.size() && c - 2 < columns.size(); ++c) {
            if (!row[c].empty()) put(row[0], context, columns[c - 2], row[c]);
        }
    }
}

void read_one(std::string_view code, std::string_view text) {
    const auto rows = parse_tsv(text);
    const int column = add_code(normalize(code));
    // The header (key, context, text) is the first row; a file with no
    // header is read whole.
    const bool header = !rows.empty() && !rows[0].empty() && rows[0][0] == "key";
    for (std::size_t r = header ? 1 : 0; r < rows.size(); ++r) {
        const auto &row = rows[r];
        if (row.size() < 3 || row[2].empty()) continue;
        put(row[0], row[1], column, row[2]);
    }
}

std::vector<std::string> split(std::string_view text, char by) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find(by, start);
        const std::string_view part =
            text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
        if (!part.empty()) out.emplace_back(part);
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

// The codes a game ships, from [lang] languages.
std::vector<std::string> shipped() { return split(RMP_LANG_LANGUAGES, ','); }

// The codes a per-language file is looked for under: the list, else every
// language with plural rules here, else nothing more. A folder cannot be
// listed inside a pack or an APK, so the names are tried.
constexpr std::array KNOWN_CODES{ "en", "es", "fr", "de", "it", "pt", "pt-BR", "nl", "sv", "da",
                                  "nb", "fi", "et", "ca", "el", "hu", "tr", "bg", "ru", "uk",
                                  "be", "pl", "cs", "sk", "ja", "zh", "zh-TW", "ko", "th", "vi",
                                  "id", "ms", "hi", "ro", "hr", "sr", "sl", "lt", "lv", "eu",
                                  "gl" };

std::optional<std::string> read_resource(const std::string &name, std::vector<Watched> &watch) {
    if (!rmp::assets::detail::resource_exists(name.c_str())) return std::nullopt;
    const std::vector<unsigned char> bytes = rmp::assets::load_data(name);
    const int index = rmp::assets::detail::watch_file(name);
    if (index >= 0) watch.push_back({ index, rmp::assets::detail::watch_generation(index) });
    return std::string(bytes.begin(), bytes.end());
}

bool is_debug() { return RMP_PRODUCTION_BUILD == 0; }

void ensure_loaded() {
    if (!lang.loaded) load_from_resources();
}

// The integer part, and what CLDR calls n % 10 and n % 100, of a whole number.
struct Whole {
    long long i;
    long long mod10;
    long long mod100;
};

Whole whole(double n) {
    const auto i = static_cast<long long>(std::fabs(n));
    return { i, i % 10, i % 100 };
}

enum class Rule {
    OTHER, // one form: ja, zh, ko, th, vi, id, ms
    ONE, // one (n is 1) | other: en, de, es, it, nl, ...
    ZERO_ONE, // one (0 <= n < 2) | other: fr, pt, hi
    EAST_SLAVIC, // one | few | many | other: ru, uk, be
    POLISH, // one | few | many | other
    CZECH, // one | few | many | other: cs, sk
    UNKNOWN, // any number of forms: the last one is "other"
};

Rule rule_of(std::string_view code) {
    const std::string_view base = code.substr(0, code.find('-'));
    for (const std::string_view c : { "ja", "zh", "ko", "th", "vi", "id", "ms", "lo", "my", "km" }) {
        if (base == c) return Rule::OTHER;
    }
    for (const std::string_view c : { "en", "de", "es", "it", "nl", "sv", "da", "nb", "nn", "no",
                                      "fi", "et", "ca", "el", "hu", "tr", "bg", "eu", "gl", "af",
                                      "sq", "ka", "kk", "ky", "mn", "ur", "ta", "te", "ml" }) {
        if (base == c) return Rule::ONE;
    }
    for (const std::string_view c : { "fr", "pt", "hi", "bn", "fa" }) {
        if (base == c) return Rule::ZERO_ONE;
    }
    for (const std::string_view c : { "ru", "uk", "be" }) {
        if (base == c) return Rule::EAST_SLAVIC;
    }
    if (base == "pl") return Rule::POLISH;
    if (base == "cs" || base == "sk") return Rule::CZECH;
    return Rule::UNKNOWN;
}

bool few_slavic(const Whole &w) {
    return w.mod10 >= 2 && w.mod10 <= 4 && (w.mod100 < 12 || w.mod100 > 14);
}

std::string number_text(double n, bool integer) {
    std::array<char, 64> buffer{};
    std::to_chars_result done{};
    if (integer && std::isfinite(n) && std::fabs(n) < 9.2e18) {
        done = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                             static_cast<long long>(n));
    } else {
        done = std::to_chars(buffer.data(), buffer.data() + buffer.size(), n);
    }
    if (done.ec != std::errc{}) return {};
    return { buffer.data(), done.ptr };
}

// The forms of `text`, split on | -- only when a number chooses among them.
std::string choose_form(std::string_view text, std::span<const TextArg> args,
                        std::string_view code) {
    const auto number = std::ranges::find_if(args, &TextArg::numeric);
    if (number == args.end() || text.find('|') == std::string_view::npos) return std::string(text);
    std::vector<std::string_view> forms;
    std::size_t start = 0;
    while (true) {
        const std::size_t bar = text.find('|', start);
        forms.push_back(text.substr(start, bar == std::string_view::npos ? text.size() - start
                                                                         : bar - start));
        if (bar == std::string_view::npos) break;
        start = bar + 1;
    }
    const double n = number->number();
    const bool integer = std::isfinite(n) && n == std::floor(n);
    const int form = plural_form(code, n, integer);
    const std::size_t at = form < 0 || static_cast<std::size_t>(form) >= forms.size()
                               ? forms.size() - 1
                               : static_cast<std::size_t>(form);
    return std::string(forms[at]);
}

std::string translated(std::string_view key, std::string_view context,
                       std::span<const TextArg> args) {
    ensure_loaded();
    const std::string code = rmp::lang::current();
    for (const std::string &c : fallback_chain(code, RMP_LANG_DEFAULT)) {
        if (std::optional<std::string> found = cell(key, context, c)) {
            return format(*found, args, c);
        }
    }
    if (is_debug() && RMP_LANG_COLLECT != 0 && !key.empty() &&
        !lang.rows.contains(row_key(key, context))) {
        lang.missing.insert(row_key(key, context));
    }
    return format(key, args, RMP_LANG_DEFAULT);
}

} // namespace

std::vector<std::vector<std::string>> parse_tsv(std::string_view text) {
    std::vector<std::vector<std::string>> rows;
    // A UTF-8 byte order mark, which a spreadsheet may write first.
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        std::vector<std::string> cells(1);
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (c == '\t') {
                cells.emplace_back();
            } else if (c == '\\' && i + 1 < line.size()) {
                const char next = line[++i];
                if (next == 't') {
                    cells.back() += '\t';
                } else if (next == 'n') {
                    cells.back() += '\n';
                } else if (next == '\\') {
                    cells.back() += '\\';
                } else {
                    cells.back() += '\\';
                    cells.back() += next;
                }
            } else {
                cells.back() += c;
            }
        }
        rows.push_back(std::move(cells));
    }
    return rows;
}

std::string escape_cell(std::string_view cell) {
    std::string out;
    out.reserve(cell.size());
    for (const char c : cell) {
        if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c != '\r') {
            out += c;
        }
    }
    return out;
}

void load_tables(const std::vector<Source> &sources) {
    lang.codes.clear();
    lang.rows.clear();
    for (const Source &s : sources) {
        if (s.code.empty()) {
            read_wide(s.text);
        } else {
            read_one(s.code, s.text);
        }
    }
    lang.loaded = true;
}

void load_from_resources() {
    std::vector<Source> sources;
    std::vector<Watched> watch;
    const std::string file = RMP_LANG_FILE;
    if (!file.empty()) {
        if (auto text = read_resource(file, watch)) sources.push_back({ {}, std::move(*text) });
    }
    const std::string folder = RMP_LANG_FOLDER;
    if (!folder.empty()) {
        std::vector<std::string> codes = shipped();
        if (codes.empty()) codes.assign(KNOWN_CODES.begin(), KNOWN_CODES.end());
        for (const std::string &code : codes) {
            if (auto text = read_resource(folder + "/" + code + ".tsv", watch)) {
                sources.push_back({ code, std::move(*text) });
            }
        }
    }
    load_tables(sources);
    lang.watched = std::move(watch);
}

void begin_frame() {
    for (const Watched &w : lang.watched) {
        if (rmp::assets::detail::watch_generation(w.index) != w.generation) {
            load_from_resources();
            break;
        }
    }
}

std::optional<std::string> cell(std::string_view key, std::string_view context,
                                std::string_view code) {
    const int column = code_index(code);
    if (column < 0) return std::nullopt;
    const auto row = lang.rows.find(row_key(key, context));
    if (row == lang.rows.end() || row->second.size() <= static_cast<std::size_t>(column)) {
        return std::nullopt;
    }
    const std::string &text = row->second[static_cast<std::size_t>(column)];
    if (text.empty()) return std::nullopt;
    return text;
}

std::vector<std::string> table_languages() {
    ensure_loaded();
    return lang.codes;
}

std::vector<std::string> fallback_chain(std::string_view code, std::string_view fallback) {
    std::vector<std::string> chain;
    const auto add = [&chain](std::string_view c) {
        if (!c.empty() && std::ranges::find(chain, c) == chain.end()) chain.emplace_back(c);
    };
    std::string_view rest = code;
    while (!rest.empty()) {
        add(rest);
        const std::size_t dash = rest.rfind('-');
        rest = dash == std::string_view::npos ? std::string_view{} : rest.substr(0, dash);
    }
    add(fallback);
    return chain;
}

std::string normalize(std::string_view locale) {
    // Cut the encoding and the modifier: es_MX.UTF-8@euro.
    locale = locale.substr(0, locale.find_first_of(".@"));
    if (locale.empty() || locale == "C" || locale == "POSIX") return {};
    std::string out;
    bool first = true;
    std::size_t part_start = 0;
    for (std::size_t i = 0; i <= locale.size(); ++i) {
        if (i < locale.size() && locale[i] != '_' && locale[i] != '-') continue;
        std::string part(locale.substr(part_start, i - part_start));
        part_start = i + 1;
        if (part.empty()) continue;
        for (char &c : part) {
            const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            const bool digit = c >= '0' && c <= '9';
            if (!alpha && !digit) return {};
            if (first || part.size() != 2) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            } else if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A'); // a region: MX
            }
        }
        if (first && (part.size() < 2 || part.size() > 3)) return {};
        // A script (Hant) keeps its capital.
        if (!first && part.size() == 4) part[0] = static_cast<char>(part[0] - 'a' + 'A');
        if (!first) out += '-';
        out += part;
        first = false;
    }
    return out;
}

int plural_form(std::string_view code, double n, bool integer) {
    if (!std::isfinite(n)) return -1;
    const Whole w = whole(n);
    switch (rule_of(code)) {
        case Rule::OTHER: return 0;
        case Rule::ONE: return integer && w.i == 1 ? 0 : 1;
        case Rule::ZERO_ONE: return w.i <= 1 ? 0 : 1;
        case Rule::EAST_SLAVIC:
            if (!integer) return 3;
            if (w.mod10 == 1 && w.mod100 != 11) return 0;
            return few_slavic(w) ? 1 : 2;
        case Rule::POLISH:
            if (!integer) return 3;
            if (w.i == 1) return 0;
            return few_slavic(w) ? 1 : 2;
        case Rule::CZECH:
            if (!integer) return 2;
            if (w.i == 1) return 0;
            return w.i >= 2 && w.i <= 4 ? 1 : 3;
        case Rule::UNKNOWN: return -1;
    }
    return -1;
}

int plural_count(std::string_view code) {
    switch (rule_of(code)) {
        case Rule::OTHER: return 1;
        case Rule::ONE:
        case Rule::ZERO_ONE: return 2;
        case Rule::EAST_SLAVIC:
        case Rule::POLISH:
        case Rule::CZECH: return 4;
        case Rule::UNKNOWN: return 0;
    }
    return 0;
}

std::string format(std::string_view text, std::span<const TextArg> args, std::string_view code) {
    const std::string chosen = choose_form(text, args, code);
    if (args.empty() && chosen.find_first_of("{}") == std::string::npos) return chosen;
    std::string out;
    out.reserve(chosen.size() + 16);
    for (std::size_t i = 0; i < chosen.size(); ++i) {
        const char c = chosen[i];
        if ((c == '{' || c == '}') && i + 1 < chosen.size() && chosen[i + 1] == c) {
            out += c; // {{ and }}
            ++i;
            continue;
        }
        if (c == '{') {
            const std::size_t close = chosen.find('}', i + 1);
            if (close != std::string::npos) {
                const std::string_view name(chosen.data() + i + 1, close - i - 1);
                const auto arg = std::ranges::find(args, name, &TextArg::name);
                if (arg != args.end()) {
                    out += arg->text();
                    i = close;
                    continue;
                }
            }
        }
        out += c;
    }
    return out;
}

std::string system_locale() {
    if (lang.test_locale) return normalize(*lang.test_locale);
#if defined(__APPLE__)
    std::string found;
    const CFArrayRef preferred = CFLocaleCopyPreferredLanguages();
    if (preferred != nullptr) {
        if (CFArrayGetCount(preferred) > 0) {
            const auto first = static_cast<CFStringRef>(CFArrayGetValueAtIndex(preferred, 0));
            std::array<char, 64> buffer{};
            if (CFStringGetCString(first, buffer.data(), static_cast<CFIndex>(buffer.size()),
                                   kCFStringEncodingUTF8)) {
                found = buffer.data();
            }
        }
        CFRelease(preferred);
    }
    if (!found.empty()) return normalize(found);
#elif defined(__ANDROID__)
    if (android_app *app = GetAndroidApp(); app != nullptr && app->config != nullptr) {
        std::array<char, 2> language{};
        std::array<char, 2> country{};
        AConfiguration_getLanguage(app->config, language.data());
        AConfiguration_getCountry(app->config, country.data());
        std::string code(language.data(), language.size());
        if (country[0] != 0) code += "-" + std::string(country.data(), country.size());
        return normalize(code);
    }
#elif defined(__EMSCRIPTEN__)
    // NOLINTNEXTLINE(custom-rmp-raw-pointer): the C string emscripten hands back, copied at once
    char *said = static_cast<char *>(EM_ASM_PTR({
        const text = (navigator.languages && navigator.languages[0]) || navigator.language || "";
        return stringToNewUTF8(text);
    }));
    if (said != nullptr) {
        std::string code(said);
        std::free(said); // NOLINT(cppcoreguidelines-no-malloc): emscripten's malloc'd copy
        return normalize(code);
    }
#elif defined(_WIN32)
    std::array<wchar_t, 85> name{};
    if (GetUserDefaultLocaleName(name.data(), static_cast<int>(name.size())) > 0) {
        std::string code;
        for (const wchar_t c : name) {
            if (c == 0) break;
            code += c < 128 ? static_cast<char>(c) : '?';
        }
        return normalize(code);
    }
#endif
    for (const char *var : { "LC_ALL", "LC_MESSAGES", "LANG" }) { // NOLINT(custom-rmp-raw-pointer)
        // NOLINTNEXTLINE(custom-rmp-raw-pointer,concurrency-mt-unsafe): read once, copied at once
        const char *value = std::getenv(var);
        if (value != nullptr && *value != '\0') {
            std::string code = normalize(value);
            if (!code.empty()) return code;
        }
    }
    return {};
}

void set_system_locale_for_tests(std::optional<std::string> locale) {
    lang.test_locale = std::move(locale);
    lang.chosen = false;
}

void set_persist(bool persist) { lang.persist = persist; }

std::string starting_language() {
    const std::vector<std::string> list = shipped();
    const auto offered = [&list](std::string_view code) {
        return list.empty() || std::ranges::find(list, code) != list.end();
    };
    if (lang.persist) {
        rmp::Value saved;
        if (rmp::save::read(SAVE_SLOT, saved)) {
            const std::string code = normalize(saved["code"].as_string());
            if (!code.empty() && offered(code)) return code;
        }
    }
    const std::string system = system_locale();
    // es-MX, then es: the first of the chain the game ships. With no list,
    // the system's own code, which the lookup itself walks down.
    if (!system.empty()) {
        if (list.empty()) return system;
        for (const std::string &c : fallback_chain(system, {})) {
            if (offered(c)) return c;
        }
    }
    return RMP_LANG_DEFAULT;
}

std::string font_for(std::string_view code) {
    for (const std::string &pair : split(RMP_LANG_FONTS, ';')) {
        const std::size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        const std::string_view name(pair.data(), eq);
        for (const std::string &c : fallback_chain(code, {})) {
            if (name == c) return pair.substr(eq + 1);
        }
    }
    return {};
}

std::vector<std::string> missing_seen() {
    std::vector<std::string> out;
    for (const std::string &k : lang.missing) {
        const std::size_t mark = k.find(CONTEXT_MARK);
        out.push_back(escape_cell(k.substr(mark + 1)) + "\t" + escape_cell(k.substr(0, mark)));
    }
    return out;
}

void write_seen() {
    if (!is_debug() || RMP_LANG_COLLECT == 0 || lang.missing.empty()) return;
    const std::string path = std::string(GetApplicationDirectory()) + "lang_seen.tsv";
    const std::unique_ptr<std::FILE, int (*)(std::FILE *)> file(std::fopen(path.c_str(), "wb"),
                                                                &std::fclose);
    if (!file) {
        // A condition of the machine (a read-only folder), not the game's
        // mistake: a line in the log, never a report.
        TraceLog(LOG_INFO, "LANG: could not write %s", path.c_str());
        return;
    }
    std::fputs("key\tcontext\n", file.get());
    for (const std::string &row : missing_seen()) {
        std::fputs(row.c_str(), file.get());
        std::fputc('\n', file.get());
    }
    TraceLog(LOG_INFO, "LANG: %d text(s) the tables lack, in %s",
             static_cast<int>(lang.missing.size()), path.c_str());
}

void reset_for_tests() {
    lang.loaded = false;
    lang.codes.clear();
    lang.rows.clear();
    lang.current.clear();
    lang.chosen = false;
    lang.persist = false;
    lang.test_locale.reset();
    lang.watched.clear();
    lang.missing.clear();
    lang.font.clear();
}

std::string number_text_for(double n, bool integer) { return number_text(n, integer); }

std::string translate_with(std::string_view key, std::string_view context,
                           std::span<const TextArg> args) {
    return translated(key, context, args);
}

void set_current(const std::string &code) {
    lang.current = code;
    lang.chosen = true;
    if (!lang.persist) return;
    rmp::Value v;
    v["code"] = code;
    (void)rmp::save::write(SAVE_SLOT, v);
}

std::string current_code() {
    if (!lang.chosen) {
        lang.current = starting_language();
        lang.chosen = true;
    }
    return lang.current;
}

std::vector<std::string> split_shipped() { return shipped(); }

} // namespace rmp::lang::detail

namespace rmp {

std::string TextArg::text() const {
    if (!_numeric) return _text;
    if (_single) {
        std::array<char, 32> buffer{};
        const auto done = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                        static_cast<float>(_number));
        return { buffer.data(), done.ptr };
    }
    return rmp::lang::detail::number_text_for(_number, _integer);
}

Text::Text(std::string_view key, const TextOptions &options)
    : _key(key), _context(options.context), _translate(true) {}

Text::Text(std::string_view key, std::initializer_list<TextArg> args, const TextOptions &options)
    : _key(key), _context(options.context), _args(args), _translate(true) {}

std::string Text::str() const {
    if (!_translate) return _key;
    return rmp::lang::detail::translate_with(_key, _context, _args);
}

std::string Text::id() const {
    if (_context.empty()) return _key;
    std::string out = _context;
    out += '\x04';
    out += _key;
    return out;
}

namespace lang {

void set(std::string_view code) {
    std::string normal = detail::normalize(code);
    if (normal.empty()) normal = RMP_LANG_DEFAULT;
    detail::set_current(normal);
}

std::string current() { return detail::current_code(); }

std::vector<std::string> available() {
    std::vector<std::string> list = detail::split_shipped();
    if (!list.empty()) return list;
    return detail::table_languages();
}

std::string name(std::string_view code) {
    if (auto own = detail::cell("language.name", {}, code)) return *own;
    return std::string(code);
}

std::string translate(std::string_view key, std::string_view context) {
    return detail::translate_with(key, context, {});
}

} // namespace lang

} // namespace rmp
