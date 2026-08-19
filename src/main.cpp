#include <cstdlib>
#include <raylib_multiplatform.h>

class GameAssets {
public:
  Image img;
  Texture2D rabbit;
} game;

int screen_x{};
int screen_y{};

// Called once at startup: set config flags, create the window, load assets.
//
// The resource pack is already open by the time you get here: the entry point
// in raylib_multiplatform.h calls rmp::assets::init() before this, and
// rmp::assets::shutdown() after _exit(). Neither is yours to remember.
static inline void _ready() {

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  // Title and size come from raylib_multiplatform.toml — see [window].
  InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);

  // Served from resources.rres when a release packed one, from the loose file
  // in resources/ otherwise. Same call either way.
  game.img = rmp::assets::load_image("rabbit.png");
  game.rabbit = LoadTextureFromImage(game.img);
}

// Called each frame
static inline void _process(float delta) {

  screen_x = GetScreenWidth();
  screen_y = GetScreenHeight();

  BeginDrawing();
  ClearBackground(ALICEBLUE);

  DrawTexture(game.rabbit, screen_x / 2 - game.rabbit.width / 2,
              screen_y / 2 - game.rabbit.height / 2, WHITE);

  // A menu, in three calls. It stays centred and keeps its proportions at any
  // window size — resize the window and watch. Nothing here mentions a
  // coordinate, a font or a hitbox.
  rmp::ui::begin();
  rmp::ui::text("Raylib is Multiplatform!");
  if (rmp::ui::button("Play"))
    TraceLog(LOG_INFO, "MENU: play");
  if (rmp::ui::button("Options"))
    TraceLog(LOG_INFO, "MENU: options");
  if (rmp::ui::button("Quit"))
    std::exit(0);
  rmp::ui::end();

  // CI smoke-test hook: read the frame back and check something was actually
  // drawn. Must sit here, between the last draw call and EndDrawing() — see
  // tests/smoke_test.h for why either side of that line is wrong. No-op unless
  // RAY_TEST_MAX_FRAMES is set.
  SmokeTest_CaptureFrame();

  EndDrawing();
}

// Called once at shutdown: unload assets, close the window.
static inline void _exit() {
  UnloadTexture(game.rabbit);
  UnloadImage(game.img);
  CloseWindow();
}

// Main function or ios functions + smoke tests
RAYLIB_MULTIPLATFORM_MAIN_LOOP_BODY;
