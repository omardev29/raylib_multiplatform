// ===========================================================================
// examples/plain_c -- the framework with none of the framework.
//
// Plain C, your own main(), raylib and nothing of ours but the CI hooks. No
// rmp::assets, no scenes, no entry-point macro. What you keep is everything
// the framework does *around* your code, which is the part that is hard to
// reproduce: the build targets, the pinned toolchains, the icons and
// identifiers generated from raylib_multiplatform.toml, the release pipeline.
//
// To use it, in a game made with `rmp new`, from the game's folder:
//
//     rm src/main.cpp && rm -r src/rmp/ src/scenes/
//     cp <the framework>/examples/plain_c/src/main.c src/
//     rmp run
//
// src/ is globbed by all four build systems (CMake, the Android CMakeLists,
// XcodeGen, Emscripten), so nothing else needs editing. src/rmp/ has to go or
// you link two main()s -- src/main.cpp is where RMP_GAME puts one -- and with
// it gone CMakeLists.txt builds no framework library at all. Keep include/rmp/:
// this file reads [window] from <rmp/config.h>, the one header of ours that is
// plain #defines. The rmp new CI job follows the first two lines in a fresh
// game and boots what they make.
//
// What you give up, and what replaces it:
//
//   assets::Load*          ->  LoadTexture(RMP_RESOURCES_PATH "x.png"), as below
//   the resource pack      ->  gone. Loose files only: a release ships
//                              resources/ as it is, with a PACK_SKIPPED.txt
//                              saying why (tools/ship_resources.sh), because
//                              raw raylib cannot read resources.rres -- reading
//                              it is what src/rmp/loader_hook.cpp was doing.
//   RMP_WINDOW_TITLE, ...  ->  #include <rmp/config.h>, as below: [window] and
//                              the rest of the .toml as plain #defines, in C.
//   the smoke-test hooks   ->  <smoke_test.h>, as below. It is C, it is on the
//                              include path, and every CI boot greps the lines it
//                              prints: a boot, a frame read back, a clean exit.
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
// right loop for each target and calls the same three hooks. So if you want
// iOS or Web, either keep the framework's entry point (which is one include and
// one line, and costs you nothing else) or write those two runners yourself.
// Linux, Windows, macOS, the BSDs and Android run this as it is.
// ===========================================================================

#include <raylib.h>
#include <rmp/config.h> // [window] from raylib_multiplatform.toml, as #defines
#include <smoke_test.h> // the CI hooks: no-ops unless RAY_TEST_MAX_FRAMES is set

// RMP_RESOURCES_PATH is defined by CMake, not by us: an absolute path to
// resources/ in a development build, "./resources/" in a release, "" on
// Android (where assets sit at the root of the APK). Build every asset path
// out of it and the same source works on all of them.

int main(void) {
    SmokeTest_Begin();
#if RMP_PRODUCTION_BUILD && !defined(PLATFORM_ANDROID) && !defined(__EMSCRIPTEN__)
    // A release reads "./resources/", and "." has to be the executable's folder,
    // not wherever it was started from -- a file manager, a shortcut, another
    // terminal. The framework's entry point does this for a C++ game (see
    // enter_executable_folder() in src/rmp/app.cpp); here it is yours.
    ChangeDirectory(GetApplicationDirectory());
#endif
    // [window] in the .toml, the way RMP_GAME opens it: resizable, vsync if it
    // says so, and its size and title. CI checks the window was asked for the
    // vsync the .toml names.
    SetConfigFlags(RMP_WINDOW_VSYNC ? FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT
                                    : FLAG_WINDOW_RESIZABLE);
    InitWindow(RMP_WINDOW_WIDTH, RMP_WINDOW_HEIGHT, RMP_WINDOW_TITLE);

    Texture2D rabbit = LoadTexture(RMP_RESOURCES_PATH "rabbit.png");
    // One asset asked for, and failed if it came back empty: what the boot
    // line reports and every CI boot requires to be assets_failed=0.
    SmokeTest_ReportBoot(rabbit.id == 0 ? 1 : 0, 1);

    // The window's X ends it, and under CI so does the frame budget.
    while (!WindowShouldClose() && !SmokeTest_Done()) {
        BeginDrawing();
        ClearBackground(RAYWHITE);

        DrawTexture(rabbit, GetScreenWidth() / 2 - rabbit.width / 2,
                    GetScreenHeight() / 2 - rabbit.height / 2, WHITE);
        DrawText("Raylib is Multiplatform!", 190, 200, 20, LIGHTGRAY);

        SmokeTest_CaptureFrame(); // between the last draw and EndDrawing()
        EndDrawing();
        SmokeTest_Tick();
    }

    UnloadTexture(rabbit);
    CloseWindow();
    return 0;
}
