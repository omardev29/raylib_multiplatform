// ---------------------------------------------------------------------------
// examples/games/07_platformer/src/main.cpp — a platformer, designed in LDtk.
//
// The seventh judge, and the first with a level that somebody designs in an
// editor instead of a formation written in code. Three levels side by side in
// one LDtk world -- a meadow, a desert and a snowfield -- with Kenney's Pixel
// Platformer art, walked through from left to right to the flag.
//
// What the framework does here, and therefore what is NOT in these files:
//
//   rmp::Tilemap    resources/world.ldtk, loaded by name. The scene draws it,
//                   the player stands on its IntGrid `Solid` cells and runs
//                   along them without catching on a seam, and the crate in the
//                   meadow is solid because its tile is tagged `Solid` in the
//                   tileset. No collision code anywhere in the game.
//   entities        every coin, enemy, sign and door is placed in LDtk and
//                   arrives through map.on_object(): a patrol is an
//                   Array<Point> drawn in the editor, a platform's destination
//                   is a Point, the key names its door with an EntityRef, a
//                   sign's text is a String and its arrow an Enum.
//   the world       walking off the right edge of a level is
//                   map.neighbour_at() and a scene change; the player arrives
//                   at the same world position in the next one.
//   Platformer      the run, the jump, coyote time and the buffered jump.
//   rmp::Camera     follows with smoothing, stops at the level's edges
//                   (`limits = map.bounds()`), and shakes when you are hit.
//   rmp::audio      the music loop and the effects, by name; the music does not
//                   restart when the level changes, because asking for the
//                   track that is playing is not a restart.
//   rmp::save       the best run, kept between sessions.
//   rmp::global     the run itself -- lives, coins, the clock, what has been
//                   collected -- which has to survive the change of scene.
//
// What is left is the game: what a coin is worth, what a stomp does, how long
// you are safe after a hit, and when you have won.
//
// THE LEVEL IS LDtk's. tools/make_platformer_assets.py generated the first
// version and cut the art out of Kenney's packs; from there world.ldtk is
// edited in LDtk 1.5, and the game reads whatever it saves. Paint IntGrid
// `Solid` and the auto-layer rules draw the edges and corners; set a level's
// `biome` to Grass, Sand or Snow and they draw that terrain.
// ---------------------------------------------------------------------------

#include <rmp/app.h>

#include "game.h"

RMP_GAME(game::TitleScene);
