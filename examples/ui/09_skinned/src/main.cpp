// ---------------------------------------------------------------------------
// examples/ui/09_skinned/src/main.cpp
//
// A game's shop screen, skinned: the other way to write rmp::ui.
//
// The examples before this one write the interface a call at a time, and a
// main menu is three of them. This one is what a screen DESIGNED by an artist
// looks like: every button and panel a picture from one Aseprite file, every
// icon a named slice of it, a title in a font of its own with an outline, a
// soft shadow under the shop, sounds on hover and click, buttons that grow
// under the pointer and shrink when pressed, a panel that slides in, a button
// that shakes when there is not enough money -- and widgets you KEEP, members
// of the scene, each with its look and its callbacks, the way a Godot scene
// keeps its nodes.
//
// Where each piece is:
//
//   the look of everything    ShopScene::_ready(): one Theme, with a Style for
//                             every button and every panel, sounds and motion
//   the widgets               src/scenes/shop.h: kept, with their callbacks
//   the screen                ShopScene::_draw(): where each one is shown
//
// The art is generated, as the art of every example is: tools/make_skinned_art.py.
//
// Run it: `rmp example 09_skinned`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>

#include "scenes/shop.h"

RMP_GAME(ShopScene);
