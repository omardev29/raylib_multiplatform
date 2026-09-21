// ---------------------------------------------------------------------------
// examples/scenes/01_stack/src/main.cpp
//
// The whole scene model: a menu that goes to a game, and a game that puts a
// pause overlay on top of itself. One scene per file under src/scenes/, the
// way Godot does it and the way a real project is laid out -- src/ is globbed
// recursively by all four build systems, so a new scene file costs no CMake,
// no Gradle and no Xcode edit.
//
// Three things are worth watching for, because they are what the stack buys
// you and none of them is written here:
//
//   1. PauseScene contains NO POLICY. The defaults are a pause menu -- what is
//      underneath freezes, stays visible, and stops receiving input -- so
//      pushing it stops the world. It works the same with five enemies as with
//      ten thousand, because what is suspended is the SCENE.
//   2. Nothing in GameScene knows the pause menu exists. There is no
//      `if (paused) return;` at the top of _update, and no `player.freeze()`.
//   3. `Scene::change<T>()` from inside a _draw() is safe. The transition is
//      deferred to the end of the frame, so the scene that asked for it
//      survives the rest of the frame it asked from.
//
// Built and booted by CI on every push, and by `just example 01_stack` here.
// ---------------------------------------------------------------------------

#include <rmp/app.h>

#include "scenes/main_menu.h"

// Four lines in a real project, and one of them is this. RMP_GAME opens the
// window from [window] in raylib_multiplatform.toml, enters this scene, and
// writes the entry point for whichever of the seventeen targets you are
// building.
RMP_GAME(MainMenuScene);
