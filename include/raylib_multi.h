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
#define ALICEBLUE CLITERAL(Color){0, 240, 248, 255}
#define GIORNOGOLD CLITERAL(Color){238, 207, 34, 255} // The Golden Experience
