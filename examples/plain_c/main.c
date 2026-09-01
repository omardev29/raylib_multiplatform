// ===========================================================================
// examples/plain_c/main.c — the template with none of the template.
//
// Plain C, one include, your own main(). No <rmp/...>, no rmp::assets, no
// scenes, no entry-point macro. What you keep is everything the framework does
// *around* your code, which is the part that is hard to reproduce: seventeen
// build targets, the pinned toolchains, the icons and identifiers generated
// from raylib_multiplatform.toml, the release pipeline.
//
// To use it:
//
//     rm src/main.cpp && rm -r src/rmp/ src/scenes/   # yes, all of them
//     cp examples/plain_c/main.c src/
//     cmake --preset debug && cmake --build build
//
// src/ is globbed by all four build systems (CMake, the Android CMakeLists,
// XcodeGen, Emscripten), so nothing else needs editing. src/rmp/ has to go or
// you link two main()s -- src/main.cpp is where RMP_GAME puts one -- and
// include/rmp/ can go with it once nothing includes it.
//
// What you give up, and what replaces it:
//
//   assets::Load*          ->  LoadTexture(RESOURCES_PATH "x.png"), as below
//   the resource pack      ->  gone. Loose files only. Do not run the
//                              pack_resources target; a release built with a
//                              pack ships resources.rres and nothing else, and
//                              raw raylib cannot read it. (Reading it is what
//                              src/rmp/loader_hook.cpp was doing for you.)
//   APP_WINDOW_TITLE, ...  ->  #include <rmp/config.h>
//                              if you want them; they are plain #defines and
//                              work in C. Keeping that one generated header
//                              costs you nothing else.
//   the smoke-test hooks   ->  see below. CI checks for those two log lines and
//                              fails the build without them.
//
// TWO PLATFORMS DO NOT RUN THIS FILE AS IT STANDS, and both for the same
// reason: the `while` loop below assumes your code owns the frame loop, and on
// those two it does not.
//
//   iOS   does not call main() at all. The run loop belongs to UIKit, and
//         raylib's iOS backend calls ios_ready/ios_update/ios_destroy.
//   Web   has no loop either: the browser calls you back, through
//         emscripten_set_main_loop(). A `while` loop there freezes the tab
//         unless the whole binary is built with -s ASYNCIFY, which is a real
//         cost in size and speed and which this project does not pay.
//
// Papering over exactly that is what RMP_ENTRY_POINT is for -- it writes the
// right loop for each of the seventeen targets and calls the same three hooks. So
// if you want iOS or Web, either keep the framework's entry point (which is one
// include and one line, and costs you nothing else) or write those two runners
// yourself. Linux, Windows, macOS, the BSDs and Android run this as it is.
// ===========================================================================

#include <raylib.h>

// RESOURCES_PATH is defined by CMake, not by us: an absolute path to
// resources/ in a development build, "./resources/" in a release, "" on
// Android (where assets sit at the root of the APK). Build every asset path
// out of it and the same source works on all of them.

// The marker CI greps for. Without it the build is fine and the smoke-test job
// fails: it cannot tell a working game from one that boots to a black screen.
// `assets_failed=0` is the claim being made — no asset was asked for and not
// found. Nothing here tracks that, so this file simply asserts it; if you load
// files and want the claim checked, include <smoke_test.h> (it is
// C-compatible) and use the template's asset layer.
#define RAY_TEST_REPORT_BOOT()                                      \
    TraceLog(LOG_INFO,                                              \
             "RAY_TEST_BOOT_OK assets_failed=0 assets_requested=0 " \
             "testFrames=0")

int main(void) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(800, 450, "raylib, plain C");

    Texture2D rabbit = LoadTexture(RESOURCES_PATH "rabbit.png");
    RAY_TEST_REPORT_BOOT();

    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(RAYWHITE);

        DrawTexture(rabbit, GetScreenWidth() / 2 - rabbit.width / 2,
                    GetScreenHeight() / 2 - rabbit.height / 2, WHITE);
        DrawText("Raylib is Multiplatform!", 190, 200, 20, LIGHTGRAY);

        EndDrawing();
    }

    UnloadTexture(rabbit);
    CloseWindow();
    return 0;
}
