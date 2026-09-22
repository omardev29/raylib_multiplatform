// ---------------------------------------------------------------------------
// examples/assets/01_loading/src/main.cpp
//
// How resources/ reaches your game, in both shapes it can arrive in:
//   - a packed, optionally AES-encrypted "resources.rres", or
//   - the loose files, while you are working.
// The same code reads either. Switch between them with:
//   cmake --build build --target pack_resources     # -> resources/resources.rres
//   cmake --build build --target unpack_resources   # back to loose files
//
// This example carries its own resources/ next to src/, and that is all it
// takes: an example with a resources/ folder reads that one, not the game's.
//
// Note what is missing: there is no rmp::assets::init() call anywhere below. The
// entry point macro opens the pack before on_ready() and closes it after
// on_exit(), so there is nothing to remember and nothing to get wrong.
//
// Built and booted by CI on every push, and by `just example` here.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/assets.h>

#include <vector>

// The counted handles, not the raylib structs. Each one owns what it loaded
// and releases it when it goes -- there is no Unload* anywhere in this file.
// Writing `Texture2D t = rmp::assets::load_texture(...)` instead would copy
// the struct out and destroy the only owner on the same line.
static rmp::Texture player;
static rmp::Font ui;
static rmp::Sound jump;

// Called once at startup: the pack (if any) is already open by now.
static inline void on_ready() {
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
    InitAudioDevice(); // LoadSound needs this first

    // Load by resource name — no path, no extension guessing, and no #ifdef
    // for "did this build get a pack or not".
    player = rmp::assets::load_texture("rabbit.png");
    ui = rmp::assets::load_font("ui.ttf", 20);
    jump = rmp::assets::load_sound("jump.wav");

    // Anything else, as bytes: a vector that frees itself, empty when the
    // file is not there.
    const std::vector<unsigned char> level = rmp::assets::load_data("level1.json");
    TraceLog(LOG_INFO, "level1.json: %d bytes", static_cast<int>(level.size()));

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

static inline void on_frame(float delta) {
    if (IsKeyPressed(KEY_SPACE)) PlaySound(jump);

    BeginDrawing();
    ClearBackground(RAYWHITE);
    const Texture2D &tex = player; // the raylib struct, borrowed for the call
    DrawTexture(tex, GetScreenWidth() / 2 - tex.width / 2,
                GetScreenHeight() / 2 - tex.height / 2, WHITE);
    DrawTextEx(ui,
               rmp::assets::using_pack() ? "serving from resources.rres"
                                         : "serving loose files",
               Vector2{ 10, 40 }, 20, 1, GRAY);
    EndDrawing();
}

static inline void on_exit() {
    // Release while the window and the audio device still exist. A handle
    // held in a global would otherwise let go after main() has returned.
    jump = {};
    ui = {};
    player = {};
    CloseAudioDevice();
    CloseWindow();
}

RMP_ENTRY_POINT(on_ready, on_frame, on_exit);
