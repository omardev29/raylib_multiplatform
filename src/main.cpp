#include <raylib.h>
#ifdef __ANDROID__
#include <raymob.h>
#endif // __ANDROID__
#include <admob.h>
#include <assets.h>
#include <test.h>

#include <cstdlib>  // getenv, atoi

// The raylib-iOS fork's raylib.h predates some of the extra named colors
// (e.g. ALICEBLUE) that upstream raylib defines. Provide a fallback so the
// same game code compiles on every backend.
#ifndef ALICEBLUE
#define ALICEBLUE CLITERAL(Color){ 0, 240, 248, 255 }
#endif

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
static int lastRewardAmount = 0;   // last rewarded-ad amount (demo feedback)

// ---------------------------------------------------------------------------
// CI smoke-test hook.
// Set RAY_TEST_MAX_FRAMES=<N> to make the game boot, render <N> frames and
// exit cleanly (exit code 0) instead of looping until the window is closed.
// Used by the headless CI tests; in a normal run the variable is unset and the
// game behaves exactly as before.
// ---------------------------------------------------------------------------
static int g_frame = 0;
static int g_maxFrames = 0;      // 0 = run until the window is closed
static bool g_testWantsExit = false;

// Called once at startup: set config flags, create the window, load assets.
static void _ready() {
#ifdef __ANDROID__
  Vibrate(2);
#endif

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(800, 450, "raylib [core] example - basic window");

  Assets::Init();          // use resources.rres if present, else loose files
  assets = LoadGameAssets();

  // AdMob: preload both ad types. These are no-ops outside Android, so the
  // same code runs everywhere.
  RequestInterstitialAd();
  RequestRewardedAd();

  // CI smoke-test hook: read the frame budget (if any) and emit a boot marker.
  const char *envFrames = getenv("RAY_TEST_MAX_FRAMES");
  if (envFrames && envFrames[0]) g_maxFrames = atoi(envFrames);
  TraceLog(LOG_INFO, "RAY_TEST_BOOT_OK texture=%dx%d testFrames=%d",
           assets.rabbit.width, assets.rabbit.height, g_maxFrames);
}

// Called once per frame: update + draw. Use GetFrameTime() for delta time.
static void _process() {
  // AdMob example (no-op outside Android):
  //   SPACE -> show interstitial, R -> show rewarded. Preload the next one
  //   right after showing, and poll the reward flag each frame.
  if (IsKeyPressed(KEY_SPACE) && IsInterstitialAdLoaded()) {
    ShowInterstitialAd();
    RequestInterstitialAd();
  }
  if (IsKeyPressed(KEY_R) && IsRewardedAdLoaded()) {
    ShowRewardedAd();
    RequestRewardedAd();
  }
  if (TakeRewardEarned()) {
    lastRewardAmount = GetRewardAmount();
    TraceLog(LOG_INFO, "ADMOB: reward earned, amount=%d", lastRewardAmount);
    // Grant the reward to the player here (coins, extra life, ...).
  }

  int screen_x = GetScreenWidth();
  int screen_y = GetScreenHeight();

  BeginDrawing();
  ClearBackground(ALICEBLUE);

  DrawTexture(assets.rabbit, screen_x / 2 - assets.rabbit.width / 2,
              screen_y / 2 - assets.rabbit.height / 2, WHITE);

  DrawText("Omar's raylib template!", 190, 200, 20, LIGHTGRAY);
  DrawText("SPACE: interstitial   R: rewarded", 10, screen_y - 40, 20, GRAY);
  if (lastRewardAmount > 0) {
    DrawText(TextFormat("Last reward: %d", lastRewardAmount), 10, screen_y - 70, 20, DARKGREEN);
  }

  EndDrawing();

  // CI smoke-test hook: stop once the frame budget is exhausted.
  if (g_maxFrames > 0) {
    if (++g_frame >= g_maxFrames) {
      TraceLog(LOG_INFO, "RAY_TEST_DONE_FRAMES rendered=%d", g_frame);
      g_testWantsExit = true;
    }
  }
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

  // g_testWantsExit lets the CI smoke test stop the loop after N frames.
  while (!WindowShouldClose() && !g_testWantsExit) {
    _process();
  }

  _exit();
  return 0;
}

#endif
