#pragma once
// ---------------------------------------------------------------------------
// rmp/config.h — the values that come from raylib_multiplatform.toml.
//
//     InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
//
// They are macros rather than constants on purpose: the plain-C entry point in
// examples/plain_c/ uses the same ones, and a C file has no namespaces to reach
// into.
//
// This header exists so that the word "generated" never appears in anyone's
// code. What it includes is rewritten by tools/configure.py on every configure
// and is not committed; this file is, so an editor that cannot find the other
// one still resolves the include.
// ---------------------------------------------------------------------------

#include <rmp/generated/config.h>
