// ---------------------------------------------------------------------------
// The Linux console back, however a DRM/KMS game ends (src/rmp/console.cpp).
//
// Everything here happens on a pseudo-terminal the test makes, never on the
// terminal the tests run in, and never on a virtual console: raylib's DRM
// backend changes three things on the terminal it starts on, and the test
// changes the same three -- termios (ICANON, ECHO, ISIG) and O_NONBLOCK; the
// keyboard mode only a virtual console has -- and looks at what comes back.
//
// The ways a game ends are tests/console_canary.cpp's: each one a process of
// its own, guarded or not, which breaks the terminal like raylib and then
// exits, crashes, aborts, is killed or is stopped by a sanitizer. The
// unguarded canary is the control: a crash leaves the terminal broken, which
// is why the guard exists. SIGKILL is the one the guard cannot reach, and the
// test says so: `rmp run` puts the terminal back from outside for that.
// ---------------------------------------------------------------------------

#include "doctest.h"

#include "../src/rmp/internal.h"

#if defined(__linux__)
#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

// A pseudo-terminal: the master kept open for as long as the test runs (a
// slave whose master has closed hangs up), and the slave's name and an fd of
// the test's own on it, to read what a canary left behind.
struct Pty {
    int main_fd = -1;
    int sub_fd = -1;
    std::string name;

    Pty() {
        main_fd = posix_openpt(O_RDWR | O_NOCTTY);
        REQUIRE(main_fd >= 0);
        REQUIRE(grantpt(main_fd) == 0);
        REQUIRE(unlockpt(main_fd) == 0);
        name = ptsname(main_fd);
        sub_fd = open(name.c_str(), O_RDWR | O_NOCTTY);
        REQUIRE(sub_fd >= 0);
    }
    Pty(const Pty &) = delete;
    Pty &operator=(const Pty &) = delete;
    Pty(Pty &&) = delete;
    Pty &operator=(Pty &&) = delete;
    ~Pty() {
        close(sub_fd);
        close(main_fd);
    }
};

// The three things raylib changes, as one value to compare.
struct Seen {
    tcflag_t lflag = 0;
    int flags = 0;

    bool operator==(const Seen &) const = default;
};

Seen seen(int fd) {
    termios settings{};
    REQUIRE(tcgetattr(fd, &settings) == 0);
    return { .lflag = settings.c_lflag & (ICANON | ECHO | ISIG),
             .flags = fcntl(fd, F_GETFL) };
}

void break_like_raylib(int fd) {
    termios settings{};
    tcgetattr(fd, &settings);
    settings.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
    tcsetattr(fd, TCSANOW, &settings);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

// Ours is the one with SA_SIGINFO among SIGTERM's and SIGABRT's: doctest's
// own handlers do without it, and the sanitizers leave both signals alone.
bool handler_is_ours(int signal) {
    struct sigaction now{};
    sigaction(signal, nullptr, &now);
    return (now.sa_flags & SA_SIGINFO) != 0;
}

// Whatever a failed REQUIRE leaves, the guard does not outlive the case: its
// fd would be a closed one, or somebody else's.
struct Unguard {
    Unguard() = default;
    Unguard(const Unguard &) = delete;
    Unguard &operator=(const Unguard &) = delete;
    Unguard(Unguard &&) = delete;
    Unguard &operator=(Unguard &&) = delete;
    ~Unguard() { rmp::app::detail::unguard_terminal(); }
};

#if defined(RMP_CONSOLE_CANARY)
struct Ending {
    bool exited = false;
    int code = -1; // the exit status, when it exited
    int signal = 0; // the signal, when one ended it
    std::string out; // stdout and stderr together
};

// The canary in one mode on `pty`, how it ended and what it said. Its stdin is
// /dev/null: a program started from a test reads nothing of ours. Through
// sh, which execs it or reports a signal as 128 + its number: both read here
// as the signal.
Ending canary(const Pty &pty, std::string_view guarded, std::string_view how) {
    const std::string command = std::string("'") + RMP_CONSOLE_CANARY + "' '" + pty.name +
        "' " + std::string(guarded) + " " + std::string(how) + " 2>&1 < /dev/null";
    // NOLINTNEXTLINE(bugprone-command-processor, custom-rmp-raw-pointer): popen's own
    FILE *pipe = popen(command.c_str(), "r");
    REQUIRE(pipe != nullptr);
    Ending ending;
    std::array<char, 512> chunk{};
    while (std::fgets(chunk.data(), static_cast<int>(chunk.size()), pipe) != nullptr) {
        ending.out += chunk.data();
    }
    const int raw = pclose(pipe);
    if (WIFSIGNALED(raw)) {
        ending.signal = WTERMSIG(raw);
    } else if (WIFEXITED(raw) && WEXITSTATUS(raw) > 128) {
        ending.signal = WEXITSTATUS(raw) - 128;
    } else if (WIFEXITED(raw)) {
        ending.exited = true;
        ending.code = WEXITSTATUS(raw);
    }
    return ending;
}
#endif

} // namespace

TEST_SUITE("console") {
    TEST_CASE("what a game changed on its terminal comes back") {
        const Pty pty;
        const Unguard unguard;
        const Seen before = seen(pty.sub_fd);
        CHECK((before.lflag & ECHO) != 0);
        REQUIRE(rmp::app::detail::guard_terminal(pty.sub_fd));
        break_like_raylib(pty.sub_fd);
        CHECK_FALSE(seen(pty.sub_fd) == before);
        rmp::app::detail::restore_terminal();
        CHECK(seen(pty.sub_fd) == before);
        // Again, and again: putting it back is idempotent, the atexit handler
        // and the death callback may both run after a signal handler did.
        break_like_raylib(pty.sub_fd);
        rmp::app::detail::restore_terminal();
        rmp::app::detail::restore_terminal();
        CHECK(seen(pty.sub_fd) == before);
        rmp::app::detail::unguard_terminal();
        // Unguarded, there is nothing to put back.
        break_like_raylib(pty.sub_fd);
        rmp::app::detail::restore_terminal();
        CHECK_FALSE(seen(pty.sub_fd) == before);
    }

    TEST_CASE("what is not a terminal is not guarded, and no handler is touched") {
        std::array<int, 2> ends{};
        REQUIRE(pipe(ends.data()) == 0);
        CHECK_FALSE(rmp::app::detail::guard_terminal(ends[0]));
        CHECK_FALSE(rmp::app::detail::guard_terminal(-1));
        CHECK_FALSE(handler_is_ours(SIGTERM));
        CHECK_FALSE(handler_is_ours(SIGABRT));
        close(ends[0]);
        close(ends[1]);
    }

    TEST_CASE("the guard stands in front of the handlers, and gives them back") {
        struct sigaction before{};
        sigaction(SIGTERM, nullptr, &before);
        const auto was = before.sa_handler;
        // An ignored signal stays ignored: nohup's SIGHUP is the user's choice.
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        struct sigaction hangup{};
        sigaction(SIGHUP, &ignore, &hangup);

        const Pty pty;
        const Unguard unguard;
        REQUIRE(rmp::app::detail::guard_terminal(pty.sub_fd));
        CHECK(handler_is_ours(SIGTERM));
        CHECK(handler_is_ours(SIGABRT));
        struct sigaction now{};
        sigaction(SIGHUP, nullptr, &now);
        CHECK(((now.sa_flags & SA_SIGINFO) == 0 && now.sa_handler == SIG_IGN));

        rmp::app::detail::unguard_terminal();
        sigaction(SIGTERM, nullptr, &now);
        CHECK(now.sa_handler == was);
        CHECK_FALSE(handler_is_ours(SIGTERM));
        sigaction(SIGHUP, &hangup, nullptr);
    }

#if defined(RMP_CONSOLE_CANARY)
    TEST_CASE(
        "a game that ends without its atexit handlers leaves it broken: the control") {
        for (const std::string_view how :
             std::array<std::string_view, 3>{ "segv", "abort", "term" }) {
            CAPTURE(how);
            const Pty pty;
            const Seen before = seen(pty.sub_fd);
            const Ending ending = canary(pty, "unguarded", how);
            REQUIRE(ending.out.find("broken") != std::string::npos);
            CHECK_FALSE((ending.exited && ending.code == 0));
            CHECK_FALSE(seen(pty.sub_fd) == before);
        }
    }

    TEST_CASE(
        "guarded, it comes back however the game ends, and the ending is the same") {
        struct Case {
            std::string_view how;
            int signal; // the signal it still dies of; 0: it exits
        };
        const std::array cases{
            Case{ .how = "exit", .signal = 0 },
            Case{ .how = "segv", .signal = SIGSEGV },
            Case{ .how = "abort", .signal = SIGABRT },
            Case{ .how = "term", .signal = SIGTERM },
            Case{ .how = "trap", .signal = SIGILL },
            Case{ .how = "fpe", .signal = SIGFPE },
        };
        for (const Case &c : cases) {
            CAPTURE(c.how);
            const Pty pty;
            const Seen before = seen(pty.sub_fd);
            const Ending ending = canary(pty, "guarded", c.how);
            INFO(ending.out);
            REQUIRE(ending.out.find("broken") != std::string::npos);
            CHECK(ending.out.find("survived") == std::string::npos);
            CHECK(seen(pty.sub_fd) == before);
            // A segfault and a division by zero are a sanitizer's to report,
            // where one is on: it still has the signal, or stops the game first.
            const bool reported = ending.out.find("Sanitizer") != std::string::npos ||
                ending.out.find("runtime error:") != std::string::npos;
            if (c.signal == 0) {
                CHECK((ending.exited && ending.code == 0));
            } else if (reported) {
                CHECK((ending.exited && ending.code != 0));
            } else if (c.how == "trap") {
                // __builtin_trap is ud2 on x86 (SIGILL) and brk on arm64 (SIGTRAP).
                CHECK((ending.signal == SIGILL || ending.signal == SIGTRAP));
            } else {
                CHECK(ending.signal == c.signal);
            }
        }
    }

#if defined(RMP_SANITIZE_ADDRESS)
    TEST_CASE("guarded, it comes back after AddressSanitizer stops the game") {
        const Pty pty;
        const Seen before = seen(pty.sub_fd);
        const Ending ending = canary(pty, "guarded", "overflow");
        INFO(ending.out);
        CHECK(ending.out.find("heap-buffer-overflow") != std::string::npos);
        CHECK((ending.exited && ending.code != 0));
        CHECK(seen(pty.sub_fd) == before);
        // And the control: without the death callback, it stays broken.
        const Pty other;
        const Seen untouched = seen(other.sub_fd);
        canary(other, "unguarded", "overflow");
        CHECK_FALSE(seen(other.sub_fd) == untouched);
    }
#endif

    TEST_CASE("SIGKILL is the one nothing inside can catch") {
        const Pty pty;
        const Seen before = seen(pty.sub_fd);
        const Ending ending = canary(pty, "guarded", "kill");
        CHECK(ending.signal == SIGKILL);
        // Broken: this is why `rmp run` puts the terminal back from outside.
        CHECK_FALSE(seen(pty.sub_fd) == before);
    }
#endif
}

#endif
