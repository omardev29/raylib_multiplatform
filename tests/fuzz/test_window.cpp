// ---------------------------------------------------------------------------
// RAY_TEST_WINDOW's reader (rmp::scenes::detail::parse_test_window() and
// read_test_window(), src/rmp/scene.cpp), fuzzed. An environment variable is
// text from outside the program, and the CI steps that set it are written by
// hand.
//
// The oracle is scene_internal.h's sentence, whatever the text:
//
//   - parse_test_window() returns: no crash, no UB, no loop;
//   - it reads a size exactly when the text is two whole numbers of one to
//     five digits joined by a lower-case x, each 16..16384 -- checked against
//     a regular expression, a second reading of that sentence -- and the size
//     is those two numbers;
//   - read_test_window() has a size exactly when parse_test_window() does,
//     and refuses exactly a text that is set and is not one.
//
// Inputs: tests/fuzz/corpus/test_window/ and tests/fuzz/regressions/
// test_window/, the edges named for what they are.
//
//   bash tools/fuzz.sh replay test_window         every committed input, once
//   bash tools/fuzz.sh run test_window 60         a minute of libFuzzer
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../../src/rmp/scene_internal.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>

namespace {

using rmp::test::fuzz::require;

// The sentence, read by std::regex and std::stoi.
std::optional<rmp::scenes::detail::WindowSize> read_by_regex(const std::string &text) {
    static const std::regex SHAPE("([0-9]{1,5})x([0-9]{1,5})");
    std::smatch m;
    if (!std::regex_match(text, m, SHAPE)) return std::nullopt;
    const int w = std::stoi(m[1].str());
    const int h = std::stoi(m[2].str());
    if (w < 16 || w > 16384 || h < 16 || h > 16384) return std::nullopt;
    return rmp::scenes::detail::WindowSize{ w, h };
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
    const std::string text(input.begin(), input.end());

    const std::optional<rmp::scenes::detail::WindowSize> got =
        rmp::scenes::detail::parse_test_window(text);
    const std::optional<rmp::scenes::detail::WindowSize> want = read_by_regex(text);
    require(got.has_value() == want.has_value(),
            "a size is read exactly when the text is WIDTHxHEIGHT, each 16..16384");
    if (got && want) {
        require(got->width == want->width && got->height == want->height,
                "the size read is the two numbers of the text");
    }

    const rmp::scenes::detail::TestWindow asked =
        rmp::scenes::detail::read_test_window(text);
    require(asked.size.has_value() == got.has_value(),
            "read_test_window has a size exactly when the text is one");
    require(asked.refused == (!text.empty() && !got.has_value()),
            "read_test_window refuses exactly a text that is set and is not a size");
    return 0;
}
