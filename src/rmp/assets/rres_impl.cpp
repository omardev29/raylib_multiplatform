// ===========================================================================
// The rres implementation, and nothing else.
//
// This translation unit exists only to compile rres once. Defining
// RRES_IMPLEMENTATION / RRES_RAYLIB_IMPLEMENTATION pulls the container format,
// AES, Argon2i and QOI in as source, so it has to happen in exactly one .cpp —
// which is this one. Every other file here includes rres-raylib.h plain and
// gets declarations only.
//
// rres pulls tiny-AES, monocypher and qoi in as C sources. They compile clean
// as C++ (both clang and gcc, checked), and rres.h/rres-raylib.h wrap their
// declarations in extern "C", so the symbols are the same either way — which
// is what lets this be a .cpp instead of a .c.
// ===========================================================================

#include <raylib.h> // must precede rres-raylib.h; also provides ComputeMD5()

#define RRES_IMPLEMENTATION
#define RRES_RAYLIB_IMPLEMENTATION
// Encryption support. AES uses Argon2i from monocypher, which rres-raylib.h
// only includes when XCHACHA20 is enabled, so enable both to get a buildable
// AES path (also lets us read XChaCha20-packed files).
#define RRES_SUPPORT_ENCRYPTION_AES
#define RRES_SUPPORT_ENCRYPTION_XCHACHA20
#include "rres-raylib.h"
