#pragma once
// ===========================================================================
// raylib_multiplatform.h — the one header this template asks you to include.
//
//     #include <raylib_multiplatform.h>
//
// That single line gives you raylib, the values generated from
// raylib_multiplatform.toml, the Android bindings, the asset layer and the
// entry point.
//
// This file is an umbrella: the pieces live in include/raylib_multiplatform/
// and it only pulls them in. That way the template can grow without you ever
// adding an include to your own code — which is the whole point.
//
// What belongs to the template, and never to you:
//
//     include/raylib_multiplatform.h     this file
//     include/raylib_multiplatform/      its parts, and generated/
//     src/raylib_multiplatform/          the implementation
//
// Everything else under src/ and include/ is yours. If you would rather not
// use any of it, examples/main.c is a plain C entry point that includes only
// <raylib.h>.
// ===========================================================================

#include <raylib.h>

// Generated from raylib_multiplatform.toml by tools/configure.py:
// APP_NAME, APP_WINDOW_TITLE, APP_WINDOW_WIDTH/HEIGHT, APP_RRES_PASSWORD.
#include <raylib_multiplatform/generated/app_config.h>

#include <raylib_multiplatform/platform.h>   // Android bindings, ads, CI hook
#include <raylib_multiplatform/colors.h>     // extra colors
#include <raylib_multiplatform/assets.h>     // rmp::assets — resources/
#include <raylib_multiplatform/ads.h>        // rmp::ads    — interstitial/rewarded
#include <raylib_multiplatform/lifecycle.h>  // _ready/_process/_exit -> main()
