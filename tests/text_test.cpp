// ---------------------------------------------------------------------------
// rmp::Text and rmp::lang (src/rmp/text.cpp): the tables, the lookup and its
// fallbacks, plurals, placeholders, context, the system language.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/text_internal.h"

#include <rmp/text.h>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace d = rmp::lang::detail;

constexpr std::string_view WIDE =
    "key\tcontext\ten\tes\tja\n"
    "# a comment\n"
    "Play\t\tPlay\tJugar\tプレイ\n"
    "Open\tdoor\tOpen\tAbrir\t開ける\n"
    "Open\tfile\tOpen\tAbrir archivo\t\n"
    "{count} gem|{count} gems\t\t{count} gem|{count} gems\t{count} "
    "gema|{count} gemas\t{count}個の宝石\n"
    "Score: {score}\t\tScore: {score}\tPuntos: {score}\t\n"
    "language.name\t\tEnglish\tEspañol\t日本語\n";

constexpr std::string_view RUSSIAN =
    "key\tcontext\ttext\n"
    "{count} gem|{count} gems\t\t{count} самоцвет|{count} "
    "самоцвета|{count} самоцветов|{count} самоцвета\n"
    "Play\t\tИграть\n";

struct Tables {
    Tables() {
        d::reset_for_tests();
        d::set_persist(false);
        d::set_system_locale_for_tests("en");
        d::load_tables({ { {}, std::string(WIDE) }, { "ru", std::string(RUSSIAN) } });
    }
    ~Tables() { d::reset_for_tests(); }
    Tables(const Tables &) = delete;
    Tables &operator=(const Tables &) = delete;
};

std::string say(const rmp::Text &t) { return t.str(); }

} // namespace

TEST_SUITE("text: rmp::Text") {
    TEST_CASE_FIXTURE(Tables, "a literal is translated, a runtime string never") {
        rmp::lang::set("es");
        CHECK(say("Play") == "Jugar");
        const std::string runtime = "Play";
        CHECK(say(runtime) == "Play");
        CHECK(say(std::string_view(runtime)) == "Play");
        CHECK_FALSE(rmp::Text(runtime).translated());
        CHECK(rmp::Text("Play").translated());
    }

    TEST_CASE_FIXTURE(Tables, "what the tables lack is shown as written") {
        rmp::lang::set("es");
        CHECK(say("Quit") == "Quit");
    }

    TEST_CASE_FIXTURE(Tables,
                      "es-MX falls back to es, an unknown language to the default") {
        rmp::lang::set("es-MX");
        CHECK(rmp::lang::current() == "es-MX");
        CHECK(say("Play") == "Jugar");
        rmp::lang::set("xx");
        CHECK(say("Play") == "Play");
        // An empty cell (ja for "Open" in a file) falls back too.
        rmp::lang::set("ja");
        CHECK(say(rmp::Text("Open", { .context = "file" })) == "Open");
    }

    TEST_CASE_FIXTURE(Tables, "a context tells two texts with the same words apart") {
        rmp::lang::set("es");
        CHECK(say(rmp::Text("Open", { .context = "door" })) == "Abrir");
        CHECK(say(rmp::Text("Open", { .context = "file" })) == "Abrir archivo");
        CHECK(say("Open") == "Open"); // no context: another row, and none
        CHECK(rmp::Text("Open", { .context = "door" }).id() != rmp::Text("Open").id());
        CHECK(rmp::Text("Open").id() == "Open");
        CHECK(rmp::Text("Open", { .context = "door" }).context() == "door");
        CHECK(rmp::Text("Open").context().empty());
    }

    TEST_CASE_FIXTURE(Tables, "placeholders are filled, and braces doubled are braces") {
        rmp::lang::set("es");
        CHECK(say(rmp::Text("Score: {score}", { { "score", 42 } })) == "Puntos: 42");
        CHECK(say(rmp::Text("{a} and {b}", { { "a", "x" }, { "b", 2.5 } })) ==
              "x and 2.5");
        CHECK(say(rmp::Text("{{literal}} {missing}", { { "a", 1 } })) ==
              "{literal} {missing}");
        const rmp::Text score("Score: {score}", { { "score", 42 } });
        REQUIRE(score.args().size() == 1);
        CHECK(score.args()[0].name() == "score");
        CHECK(score.args()[0].number() == 42);
        CHECK(rmp::Text("Play").args().empty());
        CHECK(rmp::TextArg("n", 1e8).text() == "100000000");
        CHECK(rmp::TextArg("n", 1378324.5).text() == "1378324.5");
        // The id does not change with the value: focus survives a score.
        CHECK(rmp::Text("Score: {score}", { { "score", 1 } }).id() ==
              rmp::Text("Score: {score}", { { "score", 2 } }).id());
    }

    TEST_CASE_FIXTURE(Tables, "plurals: English and Spanish one|other, Japanese other") {
        const auto gems = [](int n) {
            return say(rmp::Text("{count} gem|{count} gems", { { "count", n } }));
        };
        rmp::lang::set("en");
        CHECK(gems(1) == "1 gem");
        CHECK(gems(0) == "0 gems");
        CHECK(gems(5) == "5 gems");
        rmp::lang::set("es");
        CHECK(gems(1) == "1 gema");
        CHECK(gems(2) == "2 gemas");
        rmp::lang::set("ja");
        CHECK(gems(1) == "1個の宝石");
        CHECK(gems(7) == "7個の宝石");
    }

    TEST_CASE_FIXTURE(Tables, "plurals: Russian one|few|many from a file of its own") {
        rmp::lang::set("ru");
        const auto gems = [](int n) {
            return say(rmp::Text("{count} gem|{count} gems", { { "count", n } }));
        };
        CHECK(gems(1) == "1 самоцвет");
        CHECK(gems(21) == "21 самоцвет");
        CHECK(gems(3) == "3 самоцвета");
        CHECK(gems(5) == "5 самоцветов");
        CHECK(gems(11) == "11 самоцветов");
        CHECK(gems(14) == "14 самоцветов");
        CHECK(say("Play") == "Играть");
    }

    TEST_CASE_FIXTURE(Tables, "a | without a number is a character") {
        rmp::lang::set("en");
        CHECK(say(rmp::Text("A|B", { { "x", "y" } })) == "A|B");
        CHECK(say("A|B") == "A|B");
    }

    TEST_CASE_FIXTURE(Tables, "a runtime string with braces is left alone") {
        const std::string runtime = "{not a placeholder}";
        CHECK(say(runtime) == runtime);
    }

    TEST_CASE("a char array is read up to its NUL") {
        // NOLINTNEXTLINE(modernize-avoid-c-arrays): a char array is what this test is about
        const char buffer[16] = "Hi";
        const rmp::Text t(buffer);
        CHECK(t.key() == "Hi");
    }
}

TEST_SUITE("text: rmp::lang") {
    TEST_CASE_FIXTURE(Tables, "available and name come from the tables") {
        const std::vector<std::string> codes = rmp::lang::available();
        CHECK(codes == std::vector<std::string>{ "en", "es", "ja", "ru" });
        CHECK(rmp::lang::name("es") == "Español");
        CHECK(rmp::lang::name("ja") == "日本語");
        CHECK(rmp::lang::name("ru") == "ru"); // no row in its file
    }

    TEST_CASE_FIXTURE(Tables, "translate is what rmp::Text draws") {
        rmp::lang::set("es");
        CHECK(rmp::lang::translate("Play") == "Jugar");
        CHECK(rmp::lang::translate("Open", "door") == "Abrir");
    }

    TEST_CASE_FIXTURE(Tables, "the starting language is the system's") {
        d::reset_for_tests();
        d::set_persist(false);
        d::set_system_locale_for_tests("es_MX.UTF-8");
        d::load_tables({ { {}, std::string(WIDE) } });
        CHECK(rmp::lang::current() == "es-MX");
        CHECK(say("Play") == "Jugar");
    }

    TEST_CASE_FIXTURE(Tables, "no system language is the default") {
        d::set_system_locale_for_tests("C");
        CHECK(d::starting_language() == RMP_LANG_DEFAULT);
    }

    TEST_CASE_FIXTURE(Tables, "a missing text is collected once, with its context") {
        rmp::lang::set("es");
        (void)say("Nowhere");
        (void)say("Nowhere");
        (void)say(rmp::Text("Nowhere", { .context = "map" }));
        (void)say("Play"); // in the tables: not collected
        const std::vector<std::string> seen = d::missing_seen();
        CHECK(seen.size() == 2);
        CHECK(seen[0] == "Nowhere\t");
        CHECK(seen[1] == "Nowhere\tmap");
    }
}

TEST_SUITE("text: the pieces") {
    TEST_CASE("normalize turns a locale into a code") {
        CHECK(d::normalize("es_MX.UTF-8") == "es-MX");
        CHECK(d::normalize("es-mx") == "es-MX");
        CHECK(d::normalize("de_DE@euro") == "de-DE");
        CHECK(d::normalize("EN") == "en");
        CHECK(d::normalize("zh-hant-tw") == "zh-Hant-TW");
        CHECK(d::normalize("C").empty());
        CHECK(d::normalize("POSIX").empty());
        CHECK(d::normalize("").empty());
        CHECK(d::normalize("x").empty());
        CHECK(d::normalize("e$").empty());
    }

    TEST_CASE("the fallback chain walks down to the default, each once") {
        CHECK(d::fallback_chain("es-MX", "en") ==
              std::vector<std::string>{ "es-MX", "es", "en" });
        CHECK(d::fallback_chain("en-GB", "en") ==
              std::vector<std::string>{ "en-GB", "en" });
        CHECK(d::fallback_chain("", "en") == std::vector<std::string>{ "en" });
    }

    TEST_CASE("TSV: escapes, comments, CRLF and a byte order mark") {
        const auto rows = d::parse_tsv("\xEF\xBB\xBF"
                                       "a\\tb\tc\\nd\te\\\\f\r\n#skip\n\nlast\\x");
        REQUIRE(rows.size() == 2);
        CHECK(rows[0] == std::vector<std::string>{ "a\tb", "c\nd", "e\\f" });
        CHECK(rows[1] == std::vector<std::string>{ "last\\x" });
        CHECK(d::parse_tsv(d::escape_cell("tab\there\nline\\slash"))[0][0] ==
              "tab\there\nline\\slash");
    }

    TEST_CASE("plural rules per language") {
        CHECK(d::plural_count("en") == 2);
        CHECK(d::plural_count("ja") == 1);
        CHECK(d::plural_count("ru") == 4);
        CHECK(d::plural_count("xx") == 0);
        CHECK(d::plural_form("en", 1, true) == 0);
        CHECK(d::plural_form("en", 1.5, false) == 1);
        CHECK(d::plural_form("fr", 0, true) == 0);
        CHECK(d::plural_form("fr", 1.5, false) == 0);
        CHECK(d::plural_form("fr", 2, true) == 1);
        CHECK(d::plural_form("pt-BR", 1, true) == 0);
        CHECK(d::plural_form("pl", 1, true) == 0);
        CHECK(d::plural_form("pl", 22, true) == 1);
        CHECK(d::plural_form("pl", 25, true) == 2);
        CHECK(d::plural_form("pl", 21, true) == 2);
        CHECK(d::plural_form("pl", 1.5, false) == 3);
        CHECK(d::plural_form("cs", 1, true) == 0);
        CHECK(d::plural_form("cs", 3, true) == 1);
        CHECK(d::plural_form("cs", 5, true) == 3);
        CHECK(d::plural_form("cs", 0.5, false) == 2);
        CHECK(d::plural_form("ru", 111, true) == 2);
        CHECK(d::plural_form("ru", 101, true) == 0);
        CHECK(d::plural_form("ru", 2.5, false) == 3);
        CHECK(d::plural_form("xx", 1, true) == -1);
        CHECK(d::plural_form("en", std::numeric_limits<double>::quiet_NaN(), false) ==
              -1);
    }

    TEST_CASE("an unknown language takes the last form, other") {
        CHECK(d::format("{n} one|{n} other", std::vector<rmp::TextArg>{ { "n", 1 } },
                        "xx") == "1 other");
    }

    TEST_CASE("numbers are written short and exact") {
        CHECK(rmp::TextArg("n", 3).text() == "3");
        CHECK(rmp::TextArg("n", -7L).text() == "-7");
        CHECK(rmp::TextArg("n", 2.5).text() == "2.5");
        CHECK(rmp::TextArg("n", 0.1f).text() == "0.1");
        CHECK(rmp::TextArg("n", 1e20).text() == "1e+20");
        CHECK(rmp::TextArg("n", 0.00001).text() == "1e-05");
        CHECK(rmp::TextArg("n", std::numeric_limits<double>::infinity()).text() == "inf");
        CHECK(rmp::TextArg("n", -std::numeric_limits<double>::infinity()).text() ==
              "-inf");
        CHECK(rmp::TextArg("n", std::size_t{ 12 }).text() == "12");
        CHECK(rmp::TextArg("s", "word").text() == "word");
        CHECK_FALSE(rmp::TextArg("s", "word").numeric());
    }

    TEST_CASE("a font per language, and down the chain") {
        // [lang] fonts is empty in the framework's own .toml.
        CHECK(d::font_for("ja").empty());
    }
}
