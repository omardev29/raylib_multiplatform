// ---------------------------------------------------------------------------
// The Linux console back, however a DRM/KMS game ends.
//
// raylib's DRM backend reads the keyboard from the terminal it was started on
// (InitKeyboard in rcore_drm.c, which SUPPORT_SSH_KEYBOARD_RPI turns on): it
// turns off echo, line editing and Ctrl+C (termios ICANON, ECHO and ISIG),
// makes the terminal non-blocking (O_NONBLOCK, a flag the shell shares) and
// puts a virtual console's keyboard in K_XLATE. It puts the three back from
// an atexit handler, and a crash, abort(), a sanitizer's report and a kill run
// none: the shell after it typed blind and Ctrl+C did nothing.
//
// The screen is not the problem. The kernel gives the console its screen back
// when the game's last handle on the card closes, which happens however the
// process ends; and raylib never sets KD_GRAPHICS, so there is no VT mode to
// restore.
//
// So guard_console(), called before InitWindow, remembers the terminal as it
// was and puts it back on a fatal signal -- then hands the signal on, so a
// crash is still a crash: a core dump, ASan's report, the status the shell
// sees -- on the sanitizers' way out, and at exit. The handler runs on an
// alternate stack, or a stack overflow would leave it none to run on. Nothing
// here may call anything a signal handler cannot: tcsetattr, fcntl, ioctl,
// sigaction, raise. tools/rmp.py does the same from outside after the game,
// for the SIGKILL nothing inside can catch.
//
// On any other platform, or a build that is not DRM/KMS, guard_console() is
// nothing: no other raylib backend touches the terminal.
// ---------------------------------------------------------------------------

#include "internal.h"

#include <string>
#include <string_view>

#if defined(__linux__) && !defined(__ANDROID__)
#include "save_internal.h"

#include <array>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <fcntl.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#if defined(RMP_SANITIZE) && __has_include(<sanitizer/common_interface_defs.h>)
#include <sanitizer/common_interface_defs.h>
#if defined(__GNUC__) && !defined(__clang__)
#include <dlfcn.h>
#endif
#endif
#endif

namespace rmp::app::detail {

#if defined(__linux__) && !defined(__ANDROID__)

namespace {

// What ends a game without its atexit handlers, other than SIGKILL. A signal
// whose disposition is SIG_IGN when the guard is set -- SIGHUP under nohup --
// is left ignored.
constexpr std::array FATAL_SIGNALS{ SIGSEGV, SIGBUS,  SIGFPE, SIGILL,  SIGTRAP, SIGSYS,
                                    SIGABRT, SIGTERM, SIGINT, SIGQUIT, SIGHUP };

// A stack overflow is a SIGSEGV with no stack left to run its handler on, and
// the kernel then ends the process without one, the terminal still broken. So
// the handler runs on an alternate stack: ASan's, which it sets up for its own
// report of the overflow, or -- in a build without it, a release -- this one.
// Far more than put_back() and a sigaction need, and than the largest signal
// frame a machine pushes (arm64's with SVE).
constexpr std::size_t ALTERNATE_STACK_SIZE = 64 * 1024;

// The terminal as it was, and the handlers ours stand in front of. Written
// before a handler is installed and only read after, so a handler never sees
// half of it.
struct Guard {
    int fd = -1;
    termios settings{};
    int flags = 0;
    int keyboard = -1; // -1: not a virtual console
    bool exit_registered = false;
    std::array<struct sigaction, FATAL_SIGNALS.size()> previous{};
    std::array<bool, FATAL_SIGNALS.size()> installed{};
    bool stack_installed = false; // the alternate stack below is the thread's
    alignas(16) std::array<std::byte, ALTERNATE_STACK_SIZE> stack{};
};

Guard guard;

// Async-signal-safe throughout: it runs inside a signal handler.
void put_back() {
    if (guard.fd < 0) return;
    tcsetattr(guard.fd, TCSANOW, &guard.settings);
    fcntl(guard.fd, F_SETFL, guard.flags);
    if (guard.keyboard >= 0) ioctl(guard.fd, KDSKBMODE, guard.keyboard);
}

// A fault the kernel raised (a positive si_code) happens again when the
// handler returns, and then reaches the handler that was there before ours;
// any other signal is raised again, and delivered to it once ours returns.
bool faults_again(int signal, int code) {
    const bool fault =
        signal == SIGSEGV || signal == SIGBUS || signal == SIGFPE || signal == SIGILL;
    return fault && code > 0;
}

// The handler: put the terminal back, give the signal back to whoever had it
// -- the default, a core dump; ASan's handler, its report -- and let it land.
// Indexing and not at(), which may throw, and nothing may here.
// NOLINTNEXTLINE(custom-rmp-raw-pointer): the signature sigaction() calls
void on_fatal_signal(int signal, siginfo_t *info, void * /*context*/) {
    put_back();
    for (std::size_t i = 0; i < FATAL_SIGNALS.size(); ++i) {
        if (FATAL_SIGNALS[i] == signal) sigaction(signal, &guard.previous[i], nullptr);
    }
    if (!faults_again(signal, info != nullptr ? info->si_code : 0)) raise(signal);
}

void put_back_at_exit() { put_back(); }

#if defined(RMP_SANITIZE) && __has_include(<sanitizer/common_interface_defs.h>)
// ASan and UBSan end the process with _exit() after their report: no atexit
// handler, no signal. Their death callback is the one way in -- and clang
// links the two as one runtime, gcc as two shared libraries with a copy of the
// callback each, where the name reaches only the first loaded, ASan's. So
// UBSan's is asked of libubsan itself (seen: a division by zero under gcc's
// UBSan left the terminal broken, under clang's it did not).
// NOLINTNEXTLINE(custom-rmp-raw-pointer): the runtimes' own signature
void on_sanitizer_death(void (*callback)()) {
    __sanitizer_set_death_callback(callback);
#if defined(__GNUC__) && !defined(__clang__)
    // NOLINTNEXTLINE(custom-rmp-raw-pointer): dlopen's own handle
    void *ubsan = dlopen("libubsan.so.1", RTLD_LAZY | RTLD_NOLOAD);
    if (ubsan == nullptr) return;
    // NOLINTNEXTLINE(custom-rmp-raw-pointer): dlsym's own
    void *set = dlsym(ubsan, "__sanitizer_set_death_callback");
    // NOLINTNEXTLINE(custom-rmp-raw-pointer): the function dlsym found
    if (set != nullptr) reinterpret_cast<void (*)(void (*)())>(set)(callback);
    dlclose(ubsan);
#endif
}
#endif

[[maybe_unused]] std::string from_environment(const std::string &name) {
    // NOLINTNEXTLINE(custom-rmp-raw-pointer): getenv's own, copied at once
    const char *value = std::getenv(name.c_str());
    return value != nullptr ? std::string(value) : std::string();
}

} // namespace

bool guard_terminal(int fd) {
    termios settings{};
    if (fd < 0 || isatty(fd) == 0 || tcgetattr(fd, &settings) != 0) return false;
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return false;
    int keyboard = -1;
    if (ioctl(fd, KDGKBMODE, &keyboard) != 0) keyboard = -1;

    unguard_terminal();
    guard.fd = fd;
    guard.settings = settings;
    guard.flags = flags;
    guard.keyboard = keyboard;

    for (std::size_t i = 0; i < FATAL_SIGNALS.size(); ++i) {
        struct sigaction current{};
        if (sigaction(FATAL_SIGNALS.at(i), nullptr, &current) != 0) continue;
        if ((current.sa_flags & SA_SIGINFO) == 0 && current.sa_handler == SIG_IGN)
            continue;
        struct sigaction ours{};
        ours.sa_sigaction = on_fatal_signal;
        // On the alternate stack (ALTERNATE_STACK_SIZE says whose), which is
        // what lets a stack overflow still reach it.
        ours.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&ours.sa_mask);
        guard.previous.at(i) = current;
        guard.installed.at(i) = sigaction(FATAL_SIGNALS.at(i), &ours, nullptr) == 0;
    }
    stack_t current{};
    if (!guard.stack_installed && sigaltstack(nullptr, &current) == 0 &&
        (current.ss_flags & SS_DISABLE) != 0) {
        stack_t ours{};
        ours.ss_sp = guard.stack.data();
        ours.ss_size = guard.stack.size();
        guard.stack_installed = sigaltstack(&ours, nullptr) == 0;
    }
#if defined(RMP_SANITIZE) && __has_include(<sanitizer/common_interface_defs.h>)
    on_sanitizer_death(put_back);
#endif
    if (!guard.exit_registered)
        guard.exit_registered = std::atexit(put_back_at_exit) == 0;
    return true;
}

void restore_terminal() { put_back(); }

void unguard_terminal() {
    for (std::size_t i = 0; i < FATAL_SIGNALS.size(); ++i) {
        if (guard.installed.at(i))
            sigaction(FATAL_SIGNALS.at(i), &guard.previous.at(i), nullptr);
        guard.installed.at(i) = false;
    }
    if (guard.stack_installed) {
        stack_t off{};
        off.ss_flags = SS_DISABLE;
        sigaltstack(&off, nullptr);
        guard.stack_installed = false;
    }
#if defined(RMP_SANITIZE) && __has_include(<sanitizer/common_interface_defs.h>)
    on_sanitizer_death(nullptr);
#endif
    // The atexit handler stays registered -- there is no taking one back --
    // and does nothing with no terminal to put back.
    guard.fd = -1;
}

std::string sudo_note(bool root, std::string_view sudo_user, std::string_view saves) {
    if (!root || sudo_user.empty() || sudo_user == "root") return {};
    const std::string user(sudo_user);
    return "CONSOLE: started with sudo by " + user +
        ", so the game runs as root and saves as root, in " + std::string(saves) +
        ". In the video and input groups, " + user +
        " can run it without sudo, and save as " + user;
}

void guard_console() {
#if defined(RMP_TARGET_DRM)
    // Started from no terminal at all -- a service, a pipe -- there is
    // nothing to guard, and nothing is installed.
    guard_terminal(STDIN_FILENO);
    // Where the saves go is read from the environment sudo left, and not
    // assumed: sudo's defaults give root's HOME, a sudoers that keeps HOME
    // gives the user's folder, written as root.
#if RMP_SAVE_PORTABLE
    const std::string saves = "saves/ next to the game";
#else
    const std::string saves = rmp::save::detail::user_folder();
#endif
    // A plain log, not RMP_REPORT_ONCE: being root is the machine, not a
    // mistake in the game, and [dev] strict must not abort it.
    const std::string note =
        sudo_note(geteuid() == 0, from_environment("SUDO_USER"), saves);
    if (!note.empty()) TraceLog(LOG_WARNING, "%s", note.c_str());
#endif
}

#else

bool guard_terminal(int /*fd*/) { return false; }
std::string sudo_note(bool /*root*/, std::string_view /*sudo_user*/,
                      std::string_view /*saves*/) {
    return {};
}
void restore_terminal() {}
void unguard_terminal() {}
void guard_console() {}

#endif

} // namespace rmp::app::detail
