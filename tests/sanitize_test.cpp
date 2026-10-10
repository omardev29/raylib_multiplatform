// ---------------------------------------------------------------------------
// The sanitizers this binary was built with: what CMake says it applied, held
// to what the compiler itself says, and each one seen catching what it is for.
//
// cmake/sanitize.cmake decides by running a probe, and defines RMP_SANITIZE,
// RMP_SANITIZE_ADDRESS and RMP_SANITIZE_UNDEFINED for what it applied. A flag
// that went missing between that decision and the compile would leave the
// defines saying one thing and the code another; the first test reads both.
// The others run tests/sanitizer_canary.cpp, built with the same flags and the
// same LeakSanitizer suppressions, and read what it printed.
//
// RMP_REQUIRE_SANITIZERS=1 -- which CI sets on the steps that are gates --
// makes a binary with no sanitizer a failure here rather than a skip.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

// What the compiler says, in the two spellings there are: gcc's macro, and
// clang's __has_feature (which gcc 14 and later answer too). gcc before 14
// has no way to say UBSan is on, and there the canary is the only proof.
#if defined(__SANITIZE_ADDRESS__)
#define RMP_COMPILER_ADDRESS 1
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#undef RMP_COMPILER_ADDRESS
#define RMP_COMPILER_ADDRESS 1
#endif
#if __has_feature(undefined_behavior_sanitizer)
#define RMP_COMPILER_UNDEFINED 1
#endif
#endif

namespace {

// Read only when this binary has no sanitizer at all.
[[maybe_unused]] bool required() {
    const char *must = std::getenv("RMP_REQUIRE_SANITIZERS");
    return must != nullptr && std::string(must) == "1";
}

struct Run {
    int status = -1;
    std::string out;
};

#if defined(RMP_SANITIZER_CANARY) && !defined(_WIN32)
// The canary in one mode, stdout and stderr together, and how it ended. Its
// stdin is /dev/null: a program started from a test reads nothing of ours.
Run canary(const std::string &mode) {
    Run run;
    const std::string command =
        std::string("'") + RMP_SANITIZER_CANARY + "' " + mode + " 2>&1 < /dev/null";
    // NOLINTNEXTLINE(bugprone-command-processor): our own binary, by its build path
    FILE *pipe = popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    char chunk[512];
    while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr) run.out += chunk;
    const int raw = pclose(pipe);
    run.status = WIFEXITED(raw) ? WEXITSTATUS(raw) : 128 + WTERMSIG(raw);
    return run;
}
#endif

} // namespace

TEST_SUITE("sanitizers") {
    TEST_CASE("what CMake applied is what the compiler compiled") {
#if defined(RMP_SANITIZE_ADDRESS)
        const bool cmake_address = true;
#else
        const bool cmake_address = false;
#endif
#if defined(RMP_COMPILER_ADDRESS)
        const bool compiler_address = true;
#else
        const bool compiler_address = false;
#endif
        CHECK(cmake_address == compiler_address);
#if defined(RMP_COMPILER_UNDEFINED)
#if defined(RMP_SANITIZE_UNDEFINED)
        const bool cmake_undefined = true;
#else
        const bool cmake_undefined = false;
#endif
        CHECK(cmake_undefined);
#endif
#if !defined(RMP_SANITIZE)
        if (required()) {
            FAIL_CHECK(
                "this unit_test has no sanitizer, and RMP_REQUIRE_SANITIZERS=1 says "
                "it must: the framework's .toml lists [dev] sanitize = [\"address\", "
                "\"undefined\"]");
        } else {
            MESSAGE("built without sanitizers ([dev] sanitize, or RMP_SANITIZE=OFF)");
        }
#endif
    }

#if defined(RMP_SANITIZER_CANARY) && !defined(_WIN32)
    TEST_CASE("the canary is quiet when nothing is wrong") {
        const Run run = canary("clean");
        CHECK(run.status == 0);
        CHECK(run.out.find("ERROR:") == std::string::npos);
        CHECK(run.out.find("runtime error") == std::string::npos);
    }

#if defined(RMP_SANITIZE_ADDRESS)
    TEST_CASE("AddressSanitizer stops a read past a heap block") {
        const Run run = canary("overflow");
        CHECK(run.status != 0);
        CHECK(run.out.find("ERROR: AddressSanitizer: heap-buffer-overflow") !=
              std::string::npos);
        CHECK(run.out.find("read 0") == std::string::npos); // it never got that far
    }

#if defined(__linux__)
    // LeakSanitizer is on by default with ASan on Linux and nowhere else this
    // runs. Through every suppression in cmake/sanitizer_hooks.c: the leak is
    // in a frame of ours, and has to be reported anyway.
    TEST_CASE("LeakSanitizer reports our leak, whatever the suppressions say") {
        const Run run = canary("leak");
        CHECK(run.status != 0);
        CHECK(run.out.find("ERROR: LeakSanitizer: detected memory leaks") !=
              std::string::npos);
        CHECK(run.out.find("sanitizer_canary") != std::string::npos);
        CHECK(run.out.find("Suppressions used") == std::string::npos);
    }
#endif
#endif

#if defined(RMP_SANITIZE_UNDEFINED)
    TEST_CASE("UndefinedBehaviorSanitizer stops a signed overflow") {
        const Run run = canary("ub");
        CHECK(run.status != 0);
        CHECK(run.out.find("runtime error: signed integer overflow") !=
              std::string::npos);
        CHECK(run.out.find("overflowed to") == std::string::npos);
    }
#endif
#endif
}
