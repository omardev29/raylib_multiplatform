// ---------------------------------------------------------------------------
// The Yarn compiler and machine, fuzzed: src/rmp/dialogue/.
//
// The input is a .yarn file's bytes. The oracle is the compiler's promise --
// it compiles a script or says what is wrong with it, at a line and a column
// inside the file, and never aborts, reads out of bounds, leaks or loops --
// and, of every program it makes:
//
//   - its bytes read back (yarn_bytes.cpp) as the same program;
//   - run from every node, choosing each option in turn, it stops within a
//     budget of steps or keeps asking the game -- it never crashes and never
//     shows a line the program does not have.
//
// Inputs: tests/fuzz/corpus/yarn/ (seeded with Yarn Spinner's own test cases)
// and tests/fuzz/regressions/yarn/.
//
//   bash tools/fuzz.sh replay yarn      every committed input, once
//   bash tools/fuzz.sh run yarn 60      a minute of libFuzzer
// ---------------------------------------------------------------------------

#include "harness.h"

#include "../../src/rmp/dialogue/yarn.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using rmp::test::fuzz::require;

constexpr int STEPS = 400; // events per run, before it is called a loop the game lets run

} // namespace

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerInitialize(int * /*argc*/, char *** /*argv*/) {
    rmp::test::fuzz::initialize();
    return 0;
}

// NOLINTNEXTLINE(readability-identifier-naming): libFuzzer's name
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> input = rmp::test::fuzz::input({ data, size });
    const std::vector<rmp::yarn::Source> sources{
        { "fuzz.yarn", std::string(input.begin(), input.end()) }
    };
    const rmp::yarn::Compiled c = rmp::yarn::compile(sources);
    int lines = 1;
    for (const char ch : sources[0].text) lines += ch == '\n' ? 1 : 0;
    for (const rmp::yarn::Diagnostic &d : c.errors) {
        require(d.at.line >= 0 && d.at.line <= lines,
                "an error is at a line of the file");
        require(!d.message.empty(), "an error says what is wrong");
    }
    if (!c.ok()) return 0;
    const std::vector<std::uint8_t> bytes = rmp::yarn::to_bytes(c.program);
    const auto back = rmp::yarn::from_bytes(bytes);
    require(back.has_value(), "a program's bytes read back");
    require(back && rmp::yarn::to_bytes(*back) == bytes, "and they are the same program");
    for (const rmp::yarn::ProgramNode &n : c.program.nodes) {
        for (int pick = 0; pick < 3; ++pick) {
            rmp::yarn::Machine m(c.program);
            m.start(n.name);
            for (int step = 0; step < STEPS && m.running(); ++step) {
                const rmp::yarn::Event &e = m.event();
                if (e.is == rmp::yarn::Event::Is::LINE) {
                    require(c.program.line_index(e.id) >= 0,
                            "every line shown is in the program");
                    m.advance();
                } else if (e.is == rmp::yarn::Event::Is::OPTIONS) {
                    const int count = static_cast<int>(e.options.size());
                    m.choose(count == 0 ? -1 : (pick + step) % count);
                    if (m.event().is == rmp::yarn::Event::Is::OPTIONS) m.choose(-1);
                    if (m.event().is == rmp::yarn::Event::Is::OPTIONS) {
                        // Every option it offers may be unavailable but one: take the first that is.
                        for (int k = 0; k < count; ++k) {
                            if (e.options[static_cast<std::size_t>(k)].available) {
                                m.choose(k);
                                break;
                            }
                        }
                    }
                } else {
                    m.advance();
                }
            }
        }
    }
    return 0;
}
