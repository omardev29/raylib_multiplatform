// ---------------------------------------------------------------------------
// examples/ui/08_overlay/src/main.cpp
//
// rmp::ui::overlay(): a pause menu, a "Really quit?", a game-over card --
// whatever has to be in front of the game, with the game dimmed or blurred
// behind it so that the eye goes where the player has to act.
//
// Two of them, and the difference between them is the point:
//
//   the pause    a scene of its own, PUSHED over the game. Its whole _draw()
//                is one overlay(), blurred; the balls below stop because a
//                pushed scene freezes what is under it by default.
//                src/scenes/pause.cpp.
//   the dialog   "Really quit?", drawn by the game scene itself after its HUD:
//                an overlay() with a panel() in it, which is all a dialog is
//                -- there is no dialog() to learn. Nothing is pushed, so
//                nothing stops: the balls go on behind the question. The
//                overlay takes the clicks, the focus and Enter; the rest is
//                the scene's to decide. src/scenes/game.cpp.
//
// The blur is a real one on every GPU raylib drives, OpenGL ES 2.0 and WebGL
// included. raylib's software renderer has no shaders, so the screenshots CI
// takes of this example show the veil alone.
//
// Run it: `rmp example 08_overlay`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>

#include "scenes/game.h"

RMP_GAME(GameScene);
