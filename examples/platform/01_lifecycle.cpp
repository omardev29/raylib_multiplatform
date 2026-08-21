// ---------------------------------------------------------------------------
// examples/platform/01_lifecycle.cpp
//
// The template's core pattern: a Godot SceneTree-style lifecycle.
//
// You write your game in three hooks and the platform runner drives them on
// EVERY target (desktop, BSD, Web, Android use a blocking loop; iOS uses
// OS-driven frame callbacks). You normally copy this skeleton into
// src/main.cpp and fill in the three hooks.
//
//   on_ready()   -> once at startup: window, assets, preload
//   on_frame() -> every frame: update + draw (use GetFrameTime() for delta)
//   on_exit()    -> once at shutdown: unload, close
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <raylib.h>

// Called once at startup: set config flags, create the window, load assets.
static void on_ready() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE); // optional, before InitWindow
    InitWindow(800, 450, "my game");

    // Load your assets here (see assets_rres_loading.cpp).
}

// Called once per frame: update + draw. Use GetFrameTime() for delta time.
static void on_frame() {
    float dt = GetFrameTime();
    (void)dt; // use dt to move things frame-rate independently

    BeginDrawing();
    ClearBackground(RAYWHITE);
    DrawText("Hello from on_frame()!", 190, 200, 20, LIGHTGRAY);
    EndDrawing();
}

// Called once at shutdown: unload assets, close the window.
static void on_exit() {
    // UnloadTexture(...), UnloadImage(...), ...
    CloseWindow();
}

// ---------------------------------------------------------------------------
// Platform runner. Desktop / BSD / Android / Web use this loop. iOS maps the
// same hooks onto ios_ready/ios_update/ios_destroy (see src/main.cpp) — you do
// NOT write per-platform code, the runner is already provided by the template.
// ---------------------------------------------------------------------------
int main() {
    on_ready();
    while (!WindowShouldClose()) {
        on_frame();
    }
    on_exit();
    return 0;
}
