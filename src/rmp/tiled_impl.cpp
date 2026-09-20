// The one translation unit that compiles cute_tiled, like rres_impl.cpp and
// aseprite_impl.cpp. The `_impl` suffix keeps it out of clang-tidy: vendored
// code's warnings are not ours to fix.
//
// NOTE: cute_tiled uses a struct's name before its typedef, which is legal C++
// and not legal C99, so this file is C++ and there is no C build of it.
#define CUTE_TILED_IMPLEMENTATION
#include <cute_tiled.h>
