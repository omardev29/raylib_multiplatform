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
// Run it: `rmp example 01_loading`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/audio.h>

#include <vector>

// The counted handles, not the raylib structs. Each one owns what it loaded
// and releases it when it goes -- there is no Unload* anywhere in this file.
// Writing `Texture2D t = rmp::assets::load_texture(...)` instead would copy
// the struct out and destroy the only owner on the same line.
static rmp::Texture player;
static rmp::Font ui;
static rmp::Sound jump;

// Called once at startup: the pack (if any) is already open by now.
static void on_ready() {
    InitWindow(RMP_WINDOW_WIDTH, RMP_WINDOW_HEIGHT, RMP_WINDOW_TITLE);

    // Load by resource name — no path, no extension guessing, and no #ifdef
    // for "did this build get a pack or not".
    player = rmp::assets::load_texture("rabbit.png");
    ui = rmp::assets::load_font("ui.ttf", 20);
    // The sound device opens here, on the first sound, with no
    // InitAudioDevice() and no CloseAudioDevice() anywhere: a game with no
    // sound never opens it, and on a machine without one this comes back
    // empty and the game runs silent.
    jump = rmp::assets::load_sound("jump.wav");

    // Anything else, as bytes: a vector that frees itself, empty when the
    // file is not there.
    const std::vector<unsigned char> level = rmp::assets::load_data("level1.json");
    TraceLog(LOG_INFO, "level1.json: %d bytes", static_cast<int>(level.size()));

    // Plain raylib works too, and reads the pack just the same: opening the
    // pack also routes raylib's own file loading through it. That is what
    // makes ::LoadModel(RMP_RESOURCES_PATH "ship.obj") work from a pack — raylib
    // reads the .obj, its .mtl and the textures the .mtl names through the
    // same LoadFileData the pack is hooked into.
    //
    // One exception: raylib's LoadMusicStream opens the file itself and never
    // sees the pack. rmp::audio::music("song") does, and so does
    // rmp::audio::play("jump") -- the same sound as above, by logical name,
    // with overlapping voices and no handle to keep.
}

static void on_frame(float /*delta*/) {
    if (IsKeyPressed(KEY_SPACE)) PlaySound(jump);
    if (IsKeyPressed(KEY_ENTER)) rmp::audio::play("jump");

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

static void on_exit() {
    // The framework has already released everything in the resource table
    // and closed the sound device by the time this runs; letting the handles
    // go is tidiness, not a requirement -- a handle that outlives the table
    // is empty, not dangling.
    jump = {};
    ui = {};
    player = {};
    CloseWindow();
}

RMP_ENTRY_POINT(on_ready, on_frame, on_exit);
