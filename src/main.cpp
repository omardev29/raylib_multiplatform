#include <raylib_multiplatform.h>

// This file is yours. Everything below is the template's demo — replace it.

struct GameAssets {
  Texture2D rabbit;
  Image img;
};

static GameAssets game;

int screen_x{};
int screen_y{};

// Called once at startup: set config flags, create the window, load assets.
//
// The resource pack is already open by the time you get here: the entry point
// in raylib_multiplatform.h calls assets::Init() before this, and
// assets::Shutdown() after _exit(). Neither is yours to remember.
inlining void _ready() {

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  // Title and size come from raylib_multiplatform.toml — see [window].
  InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);

  // Served from resources.rres when a release packed one, from the loose file
  // in resources/ otherwise. Same call either way.
  game.img = assets::LoadImage("rabbit.png");
  game.rabbit = LoadTextureFromImage(game.img);
}

// Called each frame
inlining void _process(float delta) {

  screen_x = GetScreenWidth();
  screen_y = GetScreenHeight();

  BeginDrawing();
  ClearBackground(ALICEBLUE);

  DrawTexture(game.rabbit, screen_x / 2 - game.rabbit.width / 2,
              screen_y / 2 - game.rabbit.height / 2, WHITE);

  DrawText("Raylib is Multiplatform!", 190, 200, 20, LIGHTGRAY);

  // CI smoke-test hook: read the frame back and check something was actually
  // drawn. Must sit here, between the last draw call and EndDrawing() — see
  // tests/smoke_test.h for why either side of that line is wrong. No-op unless
  // RAY_TEST_MAX_FRAMES is set.
  SmokeTest_CaptureFrame();

  EndDrawing();
}

// Called once at shutdown: unload assets, close the window.
inlining void _exit() {
  UnloadTexture(game.rabbit);
  UnloadImage(game.img);
  CloseWindow();
}

// Main function or ios functions + smoke tests
RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY;
