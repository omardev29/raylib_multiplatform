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
//
// YOU DO NOT INCLUDE THIS, and that is deliberate. Every public rmp/ header
// includes it — rmp/app.h, rmp/scene.h, rmp/ui.h, rmp/assets.h, rmp/ads.h,
// rmp/math.h, rmp/random.h — so whichever of them you were reaching for
// anyway, these values are already there.
//
// It is the single exception to the rule in rmp/app.h that our headers include
// none of our headers, and it earns it by measurement rather than by argument:
// twelve #defines, no includes of its own, 27 ms to parse against an empty
// file's 28. The rule is there to stop compile time and coupling from creeping
// in, and a leaf header below measurement noise does neither.
//
// The alternative was making people remember an include whose only job is to
// hand them a number the .toml already decided. That is a tax with nothing on
// the other side of it, and it was already being paid wrong: src/scenes/
// main_menu.cpp included this file AND rmp/ui.h, which had included it all
// along, and nothing anywhere said so.
//
// The one place you still write it yourself is plain C — see
// examples/plain_c/main.c — because a C file includes no rmp/ header at all.
// ---------------------------------------------------------------------------

#include <rmp/generated/config.h>
