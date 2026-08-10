// ---------------------------------------------------------------------------
// examples/assets_rres_loading.cpp
//
// How to load assets through the template's dual-mode layer (include/assets.h).
//
// The same code loads from:
//   - a packed, optionally AES-encrypted "resources.rres" if one exists, or
//   - loose files otherwise.
// You switch between the two with NO code changes. Pack your assets with:
//   cmake --build build --target pack_resources     # -> resources/resources.rres
//   cmake --build build --target unpack_resources   # back to loose files
//
// Always go through Assets::* instead of raw LoadTexture(RESOURCES_PATH ...)
// so your game works in both modes.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <raylib.h>
#include <assets.h>   // Assets::Init / LoadImage / LoadTexture / ...

static Texture2D player;

static void _ready() {
    InitWindow(800, 450, "assets example");

    Assets::Init(); // detect + open resources.rres if present. Call FIRST.

    // Load by resource name (relative to the pack root / resources folder).
    // LoadTexture() uploads to the GPU in one step; you own it and must
    // UnloadTexture() it later. Use LoadImage() if you need the pixels.
    player = Assets::LoadTexture("rabbit.png");
}

static void _process() {
    BeginDrawing();
    ClearBackground(RAYWHITE);
    DrawTexture(player, GetScreenWidth() / 2 - player.width / 2,
                GetScreenHeight() / 2 - player.height / 2, WHITE);
    DrawText(Assets::UsingPack() ? "serving from resources.rres"
                                 : "serving loose files", 10, 40, 20, GRAY);
    EndDrawing();
}

static void _exit() {
    UnloadTexture(player);
    Assets::Shutdown(); // release the pack's central directory
    CloseWindow();
}

int main() {
    _ready();
    while (!WindowShouldClose()) _process();
    _exit();
    return 0;
}
