// The one translation unit that compiles cute_aseprite. Same shape and same
// reason as src/rmp/rres_impl.cpp: a single-header library's implementation is
// built once, and every other file includes the header clean.
//
// Excluded from clang-tidy by the `_impl` suffix, because vendored code's
// warnings are not ours to fix -- the rule is in CMakeLists.txt next to the
// other two.
#define CUTE_ASEPRITE_IMPLEMENTATION
#include <cute_aseprite.h>
