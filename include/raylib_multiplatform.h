#pragma once
// ===========================================================================
// raylib_multiplatform.h — the one header this template asks you to include.
//
//     #include <raylib_multiplatform.h>
//
// It pulls in raylib, the values generated from raylib_multiplatform.toml, the
// Android bindings, the asset layer and the lifecycle macros. There used to be
// three headers; they were merged into this one so that adding something to
// the template never means editing a file you own.
//
// This file, and src/raylib_multiplatform.cpp, are the template's. Your game
// goes in src/. If you would rather not use any of it, examples/main.c is a
// plain C entry point that includes only <raylib.h>.
// ===========================================================================

#include <raylib.h>

// Generated from raylib_multiplatform.toml by tools/configure.py:
// APP_NAME, APP_WINDOW_TITLE, APP_WINDOW_WIDTH/HEIGHT, APP_RRES_PASSWORD.
#include <generated/app_config.h>

#ifdef __ANDROID__
#include <raymob.h>
#endif // __ANDROID__
#include <admob.h>
#include <smoke_test.h> // CI smoke-test hook (lives in tests/); no-op outside CI

// cross compiler inlining macro to force the inlining
#if defined(_MSC_VER)
#define inlining static __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define inlining static inline __attribute__((always_inline))
#else
#define inlining static inline
#endif

// More colors
#ifndef ALICEBLUE
#define ALICEBLUE CLITERAL(Color){0, 240, 248, 255}
#endif
#define GIORNOGOLD CLITERAL(Color){238, 207, 34, 255} // The Golden Experience

// ---------------------------------------------------------------------------
// assets:: — loading from resources/
//
// Put your files in resources/ and load them by name. Which of the two ways
// they arrive is a build detail you do not have to think about:
//
//   - a packed resources.rres next to the executable (what a release ships),
//     optionally AES-encrypted;
//   - the loose files in resources/ (what you get while developing).
//
// assets::Init() picks whichever exists. You never call it: the lifecycle
// macro at the bottom of this file does, before _ready(), and Shutdown() after
// _exit().
//
// Since Init() also teaches raylib itself to read the pack, plain raylib calls
// work too — LoadTexture(RESOURCES_PATH "player.png"), LoadModel, LoadShader.
// The assets:: functions are the shorter spelling, not a requirement.
// See TECHNICAL.md, "Resources", for the two things that stay outside this:
// LoadMusicStream, and files loaded from outside resources/.
// ---------------------------------------------------------------------------

namespace assets {

// Detect and open the resource pack, if there is one, and route raylib's own
// file loading through it. Called for you by the lifecycle macro; calling it
// twice is harmless.
void Init();

// Release the pack and unhook raylib's loaders. Called for you after _exit().
void Shutdown();

// True when assets are being served from a .rres pack.
bool UsingPack();

// Load an image by its resource name (e.g. "rabbit.png"). The caller owns the
// returned Image and must UnloadImage() it.
Image LoadImage(const char *name);

// Convenience: load an image and upload it to the GPU in one step. The caller
// owns the returned Texture2D and must UnloadTexture() it.
Texture2D LoadTexture(const char *name);

// Load a sound effect (.wav/.ogg/.mp3/.qoa/...). InitAudioDevice() must have
// been called first. The caller owns it and must UnloadSound() it.
Sound LoadSound(const char *name);

// Load a font (.ttf/.otf). fontSize is the baked glyph size.
// The caller owns it and must UnloadFont() it.
Font LoadFont(const char *name, int fontSize);

// Raw bytes for anything else — a level file, a shader, JSON. `size` receives
// the byte count. Free with UnloadFileData().
unsigned char *LoadData(const char *name, int *size);

} // namespace assets

// ---------------------------------------------------------------------------
// The entry point.
//
// You write _ready(), _process(float) and _exit(); the macro below writes the
// runner for your platform. Desktop and Web get a main() with the frame loop;
// iOS gets the three callbacks UIKit expects, because there the run loop
// belongs to the OS and there is nothing to return to.
//
// Both spellings do the same four things around your code: start the smoke
// test, open the asset pack, run you, and close the pack afterwards.
// ---------------------------------------------------------------------------

// iOS rcore declares: extern void ios_ready(); ios_update(bool); ios_destroy();
// extern "C" so the symbols match the C declarations in rcore_ios.c.
//
// The SmokeTest_Tick() branch is what makes the app terminable under CI. On
// desktop the frame budget just ends the while loop in main(); UIKit owns the
// run loop here and offers no way to return from it, so the CI path tears down
// and exits explicitly. Outside CI (RAY_TEST_MAX_FRAMES unset) Tick() always
// returns 0 and this is dead code.
#define IOS_FUNCS                                                              \
  extern "C" void ios_ready() {                                                \
    /* iOS starts the process in the app container, not inside the bundle, and \
       raylib's iOS backend does not chdir for you — so the relative         \
       RESOURCES_PATH would resolve to nothing and every asset would silently  \
       load as 0x0. GetApplicationDirectory() is the .app root on iOS, which   \
       is exactly where bundle resources live. This has to happen before       \
       assets::Init(), which looks for the pack along that same path. */       \
    SmokeTest_Begin();                                                         \
    ChangeDirectory(GetApplicationDirectory());                                \
    assets::Init();                                                            \
    _ready();                                                                  \
  }                                                                            \
  extern "C" void ios_update(bool /*viewResized*/) {                           \
    _process(GetFrameTime());                                                  \
    if (SmokeTest_Tick()) {                                                    \
      _exit();                                                                 \
      assets::Shutdown();                                                      \
      exit(0);                                                                 \
    }                                                                          \
  }                                                                            \
  extern "C" void ios_destroy() {                                              \
    _exit();                                                                   \
    assets::Shutdown();                                                        \
  }

// Main loop body macro
// 1. Definición condicional de la macro
#if defined(PLATFORM_IOS)
#define RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY IOS_FUNCS
#else
#define RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY                                    \
  int main() {                                                                 \
    SmokeTest_Begin();                                                         \
    assets::Init();                                                            \
    _ready();                                                                  \
    int smokeDone = 0;                                                         \
    while (!WindowShouldClose() && !smokeDone) {                               \
      _process(GetFrameTime());                                                \
      smokeDone = SmokeTest_Tick();                                            \
    }                                                                          \
    _exit();                                                                   \
    assets::Shutdown();                                                        \
    return 0;                                                                  \
  }
#endif
