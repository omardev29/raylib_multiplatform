#include <raylib.h>
#ifdef __ANDROID__
#include <raymob.h>
#endif // __ANDROID__
#include <admob.h>
#include <assets.h>
#include <test.h>
#include <smoke_test.h> // CI smoke-test hook (lives in tests/)

// The raylib-iOS fork's raylib.h predates some of the extra named colors
// (e.g. ALICEBLUE) that upstream raylib defines. Provide a fallback so the
// same game code compiles on every backend.
#ifndef ALICEBLUE
#define ALICEBLUE CLITERAL(Color){0, 240, 248, 255}
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
static int lastRewardAmount = 0; // last rewarded-ad amount (demo feedback)

// ---------------------------------------------------------------------------
// CI smoke-test hook.
// The actual implementation lives in tests/smoke_test.h (header-only, include
// once). Set RAY_TEST_MAX_FRAMES=<N> to make the game boot, render <N> frames
// and exit cleanly. In a normal run it is unset and nothing changes.
// ---------------------------------------------------------------------------

// Called once at startup: set config flags, create the window, load assets.
static void _ready() {
#ifdef __ANDROID__
  Vibrate(2);
#endif

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(800, 450, "raylib [core] example - basic window");

  Assets::Init(); // use resources.rres if present, else loose files
  assets = LoadGameAssets();

  // AdMob: preload both ad types. These are no-ops outside Android, so the
  // same code runs everywhere.
  RequestInterstitialAd();
  RequestRewardedAd();

  // CI smoke-test hook: emit the boot marker (see tests/smoke_test.h).
  SmokeTest_ReportBoot(assets.rabbit.width, assets.rabbit.height);
}

static void _process(float delta) {
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
    DrawText(TextFormat("Last reward: %d", lastRewardAmount), 10, screen_y - 70,
             20, DARKGREEN);
  }

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
  // CI smoke-test hook: read the frame budget before the game starts.
  SmokeTest_Begin();
  _ready();

  // SmokeTest_Tick() lets the CI smoke test stop the loop after N frames.
  int smokeDone = 0;
  while (!WindowShouldClose() && !smokeDone) {
    _process(GetFrameTime());
    smokeDone = SmokeTest_Tick();
  }

  _exit();
  return 0;
}

#endif
