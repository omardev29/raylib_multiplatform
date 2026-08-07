// rres implementation translation unit (C).
//
// rres-raylib.h pulls C sources (tiny-AES, monocypher, qoi) into whichever TU
// defines RRES_RAYLIB_IMPLEMENTATION. Keeping that TU in plain C avoids any
// C++ strictness issues with those embedded C files. The rest of the project
// calls these functions through their extern "C" declarations.

#include "raylib.h"  // also provides ComputeMD5(), used by rres's AES integrity check

#define RRES_IMPLEMENTATION
#define RRES_RAYLIB_IMPLEMENTATION
// Encryption support. AES uses Argon2i from monocypher, which rres-raylib.h
// only includes when XCHACHA20 is enabled, so enable both to get a buildable
// AES path (also lets us read XChaCha20-packed files).
#define RRES_SUPPORT_ENCRYPTION_AES
#define RRES_SUPPORT_ENCRYPTION_XCHACHA20

#include "rres-raylib.h"
