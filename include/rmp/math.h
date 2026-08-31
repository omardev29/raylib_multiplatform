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

// raylib-cpp's math types, and ONLY those. They are classes that DERIVE from
// raylib's structs, so raylib::Vector2 converts to Vector2 on its own — which
// is why no rmp:: function signature ever mentions one. Our API keeps taking
// raylib's types; what you get is methods on top of them, if you want them:
//
//     position = position + velocity * delta;   // raymath's operators, plain Vector2
//     raylib::Vector2 v = aim; v.Normalize();   // raylib-cpp's methods
//
// The rest of raylib-cpp is deliberately not vendored. Its resource wrappers
// throw RaylibException on a failed load, and this framework does not throw —
// see rmp/assets.h. The math headers have no failure path at all, so taking
// only them removes the objection entirely rather than working around it.
//
// Quaternion.hpp is also left out, and not by preference: it redeclares
// raylib::Vector4, so including it alongside Vector4.hpp is a redefinition
// error. 2D does not need quaternions.
#include <raylib-cpp/Color.hpp>
#include <raylib-cpp/Matrix.hpp>
#include <raylib-cpp/Rectangle.hpp>
#include <raylib-cpp/Vector2.hpp>
#include <raylib-cpp/Vector3.hpp>
#include <raylib-cpp/Vector4.hpp>

// A couple of colours raylib does not ship. Add your own the same way — there
// is nothing in the framework that depends on these.
#ifndef ALICEBLUE
#define ALICEBLUE CLITERAL(Color){ 0, 240, 248, 255 }
#endif

#define GIORNOGOLD CLITERAL(Color){ 238, 207, 34, 255 } // The Golden Experience
