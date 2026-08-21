// ===========================================================================
// The Clay implementation, and nothing else.
//
// Same reason rres_impl.cpp exists: defining CLAY_IMPLEMENTATION compiles the
// whole layout engine, so it has to happen in exactly one translation unit.
// Every other file here includes clay.h plain and gets declarations only.
//
// Clay is C99 with an explicit C++20 path (it swaps compound literals for
// aggregate initialisation under __cplusplus), so this compiles as C++ on all
// fourteen targets, MSVC included.
// ===========================================================================

#define CLAY_IMPLEMENTATION
#include "clay.h"
