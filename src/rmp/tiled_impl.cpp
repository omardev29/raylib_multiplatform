// The one translation unit that compiles cute_tiled, like rres_impl.cpp and
// aseprite_impl.cpp. The `_impl` suffix keeps it out of clang-tidy: vendored
// code's warnings are not ours to fix.
//
// NOTE: cute_tiled uses a struct's name before its typedef, which is legal C++
// and not legal C99, so this file is C++ and there is no C build of it.
// An external tileset is fetched by src/rmp/tilemap.cpp through rmp::assets
// (so it works from the rres pack too); cute_tiled's own printf telling the
// user to embed it would be wrong advice now.
//
// Its other warnings go to raylib's log at LOG_DEBUG, not to stdout with
// printf: that skipped the game's log level, said every refusal a second time
// (src/rmp/tilemap.cpp already says why a map was refused, with its name and
// what to do), and broke tools/fuzz.sh's count of the inputs a replay ran --
// a warning cut by stdout's buffer took the start of a line with it.
#include <raylib.h>

#define CUTE_TILED_WARNING(msg) TraceLog(LOG_DEBUG, "MAP: cute_tiled: %s", (msg))
#define CUTE_TILED_NO_EXTERNAL_TILESET_WARNING
#define CUTE_TILED_IMPLEMENTATION
#include <cute_tiled.h>
