// ---------------------------------------------------------------------------
// The e2e scripts' grammar (tests/e2e/script.cpp), fuzzed -- and the shape of
// a fuzz target, for the next one to copy.
//
// The oracle is everything script.h promises about a text, whatever the text:
//
//   - parse() returns: no crash, no UB, no unbounded loop (the sanitizers and
//     libFuzzer's -timeout say so);
//   - when it refuses, the line it names is a line of the text, it gives a
//     reason, and the Script it was handed is left exactly as it was;
//   - when it accepts, the script ends with `end`, its frames never go back
//     and never pass MAX_FRAME, and each line gave at most one step;
//   - what it accepted, written back as text, reads back as the same script;
//   - readable_type() and names_class() take any text at all.
//
// Inputs: tests/fuzz/corpus/e2e_script/ (the scripts tests/e2e/ plays, and
// what fuzzing merged in) and tests/fuzz/regressions/e2e_script/.
// tests/fuzz/e2e_script.dict hands libFuzzer the grammar's words.
//
//   bash tools/fuzz.sh replay e2e_script         every committed input, once
//   bash tools/fuzz.sh run e2e_script 60         a minute of libFuzzer
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../e2e/script.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace {

using rmp::test::fuzz::require;

// A step as the grammar writes it, from what parse() made of it.
std::string written(const e2e::Step &step) {
    const std::string out = std::to_string(step.frame) + " ";
    const std::string key =
        step.pad ? e2e::pad_button_name(step.key) : e2e::key_name(step.key);
    switch (step.verb) {
        case e2e::Verb::PRESS:
            return out + "press " + key;
        case e2e::Verb::RELEASE:
            return out + "release " + key;
        case e2e::Verb::TAP:
            return out + "tap " + key;
        case e2e::Verb::CLICK:
            return out + "click " + std::to_string(step.x) + " " + std::to_string(step.y);
        case e2e::Verb::SHOT:
            return out + "shot " + step.name;
        case e2e::Verb::EXPECT_SCENE:
            return out + "expect scene " + step.name;
        case e2e::Verb::END:
            return out + "end";
    }
    return out + "?";
}

std::string written(const e2e::Script &script) {
    std::string out;
    for (const e2e::Step &step : script.steps) out += written(step) + "\n";
    return out;
}

// The same steps, the line each came from aside.
bool same(const e2e::Script &a, const e2e::Script &b) {
    return std::ranges::equal(
        a.steps, b.steps, [](const e2e::Step &x, const e2e::Step &y) {
            return x.frame == y.frame && x.verb == y.verb && x.key == y.key &&
                x.pad == y.pad && x.x == y.x && x.y == y.y && x.name == y.name;
        });
}

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerInitialize(int * /*argc*/, char *** /*argv*/) {
    rmp::test::fuzz::initialize();
    return 0;
}

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> input = rmp::test::fuzz::input({ data, size });
    const std::string bytes(input.begin(), input.end());
    const std::string_view text(bytes);

    e2e::Script script;
    script.steps.push_back(e2e::Step{ .line = -1, .name = "left as it was" });
    e2e::ParseError error;
    if (!e2e::parse(text, script, error)) {
        const int lines = static_cast<int>(std::ranges::count(text, '\n')) + 1;
        require(error.line >= 1 && error.line <= lines,
                "a refusal names a line of the text");
        require(!error.reason.empty(), "a refusal says why");
        require(script.steps.size() == 1 && script.steps[0].line == -1 &&
                    script.steps[0].name == "left as it was",
                "a refusal leaves the script it was handed as it was");
    } else {
        require(!script.steps.empty() && script.steps.back().verb == e2e::Verb::END,
                "an accepted script ends with `end`");
        for (std::size_t i = 0; i < script.steps.size(); ++i) {
            const e2e::Step &step = script.steps[i];
            require(step.frame >= 0 && step.frame <= e2e::MAX_FRAME,
                    "a frame is in range");
            if (i > 0) {
                require(step.frame >= script.steps[i - 1].frame, "frames never go back");
                require(step.line > script.steps[i - 1].line,
                        "one step a line, in order");
            }
        }
        e2e::Script again;
        e2e::ParseError again_error;
        require(e2e::parse(written(script), again, again_error),
                "an accepted script, written back, is accepted");
        require(same(script, again), "an accepted script, written back, reads the same");
    }

    const std::string readable = e2e::readable_type(text);
    static_cast<void>(e2e::names_class(readable, text));
    static_cast<void>(e2e::names_class(text, readable));
    return 0;
}
