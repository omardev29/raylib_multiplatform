// ---------------------------------------------------------------------------
// Properties of a dialogue and the game's C++ (src/rmp/dialogue/dialogue.cpp,
// src/rmp/ui/dialogue_ui.cpp): a typed function or command called with any
// arguments is called with its own count and kinds or not at all, and never
// crashes; a bound variable is the game's whatever the script writes; a wait
// of any length -- NaN, infinity, negative -- ends or holds, never crashes;
// the stage shakes and flashes by any amount; and a presenter of the game's
// own, out of dialogue_line() and dialogue_choices(), runs any script.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/dialogue/dialogue_internal.h"
#include "../../src/rmp/internal.h"
#include "../../src/rmp/text_internal.h"
#include "../../src/rmp/ui/internal.h"

#include <rmp/app.h>
#include <rmp/dialogue.h>
#include <rmp/save.h>
#include <rmp/ui.h>

#include <doctest.h>
#include <rapidcheck.h>

#include <cmath>
#include <string>
#include <vector>

// covers: rmp::Dialogue::on_command [template]
// covers: rmp::Dialogue::on_function [template]
// covers: rmp::Dialogue::bind(std::string_view, int &)
// covers: rmp::Dialogue::bind(std::string_view, float &)
// covers: rmp::Dialogue::bind(std::string_view, bool &)
// covers: rmp::Dialogue::bind(std::string_view, std::string &)
// covers: rmp::Dialogue::bind(std::string_view, rmp::Callback<DialogueValue &>, rmp::Callback<const DialogueValue &>)
// covers: rmp::Dialogue::wait(float)
// covers: rmp::Dialogue::wait_until [template]
// covers: rmp::Dialogue::waiting()
// covers: rmp::ui::DialogueStage::flash(Color, float)
// covers: rmp::ui::DialogueStage::shake(float, float)
// covers: rmp::ui::dialogue_line(rmp::Dialogue &, DialogueMemory &, const DialogueFlow &, const RichTextOptions &)
// covers: rmp::ui::dialogue_choices(rmp::Dialogue &, DialogueMemory &, const DialogueFlow &, const Style &)

namespace {

namespace dd = rmp::dialogue::detail;

void quiet() {
    SetTraceLogLevel(LOG_NONE);
    rmp::lang::detail::reset_for_tests();
    rmp::lang::detail::set_persist(false);
    rmp::lang::detail::set_system_locale_for_tests("en");
    rmp::lang::detail::load_tables({});
    rmp::detail::reset_reports_for_tests();
}

// An argument of Yarn, of any of its three kinds.
rc::Gen<std::string> argument() {
    return rc::gen::element<std::string>("1", "-2.5", "\"x\"", "true", "false", "\"\"",
                                         "$n", "$s", "1e30", "\"a b\"");
}

// A command's word: a number, a bool, a word, or nonsense.
rc::Gen<std::string> word() {
    return rc::gen::element<std::string>("1", "2.5", "-0", "true", "False", "nan", "inf",
                                         "x", "1e40", "--", "0x10");
}

std::string joined(const std::vector<std::string> &parts, std::string_view by) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += by;
        out += parts[i];
    }
    return out;
}

} // namespace

TEST_SUITE("property: dialogue and the game's C++") {
    TEST_CASE("a typed call is made with its own count and kinds, or not at all") {
        rmp::test::check("any arguments to a typed function and command", [] {
            quiet();
            const auto args = *rc::gen::resize(
                4, rc::gen::container<std::vector<std::string>>(argument()));
            const auto words =
                *rc::gen::resize(4, rc::gen::container<std::vector<std::string>>(word()));
            const std::string source =
                "title: Start\n---\n<<declare $n = 3>>\n<<declare $s = \"q\">>\n{f(" +
                joined(args, ", ") + ")}\n<<g " + joined(words, " ") + ">>\nEnd.\n===\n";
            rmp::Dialogue d = dd::compile_text(source);
            if (!d.loaded()) return; // the type checker refused it: also an answer
            int calls = 0;
            int commands = 0;
            d.on_function("f", [&](float a, std::string b) -> std::string {
                ++calls;
                RC_ASSERT(std::isfinite(a) || std::isinf(a) || std::isnan(a));
                return b + "!";
            });
            d.on_command("g", [&](float a, bool b) {
                ++commands;
                (void)a;
                (void)b;
            });
            d.start();
            for (int i = 0; i < 8 && d.running(); ++i) d.advance();
            const bool fits = args.size() == 2;
            RC_ASSERT(calls <= 1);
            if (!fits) RC_ASSERT(calls == 0);
            if (words.size() != 2) RC_ASSERT(commands == 0);
        });
    }

    TEST_CASE("a bound variable is the game's, whatever the script writes") {
        rmp::test::check("any writes to bound variables", [] {
            quiet();
            const float n = *rmp::test::gen::hostile_float();
            const std::string sv = *rmp::test::gen::hostile_string();
            int i = *rc::gen::arbitrary<int>();
            float f = 0;
            bool b = false;
            std::string s;
            float hour = 0;
            rmp::Dialogue d = dd::compile_text(
                "title: Start\n---\n<<declare $i = 0>>\n<<declare $f = 0>>\n"
                "<<declare $b = false>>\n<<declare $s = \"\">>\n<<declare $h = 0>>\n"
                "<<set $i = $i + 1>>\n<<set $f = $f * 2>>\n<<set $b = !$b>>\n"
                "<<set $s = $s + \"x\">>\n<<set $h = $h + 1>>\n{$i} {$f} {$b} "
                "{$s}\n===\n");
            RC_ASSERT(d.loaded());
            d.bind("$i", i);
            d.bind("$f", f);
            d.bind("$b", b);
            d.bind("$s", s);
            d.bind(
                "$h", [&](rmp::DialogueValue &out) { out = hour; },
                [&](const rmp::DialogueValue &in) { hour = in.as_number(); });
            f = n;
            s = sv;
            d.start();
            RC_ASSERT(b);
            RC_ASSERT(s == sv + "x");
            RC_ASSERT(hour == 1.0f);
            rmp::Value saved;
            d.save(saved);
            RC_ASSERT(!saved.contains("$i"));
            RC_ASSERT(!saved.contains("$s"));
        });
    }

    TEST_CASE("a wait of any length ends or holds, never crashes") {
        rmp::app::detail::set_fixed_delta(0.05f);
        rmp::test::check("any seconds, any done", [] {
            quiet();
            const float seconds = *rmp::test::gen::hostile_float();
            const int ready_at = *rc::gen::inRange(0, 30);
            rmp::Dialogue d = dd::compile_text(
                "title: Start\n---\n<<hold>>\n<<until>>\n<<wait 0.1>>\nEnd.\n===\n");
            RC_ASSERT(d.loaded());
            int frame = 0;
            d.on_command("hold", [&] { d.wait(seconds); });
            d.on_command("until",
                         [&] { d.wait_until([&] { return frame >= ready_at; }); });
            d.wait(seconds); // outside a command: said, and nothing
            d.start();
            // No runner steps here: with a fixed delta each ask is a frame of
            // its own (stepping the clock would leave its mark for the next suite).
            for (; frame < 400 && d.waiting(); ++frame) {
            }
            // A wait not a number of seconds waits none; one of up to ten
            // seconds is over within the 400 frames of 1/20 s.
            const bool long_wait = std::isfinite(seconds) && seconds > 10;
            if (!long_wait) RC_ASSERT(d.has_line());
            d.advance();
        });
        rmp::app::detail::set_fixed_delta(0);
    }

    TEST_CASE(
        "the stage shakes and flashes by any amount; a presenter of one's own runs") {
        rmp::ui::detail::set_test_viewport(800, 450);
        rmp::ui::detail::set_test_frame_time(1.0f / 30);
        rmp::app::detail::set_fixed_delta(1.0f / 30); // for the <<wait>>
        rmp::test::check("any shake, any flash, any script", [] {
            quiet();
            rmp::Dialogue d =
                dd::compile_text("title: Start\n---\nMika: One [wave]two[/wave].\nSora: "
                                 "Three.\n-> A\n    a\n"
                                 "-> B <<if false>>\n    b\n<<wait 0.05>>\nEnd.\n===\n");
            RC_ASSERT(d.loaded());
            d.start();
            rmp::ui::DialogueStage stage{
                .flow = { .auto_advance = 0.01f, .choice_seconds = 0.05f },
                .enter_seconds = *rmp::test::gen::hostile_float(),
                .focus_seconds = *rmp::test::gen::hostile_float()
            };
            stage.shake(*rmp::test::gen::hostile_float(),
                        *rmp::test::gen::hostile_float());
            stage.flash(Color{ 1, 2, 3, 200 }, *rmp::test::gen::hostile_float());
            rmp::Dialogue own =
                dd::compile_text("title: Start\n---\nHi.\n-> A\n    a\n===\n");
            own.start();
            rmp::ui::DialogueMemory memory;
            const rmp::ui::DialogueFlow flow{ .letters_per_second =
                                                  *rmp::test::gen::hostile_float(),
                                              .auto_advance = 0.02f,
                                              .choice_seconds = 0.05f };
            for (int f = 0; f < 90 && (d.running() || own.running()); ++f) {
                rmp::ui::begin();
                stage.show(d);
                // A line that goes on inside dialogue_line() may leave the
                // choices for dialogue_choices() in the same frame, and a
                // choice taken there may end it: only one with something to
                // show draws it.
                (void)rmp::ui::dialogue_line(own, memory, flow);
                (void)rmp::ui::dialogue_choices(own, memory, flow);
                rmp::ui::end();
            }
        });
        rmp::ui::detail::set_test_frame_time(0);
        rmp::app::detail::set_fixed_delta(0);
        rmp::detail::reset_reports_for_tests();
    }
}
