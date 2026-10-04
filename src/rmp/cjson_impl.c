/* ---------------------------------------------------------------------------
 * cJSON, compiled once, as C.
 *
 * src/rmp/save.cpp and src/rmp/ldtk.cpp include cJSON.h, through
 * json_internal.h, and no public header does: the JSON library is an
 * implementation detail of rmp::save and of the LDtk reader the way Clay is of
 * rmp::ui, so replacing it touches no game. It is compiled
 * here and not listed as a source because every build already globs src/rmp
 * -- CMake, Android's raymob CMakeLists and the generated Xcode project --
 * and a source listed in three places is a source missing from one of them.
 *
 * thirdparty/cJSON is v1.7.19, unmodified; its sha256 is pinned in
 * thirdparty/FROZEN_VERSIONS.md.
 * ------------------------------------------------------------------------- */

/* How deep a save may nest. The same number as rmp::save::detail::MAX_DEPTH
 * in save_internal.h, which is C++ and cannot be included here; the nesting
 * test in tests/save_test.cpp fails if the two drift apart. The default of
 * 1000 overflowed the web's 64 KB stack long before it was reached.
 * (Numbers are read and written under the "C" locale by src/rmp/save.cpp, so
 * ENABLE_LOCALES is not wanted: it took one byte of a two-byte point.) */
#define CJSON_NESTING_LIMIT 64

#include <cJSON.c>
