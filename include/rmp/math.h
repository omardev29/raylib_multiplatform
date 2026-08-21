#pragma once
// ---------------------------------------------------------------------------
// rmp/math.h — vectors, rectangles, colours, and the arithmetic on them.
//
//     position = position + velocity * dt;
//
// That line is the whole reason this header exists. The operators come from
// raylib's own <raymath.h>, which defines them for C++ at the bottom of the
// file, so they work on the plain Vector2 that every raylib function already
// takes — there is no separate vector type to convert to or from.
//
// COST, because it is not the one you would guess. Measured with
// g++ -fsyntax-only -std=c++20, best of five:
//
//     <raylib.h>                 11 ms
//     <math.h> on its own        97 ms
//     <raylib.h> + <raymath.h>  125 ms
//
// The expensive part is the C library's <math.h>, not raymath's own 3 139
// lines. It is not a cost that can be avoided by choosing differently: any
// translation unit doing float arithmetic pays it. What it does decide is that
// rmp/ui.h must NOT include this header — a file that only draws menus has no
// business paying for it.
// ---------------------------------------------------------------------------

#include <raylib.h>
#include <raymath.h>

// A couple of colours raylib does not ship. Add your own the same way — there
// is nothing in the framework that depends on these.
#ifndef ALICEBLUE
#define ALICEBLUE CLITERAL(Color){ 0, 240, 248, 255 }
#endif

#define GIORNOGOLD CLITERAL(Color){ 238, 207, 34, 255 } // The Golden Experience
