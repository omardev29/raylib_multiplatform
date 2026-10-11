#pragma once
// ---------------------------------------------------------------------------
// What one e2e target plays, and where its pictures go.
//
// tests/e2e/driver.cpp, script.cpp and compare.cpp are compiled ONCE, into an
// object library every e2e_<area>_<example> links (CMakeLists.txt,
// rmp_add_e2e). What differs from one target to the next is these four
// strings, which CMake writes into a file of the target's own
// (e2e_target/<target>.cpp in the build). They were -D defines on the driver,
// which made it sixteen translation units with one meaning: sixteen compiles
// in every e2e build, and sixteen clang-tidy runs of the same file -- a third
// of what the tidy job spent.
//
// A target that links the driver without its file does not link: the
// missing symbol is one of these, by name.
// ---------------------------------------------------------------------------

#include <string_view>

namespace e2e::target {

// tests/e2e/<area>/<example>.e2e, the script played.
extern const std::string_view SCRIPT;
// tests/e2e/golden/<area>/<example>/, one <shot>.png for every `shot`.
extern const std::string_view GOLDEN;
// build/examples-check/e2e/<area>/<example>/: expected, actual and diff of a
// shot that differed, and the run's saves.
extern const std::string_view OUT;
// <area>/<example>, as the run reports it.
extern const std::string_view NAME;

} // namespace e2e::target
