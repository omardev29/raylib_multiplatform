// ---------------------------------------------------------------------------
// rmp::Dialogue and the game's C++ (src/rmp/dialogue/dialogue.cpp,
// yarn_vm.cpp): typed functions and commands, checked before they are
// called; commands that wait; $variables that are the game's own; start()
// saying which function nobody registered; the machine's faults reported.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/dialogue/dialogue_internal.h"
#include "../src/rmp/internal.h"

#include <rmp/app.h>
#include <rmp/dialogue.h>
#include <rmp/save.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace dd = rmp::dialogue::detail;

struct {
    std::vector<std::string> lines;
} heard;

// The reports, heard from a clean table, and back to nothing after.
struct Listening {
    Listening() {
        rmp::detail::reset_reports_for_tests();
        heard.lines.clear();
        rmp::detail::set_report_sink(
            [](std::string_view line) { heard.lines.emplace_back(line); });
    }
    Listening(const Listening &) = delete;
    Listening &operator=(const Listening &) = delete;
    ~Listening() {
        rmp::detail::set_report_sink({});
        rmp::detail::reset_reports_for_tests();
    }
};

bool heard_about(std::string_view words) {
    return std::ranges::any_of(heard.lines, [&](const std::string &l) {
        return l.find(words) != std::string::npos;
    });
}

rmp::Dialogue script(std::string_view text) {
    rmp::Dialogue d = dd::compile_text(text);
    for (const std::string &e : d.errors()) MESSAGE(e);
    REQUIRE(d.loaded());
    return d;
}

} // namespace

TEST_SUITE("dialogue: the game's C++") {
    TEST_CASE("a typed function gets its arguments in its own types") {
        rmp::Dialogue d = script("title: Start\n---\n"
                                 "<<if has_item(\"key\", 2)>>\nOpen.\n<<else>>\nShut.\n"
                                 "<<endif>>\n{price(\"pie\")} coins. {shout(\"hi\")}\n"
                                 "{half(5)} {whole(true)}\n===\n");
        std::string asked;
        int count = 0;
        d.on_function("has_item", [&](std::string item, int n) -> bool {
            asked = item;
            count = n;
            return true;
        });
        d.on_function("price",
                      [](std::string_view item) -> int { return item == "pie" ? 3 : 0; });
        d.on_function("shout", [](const std::string &s) { return s + "!"; });
        d.on_function("half", [](double v) { return v / 2; });
        d.on_function("whole", [](bool b) { return rmp::DialogueValue(b); });
        REQUIRE(d.start());
        CHECK(asked == "key");
        CHECK(count == 2);
        CHECK(d.line().text == "Open.");
        d.advance();
        CHECK(d.line().text == "3 coins. hi!");
        d.advance();
        CHECK(d.line().text == "2.5 True");
    }

    TEST_CASE("a call with the wrong count or kind is a mistake, never a call") {
        Listening listening;
        int calls = 0;
        const auto twice = [&](float v) -> float {
            ++calls;
            return v * 2;
        };
        rmp::Dialogue d = script("title: Start\n---\n{twice(1, 2)}\n===\n");
        d.on_function("twice", twice);
        REQUIRE(d.start());
        rmp::Dialogue e = script("title: Start\n---\n{twice(\"x\")}\n===\n");
        e.on_function("twice", twice);
        REQUIRE(e.start());
        CHECK(calls == 0);
        CHECK(heard_about("twice() takes 1 argument, and the script gives it 2"));
        CHECK(heard_about(
            "twice()'s argument 1 is a number, and the script gives it a string"));
        CHECK(std::ranges::any_of(d.errors(), [](const std::string &e) {
            return e.find("twice() takes 1") != std::string::npos;
        }));
    }

    TEST_CASE("start() says which function nobody registered, before any line") {
        Listening listening;
        rmp::Dialogue d = script("title: Start\n---\nHello.\n<<if late()>>\nLate.\n"
                                 "<<endif>>\n{round(2.4)}\n===\n");
        REQUIRE(d.start());
        CHECK(d.line().text == "Hello.");
        CHECK(heard_about("the script calls late(), and nothing registered it"));
        // round() is the language's own.
        CHECK_FALSE(heard_about("round()"));
    }

    TEST_CASE("a typed command gets its words, and refuses the wrong ones") {
        Listening listening;
        rmp::Dialogue d =
            script("title: Start\n---\n<<walk Mika 2.5 true>>\n"
                   "<<walk Mika fast true>>\n<<walk Mika>>\n<<shake>>\nEnd.\n===\n");
        std::vector<std::string> walks;
        int shakes = 0;
        d.on_command("walk", [&](std::string who, float speed, bool run) {
            walks.push_back(who + " " + std::to_string(static_cast<int>(speed * 10)) +
                            (run ? " run" : " walk"));
        });
        d.on_command("shake", [&] { ++shakes; });
        REQUIRE(d.start());
        CHECK(walks == std::vector<std::string>{ "Mika 25 run" });
        CHECK(shakes == 1);
        CHECK(d.line().text == "End.");
        CHECK(
            heard_about("<<walk>>'s word 2 is a number, and the script gives it 'fast'"));
        CHECK(heard_about(
            "<<walk>> takes 3 words after its name, and the script gives it 1"));
    }

    TEST_CASE("a command waits until the game says it is done") {
        rmp::Dialogue d = script("title: Start\n---\n<<walk door>>\nArrived.\n===\n");
        bool arrived = false;
        std::string where;
        d.on_command("walk", [&](std::string to) {
            where = to;
            d.wait_until([&] { return arrived; });
        });
        REQUIRE(d.start());
        CHECK(where == "door");
        CHECK_FALSE(d.has_line());
        CHECK_FALSE(d.has_command());
        CHECK(d.running());
        CHECK(d.waiting());
        CHECK(d.waiting());
        arrived = true;
        CHECK_FALSE(d.waiting());
        REQUIRE(d.has_line());
        CHECK(d.line().text == "Arrived.");
    }

    TEST_CASE("wait(seconds) and <<wait>> count each frame once") {
        // A step is 1/20 s (under [app] max_delta); asked twice a frame, a
        // second of waiting would be half as many frames.
        rmp::app::detail::set_fixed_delta(0.05f);
        rmp::Dialogue d =
            script("title: Start\n---\n<<pause>>\nOne.\n<<wait 0.5>>\nTwo.\n===\n");
        d.on_command("pause", [&] { d.wait(1.0f); });
        REQUIRE(d.start());
        int frames = 0;
        while (d.waiting() && frames < 100) {
            (void)rmp::app::detail::step_delta();
            CHECK(d.waiting() == d.waiting()); // a second ask is the same frame
            ++frames;
        }
        CHECK(frames >= 19);
        CHECK(frames <= 21);
        CHECK(d.line().text == "One.");
        d.advance();
        frames = 0;
        while (d.waiting() && frames < 100) {
            (void)rmp::app::detail::step_delta();
            ++frames;
        }
        CHECK(frames >= 9);
        CHECK(frames <= 11);
        CHECK(d.line().text == "Two.");
        rmp::app::detail::set_fixed_delta(0);
        rmp::app::detail::forget_steps_for_tests();
    }

    TEST_CASE("a game command named wait is the game's") {
        rmp::Dialogue d = script("title: Start\n---\n<<wait 9>>\nNow.\n===\n");
        int waits = 0;
        d.on_command("wait", [&](float) { ++waits; });
        REQUIRE(d.start());
        CHECK(waits == 1);
        CHECK_FALSE(d.waiting());
        CHECK(d.line().text == "Now.");
    }

    TEST_CASE("wait() outside a command waits for nothing, and says so") {
        Listening listening;
        rmp::Dialogue d = script("title: Start\n---\nHi.\n===\n");
        REQUIRE(d.start());
        d.wait(2);
        d.wait_until([] { return false; });
        CHECK_FALSE(d.waiting());
        CHECK(d.has_line());
        CHECK(heard_about("wait() is for a command's handler"));
        CHECK(heard_about("wait_until() is for a command's handler"));
    }

    TEST_CASE("advance() past a wait goes on at once") {
        rmp::Dialogue d = script("title: Start\n---\n<<wait 100>>\nSkipped to.\n===\n");
        REQUIRE(d.start());
        CHECK(d.waiting());
        d.advance();
        CHECK_FALSE(d.waiting());
        CHECK(d.line().text == "Skipped to.");
    }

    TEST_CASE("a bound variable is the game's own, read and written") {
        rmp::Dialogue d = script("title: Start\n---\n<<declare $gold = 0>>\n"
                                 "<<declare $name = \"\">>\n<<declare $met = false>>\n"
                                 "<<declare $speed = 1>>\n"
                                 "You have {$gold}, {$name}.\n<<set $gold = $gold - 5>>\n"
                                 "<<set $name = \"Ada\">>\n<<set $met = true>>\n"
                                 "<<set $speed = 2.5>>\nDone.\n===\n");
        int gold = 12;
        std::string name = "stranger";
        bool met = false;
        float speed = 0;
        d.bind("$gold", gold);
        d.bind("$name", name);
        d.bind("$met", met);
        d.bind("$speed", speed);
        REQUIRE(d.start());
        CHECK(d.line().text == "You have 12, stranger.");
        gold = 20; // the game changed it: the script sees it
        CHECK(d.variable("$gold").as_number() == 20);
        d.advance();
        CHECK(gold == 15);
        CHECK(name == "Ada");
        CHECK(met);
        CHECK(speed == doctest::Approx(2.5));
        d.set_variable("$gold", 99);
        CHECK(gold == 99);
        // Bound variables are the game's to save.
        rmp::Value saved;
        d.save(saved);
        CHECK_FALSE(saved.contains("$gold"));
        CHECK_FALSE(saved.contains("$name"));
    }

    TEST_CASE("a variable bound through two functions") {
        rmp::Dialogue d =
            script("title: Start\n---\n<<declare $hour = 0>>\n"
                   "It is {$hour}.\n<<set $hour = $hour + 1>>\nLater.\n===\n");
        float hour = 9;
        d.bind(
            "$hour", [&](rmp::DialogueValue &out) { out = hour; },
            [&](const rmp::DialogueValue &in) { hour = in.as_number(); });
        REQUIRE(d.start());
        CHECK(d.line().text == "It is 9.");
        d.advance();
        CHECK(hour == 10);
    }

    TEST_CASE("a binding the script does not declare, or of another type, is refused") {
        Listening listening;
        rmp::Dialogue d =
            script("title: Start\n---\n<<declare $gold = 0>>\n{$gold}\n===\n");
        bool wrong = true;
        int nothing = 0;
        d.bind("$gold", wrong);
        d.bind("$ghost", nothing);
        CHECK(heard_about(
            "bind(\"$gold\"): the script declares a number, and the game binds "
            "a bool"));
        CHECK(heard_about("bind(\"$ghost\"): the script does not declare it"));
        REQUIRE(d.start());
        CHECK(d.line().text == "0"); // the script's own variable, untouched
    }

    TEST_CASE("the raw forms still take every argument as written") {
        rmp::Dialogue d =
            script("title: Start\n---\n<<log a b c>>\n{count(1, 2, 3)}\n===\n");
        std::vector<std::string> logged;
        d.on_command("log", [&](std::span<const std::string> w) {
            logged.assign(w.begin(), w.end());
        });
        d.on_function("count",
                      [](rmp::DialogueValue &out, std::span<const rmp::DialogueValue> a) {
                          out = static_cast<int>(a.size());
                      });
        REQUIRE(d.start());
        CHECK(logged == std::vector<std::string>{ "a", "b", "c" });
        CHECK(d.line().text == "3");
    }
}
