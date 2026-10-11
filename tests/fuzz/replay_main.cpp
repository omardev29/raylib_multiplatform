// ---------------------------------------------------------------------------
// A fuzz target's main() where there is no libFuzzer: gcc, a clang without
// compiler-rt, or simply the default build (tests/deep.cmake, RMP_FUZZ_ENGINE
// replay). It runs LLVMFuzzerTestOneInput once on each file named on the
// command line, under whatever sanitizers the build has, and says so the way
// libFuzzer does -- "Executed <file>" for each and "ran=N" at the end -- so
// tools/fuzz.sh counts the same lines whichever main() it ran, and fails
// unless every file it named was run.
//
// Arguments that start with '-' are libFuzzer's flags (-timeout=5,
// -rss_limit_mb=1024): ignored here, so one command line replays with either.
// A file that cannot be read fails the run -- a replay that skipped an input
// is not a pass.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// libFuzzer's C ABI, which the harness defines.
// NOLINTBEGIN(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);
extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv);
// NOLINTEND(readability-identifier-naming)

// NOLINTNEXTLINE(custom-rmp-raw-pointer): main's signature is the language's
int main(int argc, char **argv) {
    LLVMFuzzerInitialize(&argc, &argv);
    int ran = 0;
    for (const std::string_view path :
         std::span(argv, static_cast<std::size_t>(argc)).subspan(1)) {
        if (path.starts_with("-")) continue;
        std::ifstream file{ std::string(path), std::ios::binary };
        if (!file) {
            std::fprintf(stderr, "replay: cannot read %.*s\n",
                         static_cast<int>(path.size()), path.data());
            return 1;
        }
        // Exactly the file's bytes in an allocation of exactly that size, so
        // ASan sees a read one past the end of the input as what it is.
        const std::vector<std::uint8_t> bytes{ std::istreambuf_iterator<char>(file),
                                               std::istreambuf_iterator<char>() };
        const std::vector<std::uint8_t> exact(bytes.begin(), bytes.end());
        LLVMFuzzerTestOneInput(exact.data(), exact.size());
        std::fprintf(stderr, "Executed %.*s\n", static_cast<int>(path.size()),
                     path.data());
        ++ran;
    }
    std::printf("ran=%d\n", ran);
    return 0;
}
