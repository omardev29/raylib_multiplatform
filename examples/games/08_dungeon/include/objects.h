#pragma once
// ---------------------------------------------------------------------------
// Everything in a level. Nearly all of it is a plain rmp::Object with the
// catalogue's behaviors on it, built from its LDtk entity by a factory in
// src/objects/: what it IS is which behaviors it carries and with which
// fields. The hero has a class of its own, because the sword is the game's.
// ---------------------------------------------------------------------------

#include "game.h"

#include <rmp/object.h>
#include <rmp/tilemap.h>

#include <raylib.h>

#include <string_view>

namespace game {

class Hero : public rmp::Object {
public:
    void _ready() override;
    void _update(float delta) override;

private:
    void swing();
    float _cooldown = 0; // seconds until the sword can swing again
};

// A light in the dark: what the level's darkness lets through, drawn over it
// by add_darkness()'s object. A flame breathes; a gem shines still.
struct Light {
    float radius = 30;
    Color color = { 255, 140, 40, 120 };
    bool flicker = true;
    float clock = 0;
    void _ready(rmp::Object &self);
    void _update(rmp::Object &self, float delta);
};

// The dark over the whole level, and every Light drawn on top of it.
void add_darkness(rmp::Scene &scene);

// One factory per entity type of world.ldtk, registered on the scene's map.
void add_monsters(rmp::Scene &scene, rmp::Handle<rmp::Object> hero);
void add_things(LevelScene &scene, rmp::Handle<rmp::Object> hero);

// Whether this level's Cyclops is still standing.
bool boss_lives();

// A burst of `count` sparks of `color` from `at`: a death, a pickup.
void sparks(rmp::Scene &scene, Vector2 at, Color color, int count);
// A number that rises and fades from `at`: "+1", "-1".
void float_text(rmp::Scene &scene, Vector2 at, std::string_view text, Color color);

// Whether `object` is the hero.
bool is_hero(const rmp::Object &object);

} // namespace game
