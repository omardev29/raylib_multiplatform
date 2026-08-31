// Your game starts here, and this is all of it.
//
// RMP_GAME opens the window from [window] in raylib_multiplatform.toml, enters
// the scene you name, runs it every frame, and shuts everything down on the way
// out — on all fourteen targets, including the two where main() is not a main()
// at all. Your work is in src/scenes/ and src/objects/.

#include <rmp/app.h>

#include "scenes/main_menu.h"

RMP_GAME(MainMenuScene);
