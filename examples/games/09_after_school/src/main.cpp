// ---------------------------------------------------------------------------
// examples/games/09_after_school -- an afternoon in town, told in Yarn.
//
// The last bell has rung, the lantern festival starts at seven, and whoever
// you talk to -- and what you say -- decides how the evening ends: at the
// festival with Mika, in the front row of Sora's concert, or at home.
//
// What the framework does here, and therefore what is NOT in these files:
//
//   rmp::Tilemap     resources/world.ldtk: the town, its walls, every person
//                    in it and the places they walk to, from the editor.
//   rmp::Dialogue    resources/school.yarn: everything anyone says, its
//                    branches and its endings, compiled by the framework.
//                    Its $variables ARE the game's (Dialogue::bind), so
//                    every conversation knows what the others decided.
//   rmp::behavior    the people are Talkers (talk with E or the A button);
//                    you walk with TopDown; the flyer is a Collectible.
//   rmp::ui          the conversations are a DialogueStage, Persona style:
//                    big portraits that slide in, the speaker in front, each
//                    with a voice of their own; Kai's race is a timed choice.
//   rmp::lang        English and Spanish (resources/lang.tsv): every line's
//                    #line: id is its key.
//   rmp::save        the afternoon, so Continue picks it up again.
//
// Move with the arrows or WASD, talk with E (or A), pause with Escape.
// Run it: `rmp example 09_after_school`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>

#include "school.h"

RMP_GAME(game::TitleScene);
