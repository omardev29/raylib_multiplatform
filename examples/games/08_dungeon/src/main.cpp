// ---------------------------------------------------------------------------
// examples/games/08_dungeon -- a dungeon crawler whose levels are made in LDtk,
// and whose monsters are made of the behavior catalogue.
//
// Two levels of one LDtk world, the Crypt and the Sanctum, in Kenney's Tiny
// Dungeon art, under his Ancient frames and his pixel font.
//
// What the framework does here, and therefore what is NOT in these files:
//
//   rmp::Tilemap    resources/world.ldtk: the walls are its IntGrid `Solid`,
//                   drawn only where the camera looks; every monster, gem,
//                   trap, chest, lever, rune and stair is an entity with
//                   fields, built by a factory in src/objects/.
//   the behaviors   the whole cast is the catalogue with LDtk's numbers in
//                   it (B::from(MapObject)): a Slime is a Chase that finds its
//                   way with Pathfind; a Bat a Patrol along a path drawn in
//                   the editor; a Spider a Patrol, a Chase and a StateMachine
//                   that flees when it is hurt; a Wizard a Shooter; the
//                   Cyclops a StateMachine of sleep, hunt and a spiral of
//                   bolts, with Ghosts in Orbit round it. Gems and potions are
//                   Collectibles with a magnet; chests, levers, the altar and
//                   the stairs are Areas; spikes a Hitbox a Timer turns on and
//                   off (enable<B>); the blades an Orbit and a Hitbox; a rune a
//                   Draggable you lay on the altar with the pointer. The hero
//                   is TopDown, Animator and Health, and its sword a Hitbox.
//   rmp::Camera     follows, stays inside the level, shakes on a hit.
//   rmp::ui         the menus, the HUD and its health bar, all in one Theme
//                   of nine-slices and a pixel font (src/look.cpp).
//   rmp::audio      every sound, by name.
//   rmp::save       the most gems ever carried out.
//
// What is left is the game: what a gem is worth, what a potion heals, when
// the gate opens and when you have won.
//
// Move with the arrows or WASD, strike with Space or J (the A button), pause
// with Escape. Drag a rune onto the altar for another heart.
//
// Open resources/world.ldtk in LDtk 1.5 to change the levels.
// Run it: `rmp example 08_dungeon`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>

#include "dungeon.h"

RMP_GAME(game::TitleScene);
