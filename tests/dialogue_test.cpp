// ---------------------------------------------------------------------------
// rmp::Dialogue (src/rmp/dialogue/dialogue.cpp): the public face of the Yarn
// machine -- lines with their speakers, markup and tags, options, commands
// and functions of the game's, variables saved and loaded, a line in another
// language, a program as bytes and back, the embedded table.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/dialogue/dialogue_internal.h"
#include "../src/rmp/dialogue/yarn.h"
#include "../src/rmp/text_internal.h"

#include <rmp/dialogue.h>
#include <rmp/save.h>

#include <string>
#include <vector>

namespace {

namespace dd = rmp::dialogue::detail;

constexpr const char SCRIPT[] = R"(title: Start
tags: intro
---
<<declare $gold = 3>>
Mika: Hello, [b]traveller[/b]! #line:hello #mood:happy
You have {$gold} gold.
<<shake 3>>
<<camera close>>
-> Buy a pie <<if $gold >= 2>> #line:buy
    <<set $gold -= 2>>
    Mika: Enjoy! #line:enjoy
-> Leave
    <<jump Bye>>
Gold left {$gold}, lucky {lucky(7)}
===
title: Bye
---
Mika: Goodbye. #line:bye
===
)";

struct Clean {
    Clean() {
        rmp::lang::detail::reset_for_tests();
        rmp::lang::detail::set_persist(false);
        rmp::lang::detail::set_system_locale_for_tests("en");
        rmp::lang::detail::load_tables({});
    }
    ~Clean() { rmp::lang::detail::reset_for_tests(); }
    Clean(const Clean &) = delete;
    Clean &operator=(const Clean &) = delete;
};

} // namespace

TEST_SUITE("dialogue: rmp::Dialogue") {
    TEST_CASE_FIXTURE(Clean, "a conversation, line by line, with its speaker, markup and tags") {
        rmp::Dialogue d = dd::compile_text(SCRIPT);
        for (const std::string &e : d.errors()) MESSAGE(e);
        REQUIRE(d.loaded());
        CHECK(d.errors().empty());
        CHECK(d.has_node("Bye"));
        CHECK_FALSE(d.has_node("Nowhere"));
        CHECK_FALSE(d.running());
        int shakes = 0;
        d.on_command("shake", [&](std::span<const std::string> args) {
            REQUIRE(args.size() == 1);
            CHECK(args[0] == "3");
            ++shakes;
        });
        d.on_function("lucky", [](rmp::DialogueValue &out, std::span<const rmp::DialogueValue> a) {
            out = a[0].as_number() == 7;
        });
        REQUIRE(d.start());
        CHECK(d.running());
        CHECK(d.node() == "Start");
        REQUIRE(d.has_line());
        CHECK(d.line().id == "line:hello");
        CHECK(d.line().speaker == "Mika");
        CHECK(d.line().text == "Hello, traveller!");
        REQUIRE(d.line().markup.size() == 1);
        CHECK(d.line().markup[0].name == "b");
        CHECK(d.line().markup[0].position == 7);
        CHECK(d.line().markup[0].length == 9);
        CHECK(d.line().tags == std::vector<std::string>{ "line:hello", "mood:happy" });
        d.advance();
        CHECK(d.line().speaker.empty());
        CHECK(d.line().text == "You have 3 gold.");
        d.advance();
        // shake was handled by the game; camera was not, and waits.
        CHECK(shakes == 1);
        REQUIRE(d.has_command());
        CHECK(d.command() == "camera close");
        CHECK_FALSE(d.has_line());
        d.advance();
        REQUIRE(d.has_options());
        REQUIRE(d.options().size() == 2);
        CHECK(d.options()[0].text == "Buy a pie");
        CHECK(d.options()[0].available);
        CHECK(d.options()[1].text == "Leave");
        d.choose(0);
        CHECK(d.line().text == "Enjoy!");
        d.advance();
        CHECK(d.line().text == "Gold left 1, lucky True");
        CHECK(d.variable("$gold").as_number() == 1);
        d.advance();
        CHECK_FALSE(d.running());
        CHECK_FALSE(d.has_line());
        CHECK(d.line().text.empty());
        CHECK(d.options().empty());
        CHECK(d.visits("Start") == 1);
    }

    TEST_CASE_FIXTURE(Clean, "a jump, and an option that cannot be chosen") {
        rmp::Dialogue d = dd::compile_text(SCRIPT);
        d.set_variable("$gold", 1);
        d.start();
        while (d.has_line() || d.has_command()) d.advance();
        REQUIRE(d.has_options());
        CHECK_FALSE(d.options()[0].available);
        d.choose(0); // not available: still waiting
        CHECK(d.has_options());
        d.choose(1);
        CHECK(d.line().text == "Goodbye.");
        CHECK(d.node() == "Bye");
        d.advance();
        CHECK_FALSE(d.running());
    }

    TEST_CASE_FIXTURE(Clean, "variables saved and loaded with rmp::Value") {
        rmp::Dialogue d = dd::compile_text(SCRIPT);
        d.set_variable("$gold", 9);
        d.set_variable("$name", std::string("Ana"));
        d.set_variable("$met", true);
        rmp::Value saved;
        d.save(saved);
        CHECK(saved["$gold"].as_float() == 9);
        CHECK(saved["$name"].as_string() == "Ana");
        CHECK(saved["$met"].as_bool());
        rmp::Dialogue other = dd::compile_text(SCRIPT);
        other.load(saved);
        CHECK(other.variable("$gold").as_number() == 9);
        CHECK(other.variable("$name").as_string() == "Ana");
        CHECK(other.variable("$met").as_bool());
    }

    TEST_CASE_FIXTURE(Clean, "a line in another language, by its id") {
        rmp::lang::detail::load_tables(
            { { {}, "key\tcontext\ten\tes\nline:hello\t\t\tMika: ¡Hola, [b]viajero[/b]!\n"
                    "line:buy\t\t\tComprar un pastel\n" } });
        rmp::lang::set("es");
        rmp::Dialogue d = dd::compile_text(SCRIPT);
        d.start();
        CHECK(d.line().speaker == "Mika");
        CHECK(d.line().text == "¡Hola, viajero!");
        CHECK(d.line().markup[0].position == 7);
        d.advance();
        CHECK(d.line().text == "You have 3 gold."); // not in the table: the .yarn's
    }

    TEST_CASE_FIXTURE(Clean, "compile errors say where") {
        rmp::Dialogue d = dd::compile_text("title: Start\n---\n<<set $x = >>\n===\n", "bad.yarn");
        CHECK_FALSE(d.loaded());
        REQUIRE_FALSE(d.errors().empty());
        CHECK(d.errors()[0].starts_with("bad.yarn:3:"));
        CHECK_FALSE(d.start());
    }

    TEST_CASE_FIXTURE(Clean, "a program as bytes runs the same, and damaged bytes are refused") {
        const std::vector<rmp::yarn::Source> sources{ { "s.yarn", SCRIPT } };
        const rmp::yarn::Compiled c = rmp::yarn::compile(sources);
        REQUIRE(c.ok());
        std::vector<std::uint8_t> bytes = rmp::yarn::to_bytes(c.program);
        rmp::Dialogue d = dd::from_program(bytes);
        REQUIRE(d.loaded());
        d.start();
        CHECK(d.line().text == "Hello, traveller!");
        for (std::size_t cut : { std::size_t{ 0 }, std::size_t{ 7 }, bytes.size() / 2, bytes.size() - 1 }) {
            CHECK_FALSE(rmp::yarn::from_bytes(std::span(bytes).first(cut)).has_value());
        }
        bytes.push_back(0);
        CHECK_FALSE(rmp::yarn::from_bytes(bytes).has_value());
        CHECK_FALSE(dd::from_program(std::span(bytes).first(9)).loaded());
    }

    TEST_CASE_FIXTURE(Clean, "load_dialogue takes the embedded program by its name first") {
        const std::vector<rmp::yarn::Source> sources{ { "s.yarn", SCRIPT } };
        const std::vector<std::uint8_t> bytes = rmp::yarn::to_bytes(rmp::yarn::compile(sources).program);
        const std::vector<dd::Embedded> table{ { "talk.yarn", bytes } };
        dd::set_embedded(table);
        rmp::Dialogue d = rmp::assets::load_dialogue("talk.yarn");
        dd::set_embedded({});
        REQUIRE(d.loaded());
        d.start("Bye");
        CHECK(d.line().text == "Goodbye.");
    }

    TEST_CASE_FIXTURE(Clean, "a file that is not there is an error, not a crash") {
        rmp::Dialogue d = rmp::assets::load_dialogue("no_such_dialogue.yarn");
        CHECK_FALSE(d.loaded());
        REQUIRE(d.errors().size() == 1);
        CHECK_FALSE(d.start());
        d.advance();
        d.choose(0);
        CHECK_FALSE(d.running());
    }

    TEST_CASE_FIXTURE(Clean, "saliency: the first, or the least seen") {
        constexpr const char GROUPS[] = "title: Start\n---\n=> A\n=> B\n===\n";
        rmp::Dialogue d = dd::compile_text(GROUPS);
        d.set_saliency(rmp::DialogueSaliency::FIRST);
        d.start();
        CHECK(d.line().text == "A");
        d.start();
        CHECK(d.line().text == "A");
        rmp::Dialogue e = dd::compile_text(GROUPS);
        e.set_saliency(rmp::DialogueSaliency::LEAST_SEEN);
        e.start();
        const std::string first = e.line().text;
        e.start();
        CHECK(e.line().text != first);
        e.set_saliency(rmp::DialogueSaliency::BEST);
        e.set_saliency(rmp::DialogueSaliency::RANDOM_LEAST_SEEN);
        e.start();
        CHECK(e.has_line());
    }

    TEST_CASE("values convert as Yarn converts them") {
        CHECK(rmp::DialogueValue(2.5).as_string() == "2.5");
        CHECK(rmp::DialogueValue(2).as_string() == "2");
        CHECK(rmp::DialogueValue(true).as_string() == "True");
        CHECK(rmp::DialogueValue("12").as_number() == 12);
        CHECK(rmp::DialogueValue("TRUE").as_bool());
        CHECK_FALSE(rmp::DialogueValue(0.0f).as_bool());
        CHECK(rmp::DialogueValue(false).as_number() == 0);
        CHECK(rmp::DialogueValue(std::string("x")).kind() == rmp::DialogueValue::Kind::STRING);
        rmp::DialogueMarkup m{ "wave", 0, 3, { { "speed", "2" } } };
        CHECK(m.property("speed") == "2");
        CHECK(m.property("none").empty());
    }
}
