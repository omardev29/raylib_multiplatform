// ---------------------------------------------------------------------------
// examples/assets_rres_loading.cpp
//
// How resources/ reaches your game, in both shapes it can arrive in:
//   - a packed, optionally AES-encrypted "resources.rres", or
//   - the loose files, while you are working.
// The same code reads either. Switch between them with:
//   cmake --build build --target pack_resources     # -> resources/resources.rres
//   cmake --build build --target unpack_resources   # back to loose files
//
// Note what is missing: there is no assets::Init() call anywhere below. The
// entry point macro opens the pack before _ready() and closes it after
// _exit(), so there is nothing to remember and nothing to get wrong.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <raylib_multiplatform.h>

static Texture2D player;
static Font ui;
static Sound jump;

// Called once at startup: the pack (if any) is already open by now.
inlining void _ready() {
    InitWindow(800, 450, "assets example");
    InitAudioDevice();                  // LoadSound needs this first

    // Load by resource name — no path, no extension guessing, and no #ifdef
    // for "did this build get a pack or not".
    player = assets::LoadTexture("rabbit.png");
    ui     = assets::LoadFont("ui.ttf", 20);
    jump   = assets::LoadSound("jump.wav");

    // Anything else, as bytes. Free it with UnloadFileData().
    int size = 0;
    unsigned char *level = assets::LoadData("level1.json", &size);
    if (level != nullptr) UnloadFileData(level);

    // Plain raylib works too, and reads the pack just the same: opening the
    // pack also routes raylib's own file loading through it. That is what
    // makes ::LoadModel(RESOURCES_PATH "ship.obj") work from a pack — raylib
    // reads the .obj, its .mtl and the textures the .mtl names through the
    // same LoadFileData the pack is hooked into.
    //
    // One exception: LoadMusicStream opens the file itself, so it can stream
    // the song instead of holding it in memory, and never sees the pack. Ship
    // music as a loose file next to the executable.
}

inlining void _process(float delta) {
    if (IsKeyPressed(KEY_SPACE)) PlaySound(jump);

    BeginDrawing();
    ClearBackground(RAYWHITE);
    DrawTexture(player, GetScreenWidth() / 2 - player.width / 2,
                GetScreenHeight() / 2 - player.height / 2, WHITE);
    DrawTextEx(ui, assets::UsingPack() ? "serving from resources.rres"
                                       : "serving loose files",
               Vector2{10, 40}, 20, 1, GRAY);
    EndDrawing();
}

inlining void _exit() {
    UnloadSound(jump);
    UnloadFont(ui);
    UnloadTexture(player);
    CloseAudioDevice();
    CloseWindow();
}

RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY;
