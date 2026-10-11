// tests/console_canary.cpp -- a DRM/KMS game's ways of ending, on a terminal
// of the test's choosing, so that tests/console_test.cpp can see whether the
// terminal comes back after each.
//
//     console_canary PTY guarded|unguarded HOW
//
// It opens PTY (the slave of a pseudo-terminal the test made, never its own
// controlling terminal), guards it the way guard_console() guards stdin --
// unless "unguarded", the control -- does to it what raylib's DRM backend
// does (InitKeyboard in rcore_drm.c: ICANON, ECHO and ISIG off, O_NONBLOCK
// on), and ends by HOW:
//
//     exit       returns from main(): the atexit handlers run
//     segv       a write to address 16, where nothing is mapped
//     abort      abort()
//     term       raise(SIGTERM), what `kill` sends
//     trap       __builtin_trap()
//     fpe        an integer divided by zero: UBSan's report where it is on
//     overflow   a read past a heap block: ASan's report
//     kill       raise(SIGKILL), which nothing inside a process can catch
//
// Every value that decides a crash comes from the command line, so that the
// compiler cannot see it coming and refuse to build it.

#include "../src/rmp/internal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#if defined(__linux__)
#include <csignal>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

namespace {

void break_like_raylib(int fd) {
    termios settings{};
    tcgetattr(fd, &settings);
    settings.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
    tcsetattr(fd, TCSANOW, &settings);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

// Raw memory on purpose: the crash is the whole job.
// NOLINTBEGIN(cppcoreguidelines-owning-memory, custom-rmp-raw-pointer)
[[gnu::noinline]] int read_past(int size) {
    const int *block = new int[static_cast<unsigned>(size)]{};
    // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): the overflow
    const int past = block[size];
    delete[] block;
    return past;
}
// NOLINTEND(cppcoreguidelines-owning-memory, custom-rmp-raw-pointer)

// Through an address nothing is mapped at, and not null: UBSan stops a null
// store itself, and this is the kernel's SIGSEGV, which ASan's handler and
// ours both stand on.
[[gnu::noinline]] void write_to(std::uintptr_t address) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr, custom-rmp-raw-pointer): the crash
    int *volatile target = reinterpret_cast<int *>(address);
    *target = 1;
}

[[gnu::noinline]] int divide(int by) { return 1000 / by; }

} // namespace

// NOLINTNEXTLINE(custom-rmp-raw-pointer): main's own signature
int main(int argc, char **argv) {
    if (argc != 4) {
        std::fputs("usage: console_canary PTY guarded|unguarded HOW\n", stderr);
        return 2;
    }
    const std::string_view how = argv[3];
    const int fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (fd < 0) {
        std::perror(argv[1]);
        return 2;
    }
    if (std::string_view(argv[2]) == "guarded" && !rmp::app::detail::guard_terminal(fd)) {
        std::fputs("not a terminal\n", stderr);
        return 2;
    }
    break_like_raylib(fd);
    std::puts("broken");
    std::fflush(stdout);
    // 4 for "segv", 3 for "fpe": never 0, never known when compiling.
    const int from_args = static_cast<int>(how.size());
    if (how == "exit") return 0;
    if (how == "segv") write_to(static_cast<std::uintptr_t>(from_args) * 4);
    if (how == "abort") std::abort();
    if (how == "term") std::raise(SIGTERM);
    if (how == "trap") __builtin_trap();
    if (how == "fpe") return divide(from_args - 3);
    if (how == "overflow") return read_past(from_args);
    if (how == "kill") std::raise(SIGKILL);
    std::fputs("survived\n", stderr);
    return 3;
}

#else

int main() { return 0; }

#endif
