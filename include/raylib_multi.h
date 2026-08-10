#pragma once

#include <raylib.h>
#ifdef __ANDROID__
#include <raymob.h>
#endif // __ANDROID__
#include <admob.h>
#include <assets.h>
#include <smoke_test.h> // CI smoke-test hook (lives in tests/)
#include <test.h>

// More colors
#ifndef ALICEBLUE
#define ALICEBLUE CLITERAL(Color){0, 240, 248, 255}
#endif
#define GIORNOGOLD CLITERAL(Color){238, 207, 34, 255} // The Golden Experience

// iOS rcore declares: extern void ios_ready(); ios_update(bool); ios_destroy();
// extern "C" so the symbols match the C declarations in rcore_ios.c.
#define IOS_FUNCS                                                              \
  extern "C" void ios_ready() { _ready(); }                                    \
  extern "C" void ios_update(bool /*viewResized*/) {                           \
    _process(GetFrameTime());                                                  \
  }                                                                            \
  extern "C" void ios_destroy() { _exit(); }
