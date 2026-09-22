// ===========================================================================
// The registry behind rmp::global<T>() — see the long comment in rmp/app.h for
// what it is and why it is called that.
//
// It is its OWN translation unit rather than part of src/rmp/app.cpp, and for a
// reason worth writing down: app.cpp references the entry point's guard symbol,
// so anything linking it needs an RMP_ENTRY_POINT — which a test binary does
// not have. Keeping the registry here is what makes rmp::global<T>() testable
// without a window, an entry point, or a game.
// ===========================================================================

#include <rmp/app.h>

#include "internal.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace rmp::app::detail {

namespace {
// One entry per rmp::global<T>() that has been asked for, in the order they
// were first used. The registry OWNS the instance; the header keeps a cached
// pointer that `forget` clears when the instance goes.
struct Global {
    std::shared_ptr<void> instance;
    void (*forget)();
};
std::vector<Global> g_globals;
// Built during shutdown_globals(): kept alive on purpose and never destroyed
// by us -- see register_global. The process is ending.
std::vector<std::shared_ptr<void>> g_orphans;
// True for the duration of shutdown_globals(). See register_global.
bool g_shutting_down = false;
} // namespace

void register_global(std::shared_ptr<void> instance, void (*forget)()) {
    // A global built DURING the shutdown would push an entry into a registry
    // that is about to be cleared and never drained again: the instance then
    // outlives CloseWindow(), and if it holds an rmp::Texture its slot is
    // released against a GL context that is gone -- which is the exact crash
    // rmp/app.h's teardown order exists to prevent, and which only showed up
    // under xvfb. It is reached by accident, from something running late in the
    // teardown that touches a global<T>() without knowing it is one.
    //
    // Refusing keeps a valid instance that is simply never destroyed by us
    // (the header's pointer stays good, so the caller gets a working object),
    // the process ends, and the report says where to look.
    if (g_shutting_down) {
        RMP_REPORT_ONCE("GLOBAL: a global was created while the globals were being "
                        "destroyed; it will not be destroyed by the framework. "
                        "Something reached for rmp::global<T>() during shutdown.");
        g_orphans.push_back(std::move(instance));
        return;
    }
    g_globals.push_back(Global{ std::move(instance), forget });
}

void shutdown_globals() {
    g_shutting_down = true;
    // Reverse of first use, which is the order anything that behaves like a
    // static is destroyed in — so a global that was built because another one
    // needed it still exists while that one is being taken apart. The header's
    // pointer is cleared BEFORE the instance goes, so a destructor that reaches
    // for the same global gets a fresh one (and the report above), never the
    // one being destroyed.
    //
    // An index walk and not std::ranges::reverse_view, deliberately: NetBSD
    // 10.1 ships GCC 10.5, whose <ranges> is incomplete, and that toolchain has
    // already cost this project one patch to Clay. A backwards for loop
    // compiles the same everywhere and reads no worse.
    for (std::size_t i = g_globals.size(); i > 0; i--) {
        Global &g = g_globals[i - 1];
        g.forget();
        g.instance.reset();
    }
    g_globals.clear();
    g_shutting_down = false;
}

} // namespace rmp::app::detail
