#include <raylib_multi.h>

static GameAssets assets;

// Called once at startup: set config flags, create the window, load assets.
static void _ready() {

  // CI smoke-test hook: read the frame budget before the game starts.
  SmokeTest_Begin();

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(800, 450, "raylib [core] example - basic window");

  Assets::Init(); // use resources.rres if present, else loose files
  assets = LoadGameAssets();

  // CI smoke-test hook: emit the boot marker (see tests/smoke_test.h).
  SmokeTest_ReportBoot(assets.rabbit.width, assets.rabbit.height);
}

// Called each frame
static void _process(float delta) {

  int screen_x = GetScreenWidth();
  int screen_y = GetScreenHeight();

  BeginDrawing();
  ClearBackground(ALICEBLUE);

  DrawTexture(assets.rabbit, screen_x / 2 - assets.rabbit.width / 2,
              screen_y / 2 - assets.rabbit.height / 2, WHITE);

  DrawText("Omar's raylib template!", 190, 200, 20, LIGHTGRAY);

  EndDrawing();
}

// Called once at shutdown: unload assets, close the window.
static void _exit() {
  UnloadTexture(assets.rabbit);
  UnloadImage(assets.img);
  Assets::Shutdown();
  CloseWindow();
}

// ---------------------------------------------------------------------------
// Platform runner (entry point).
// ---------------------------------------------------------------------------
#if defined(PLATFORM_IOS)

// iOS rcore declares: extern void ios_ready(); ios_update(bool); ios_destroy();
// extern "C" so the symbols match the C declarations in rcore_ios.c.
extern "C" void ios_ready() { _ready(); }
extern "C" void ios_update(bool /*viewResized*/) { _process(GetFrameTime()); }
extern "C" void ios_destroy() { _exit(); }
#else

// Desktop, BSD, Android and Web all run a classic blocking loop.
int main() {
  _ready();

  // SmokeTest_Tick() lets the CI smoke test stop the loop after N frames.
  int smokeDone{};
  while (!WindowShouldClose() && !smokeDone) {
    _process(GetFrameTime());
    smokeDone = SmokeTest_Tick();
  }

  _exit();
  return 0;
}

#endif
