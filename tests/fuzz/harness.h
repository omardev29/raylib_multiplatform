#pragma once
// ---------------------------------------------------------------------------
// What every fuzz target of the framework does before its first input and at
// the start of each one, so that a replay depends on the input and nothing
// else -- not on the machine's resources/ folder, not on what the input before
// it reported:
//
//   extern "C" int LLVMFuzzerInitialize(int *, char ***) {
//       rmp::test::fuzz::initialize();
//       return 0;
//   }
//   extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
//       const std::span<const std::uint8_t> input = rmp::test::fuzz::input({ data, size });
//       ...the code under test, and the oracle: the harness returns, and every
//          promise the header makes about bad input holds...
//       return 0;
//   }
//
// The two functions are libFuzzer's C ABI, a boundary like raylib's: the
// pointer is turned into a std::span on the first line and never used again
// (the raw-pointer check exempts an extern "C" function's parameters). Their
// names are libFuzzer's too, hence the NOLINT on each.
//
// A target's inputs are tests/fuzz/corpus/<name>/ (what fuzzing found, merged)
// and tests/fuzz/regressions/<name>/ (every input that once failed, named for
// what it is); tools/fuzz.sh replays both on every push.
// ---------------------------------------------------------------------------

#include "../../src/rmp/internal.h"

#include <raylib.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace rmp::test::fuzz {

namespace detail {
struct Strict {
    int aborts_asked = 0;
};
inline Strict strict;
inline void count_abort() { ++strict.aborts_asked; }

// The empty resources folder of this process, gone when it exits.
struct Empty {
    std::filesystem::path folder;
};
inline Empty empty;
inline void remove_empty() {
    std::error_code error;
    std::filesystem::remove_all(empty.folder, error);
}
inline int process_id() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}
} // namespace detail

// Once, before the first input: no log on every input (a fuzzer runs
// thousands a second), resources from an empty folder of its own -- never the
// game's resources/, which would make a replay depend on the checkout -- and
// [dev] strict off: an abort is what libFuzzer reports as a crash, and a
// report_once() on a WRITE is allowed. Off, strict calls no handler at all;
// the counting one is there for a harness that turns strict on to watch a
// write report, which then reads detail::strict.aborts_asked instead of
// ending the run.
//
// The folder is this process's -- its id in the name -- and emptied here, not
// trusted to be empty. One name for every process was one folder for every
// fuzzer on the machine, and the campaign's agents replay side by side: each
// one's start would delete the folder another was loading from. It goes when
// the process exits; a crash leaves an empty folder behind, nothing more. A harness never writes into it -- one that needs a file on disk makes
// a folder of its own.
inline void initialize() {
    SetTraceLogLevel(LOG_NONE);
    std::error_code error;
    detail::empty.folder = std::filesystem::temp_directory_path(error) /
        ("rmp-fuzz-empty-resources-" + std::to_string(detail::process_id()));
    std::filesystem::remove_all(detail::empty.folder, error);
    std::filesystem::create_directories(detail::empty.folder, error);
    std::atexit(detail::remove_empty);
    const std::string root = (detail::empty.folder / "").string();
    rmp::assets::detail::set_resources_root(root.c_str());
    rmp::detail::set_strict(false);
    rmp::detail::set_strict_handler(detail::count_abort);
}

// At the start of each input: the once-only reports forgotten, so whether a
// report fires depends on this input alone. The bytes go through untouched.
inline std::span<const std::uint8_t> input(std::span<const std::uint8_t> bytes) {
    rmp::detail::reset_reports_for_tests();
    return bytes;
}

// The oracle: a promise that did not hold is a crash, which is what libFuzzer
// and tools/fuzz.sh replay report -- with this line first, so the report says
// which promise, and the nightly run's issue is named after it.
inline void require(bool holds, std::string_view promise) {
    if (holds) return;
    std::fprintf(stderr, "FUZZ ORACLE FAILED: %.*s\n", static_cast<int>(promise.size()),
                 promise.data());
    std::abort();
}

} // namespace rmp::test::fuzz
