// tests/sanitizer_canary.cpp -- a program that does, on request, each thing
// the sanitizers are there to catch, so that tests/sanitize_test.cpp can see
// them caught.
//
//     sanitizer_canary clean      exits 0 and reports nothing
//     sanitizer_canary leak       memory nobody frees      LeakSanitizer
//     sanitizer_canary overflow   a read past a heap block AddressSanitizer
//     sanitizer_canary ub         a signed int overflow    UndefinedBehaviorSanitizer
//
// Built by CMakeLists.txt with the flags of every other Debug target, and the
// sanitizer hooks linked in -- the LeakSanitizer suppressions among them. The
// leak happens in a function of OURS, by name, so a suppression broad enough
// to swallow our own code would make it go quiet, and the test red.
//
// Every value that decides an index or a sum comes from the command line, so
// that the compiler cannot see the mistake coming and refuse to build it.

#include <climits>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

// Leaking and overrunning raw memory is the whole job of this file. RAII would
// make both impossible, and the static analyzer finding both is the analyzer
// agreeing with the canary.
// NOLINTBEGIN(cppcoreguidelines-owning-memory)

[[gnu::noinline]] int leak_on_purpose(int size) {
    int *lost = new int[static_cast<unsigned>(size)]{};
    lost[0] = size;
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks): the leak
    return lost[0]; // and the only pointer to it goes out of scope here
}

[[gnu::noinline]] int overflow_on_purpose(int size) {
    int *block = new int[static_cast<unsigned>(size)]{};
    // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): the overflow
    const int past = block[size]; // one past the end
    delete[] block;
    return past;
}

// NOLINTEND(cppcoreguidelines-owning-memory)

[[gnu::noinline]] int overflow_an_int(int by) {
    int big = INT_MAX;
    big += by; // undefined: a signed int does not wrap
    return big;
}

} // namespace

int main(int argc, char **argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    // 4 for "leak", 8 for "overflow", 2 for "ub": never 0, never known early.
    const int from_args = static_cast<int>(mode.size());
    if (mode == "clean") {
        std::puts("clean");
        return 0;
    }
    if (mode == "leak") {
        std::printf("leaked %d ints\n", leak_on_purpose(from_args));
        return 0;
    }
    if (mode == "overflow") {
        std::printf("read %d\n", overflow_on_purpose(from_args));
        return 0;
    }
    if (mode == "ub") {
        std::printf("overflowed to %d\n", overflow_an_int(from_args));
        return 0;
    }
    std::fprintf(stderr, "usage: sanitizer_canary clean|leak|overflow|ub\n");
    return 2;
}
