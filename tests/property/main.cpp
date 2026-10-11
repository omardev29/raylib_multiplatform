// ---------------------------------------------------------------------------
// property_test's main(): doctest's, after one default.
//
// RapidCheck reads RC_PARAMS once, at the first property, and without it seeds
// from std::random_device -- a different run every time, which `rmp test` and
// a push's CI must not be: a red that comes and goes with the seed teaches
// everybody to run it again. So the default is a fixed seed and 200 cases a
// property, and the environment wins: the nightly run sets
// RC_PARAMS="seed=<random> max_success=5000" and prints it, and a failure
// prints the RC_PARAMS="reproduce=..." that plays exactly that case again.
//
//   cmake --build --preset debug --target property_test
//   ./build/debug/property_test -ts="property: gen"
//   RC_PARAMS="seed=7 max_success=5000" ./build/debug/property_test
// ---------------------------------------------------------------------------

// doctest's knob, by doctest's name: its main() is written below, not by it.
#define DOCTEST_CONFIG_IMPLEMENT // NOLINT(readability-identifier-naming)
#include <doctest.h>

#include <cstdlib>

// NOLINTNEXTLINE(custom-rmp-raw-pointer): main's signature is the language's
int main(int argc, char **argv) {
#if defined(_WIN32)
    if (std::getenv("RC_PARAMS") == nullptr)
        _putenv_s("RC_PARAMS", "seed=20261010 max_success=200");
#else
    // 0: an RC_PARAMS already in the environment is left as it is.
    setenv("RC_PARAMS", "seed=20261010 max_success=200", 0);
#endif
    doctest::Context context(argc, argv);
    return context.run();
}
