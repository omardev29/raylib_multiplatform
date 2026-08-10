#include <raylib_multi.h>

static GameAssets assets;

// Called once at startup: set config flags, create the window, load assets.
inlining void _ready() {

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(800, 450, "raylib [core] example - basic window");

  Assets::Init(); // use resources.rres if present, else loose files
  assets = LoadGameAssets();
}

// Called each frame
inlining void _process(float delta) {

  int screen_x{GetScreenWidth()};
  int screen_y{GetScreenHeight()};

  BeginDrawing();
  ClearBackground(ALICEBLUE);

  DrawTexture(assets.rabbit, screen_x / 2 - assets.rabbit.width / 2,
              screen_y / 2 - assets.rabbit.height / 2, WHITE);

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
  UnloadTexture(assets.rabbit);
  UnloadImage(assets.img);
  Assets::Shutdown();
  CloseWindow();
}

int main() { RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY; }
