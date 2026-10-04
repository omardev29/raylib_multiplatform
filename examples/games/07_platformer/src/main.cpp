// ---------------------------------------------------------------------------
// examples/games/07_platformer -- a platformer whose levels are made in LDtk.
//
// Three levels side by side in one LDtk world -- a meadow, a desert and a
// snowfield -- with Kenney's Pixel Platformer art, walked through from left to
// right to the flag.
//
// What the framework does here, and therefore what is NOT in these files:
//
//   rmp::Tilemap    resources/world.ldtk, loaded by name. The scene draws it,
//                   and the player stands on its IntGrid `Solid` cells, and on
//                   any tile tagged `Solid` in the tileset (the meadow's crate).
//                   No collision code anywhere in the game.
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
//   rmp::audio      the music and the effects, by name. Asking for the track
//                   that is already playing does not restart it, so the music
//                   carries on from one level to the next.
//   rmp::save       the best run, kept between sessions.
//   rmp::global     the run itself -- lives, coins, the clock, what has been
//                   collected -- which has to outlive each level's scene.
//
// What is left is the game: what a coin is worth, what a stomp does, how long
// you are safe after a hit, and when you have won.
//
// Open resources/world.ldtk in LDtk 1.5 to change the levels; the game reads
// whatever it saves. Paint IntGrid `Solid` and the auto-layer rules draw the
// edges and corners; a level's `biome` (Grass, Sand or Snow) picks the terrain.
//
// Run it: `rmp example 07_platformer`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>

#include "game.h"

RMP_GAME(game::TitleScene);
