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

#include <ranges>
#include <vector>

namespace rmp::app::detail {

namespace {
// One entry per rmp::global<T>() that has been asked for, in the order they
// were first used.
std::vector<void (*)()> g_globals;
} // namespace

void register_global(void (*destroy)()) { g_globals.push_back(destroy); }

void shutdown_globals() {
    // Reverse of first use, which is the order anything that behaves like a
    // static is destroyed in — so a global that was built because another one
    // needed it still exists while that one is being taken apart.
    for (void (*destroy)() : std::ranges::reverse_view(g_globals)) destroy();
    g_globals.clear();
}

} // namespace rmp::app::detail
