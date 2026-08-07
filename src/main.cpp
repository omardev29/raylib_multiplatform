#include <raylib.h>
#ifdef __ANDROID__
#include <raymob.h>
#endif // __ANDROID__
#include <assets.h>
#include <test.h>

// ---------------------------------------------------------------------------
// Game lifecycle (Godot SceneTree style).
//
// Implement the game in _ready() / _process() / _exit(). The platform runner
// at the bottom of this file drives these hooks on every target:
//   - Desktop / BSD / Android / Web (-s ASYNCIFY): a normal main() loop.
//   - iOS: callback-driven (ios_ready/ios_update/ios_destroy), because iOS has
//     no blocking main loop; the OS drives the frame callback (CADisplayLink).
// ---------------------------------------------------------------------------

static GameAssets assets;

// Called once at startup: set config flags, create the window, load assets.
static void _ready() {
#ifdef __ANDROID__
  Vibrate(2);
#endif

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(800, 450, "raylib [core] example - basic window");

  Assets::Init();          // use resources.rres if present, else loose files
  assets = LoadGameAssets();
}

// Called once per frame: update + draw. Use GetFrameTime() for delta time.
static void _process() {
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
extern "C" void ios_update(bool /*viewResized*/) { _process(); }
extern "C" void ios_destroy() { _exit(); }

#else

// Desktop, BSD, Android and Web all run a classic blocking loop.
int main() {
  _ready();

  while (!WindowShouldClose()) {
    _process();
  }

  _exit();
  return 0;
}

#endif
