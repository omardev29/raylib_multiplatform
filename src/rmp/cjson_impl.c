/* ---------------------------------------------------------------------------
 * cJSON, compiled once, as C.
 *
 * src/rmp/save.cpp is the only file that includes cJSON.h, and no public
 * header does: the JSON library is an implementation detail of rmp::save the
 * way Clay is of rmp::ui, so replacing it touches no game. It is compiled
 * here and not listed as a source because every build already globs src/rmp
 * -- CMake, Android's raymob CMakeLists and the generated Xcode project --
 * and a source listed in three places is a source missing from one of them.
 *
 * thirdparty/cJSON is v1.7.19, unmodified; its sha256 is pinned in
 * thirdparty/FROZEN_VERSIONS.md.
 * ------------------------------------------------------------------------- */

/* The parser reads numbers with strtod, which follows the C locale's decimal
 * point: a game that calls setlocale() for a German UI would find "0.5" in
 * its own save unreadable. With ENABLE_LOCALES cJSON swaps '.' for the
 * locale's point before strtod, and reads saves the same in every locale.
 * (Numbers are WRITTEN by src/rmp/save.cpp, locale-independently.) */
#define ENABLE_LOCALES

#include <cJSON.c>
