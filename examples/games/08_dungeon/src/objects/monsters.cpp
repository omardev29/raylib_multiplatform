// The monsters. Each is a list of behaviors with the fields LDtk gave it:
//
//   Slime    Chase + Pathfind: it sees you, and finds its way round the walls
//   Bat      Patrol along the Array<Point> drawn in the editor
//   Spider   Patrol + Chase, and a StateMachine that flees when it is hurt
//   Wizard   Shooter, aimed at you, three bolts in a fan, in range
//   Cyclops  a StateMachine: asleep, hunting (Pathfind), a spiral of bolts
//   Ghost    Orbit round the Cyclops
//
// Every one has an Animator, a Health and a Hitbox that hurts on touch.

#include "objects.h"

#include <rmp/assets.h>
#include <rmp/audio.h>
#include <rmp/behavior.h>
#include <rmp/random.h>

#include <raymath.h>

#include <string>
#include <utility>

namespace game {

namespace {

namespace b = rmp::behavior;

constexpr float TOUCH_KNOCKBACK = 240;
constexpr float BOLT_RADIUS = 3;
constexpr float BOLT_LIFE = 2.5f;
constexpr float BOLT_GLOW = 14;
constexpr float BODY = 11; // a monster's collider, square
constexpr float FLEE_SPEED = 70;
constexpr float FLEE_SECONDS = 2;
constexpr float WAKE_DISTANCE = 120;
constexpr float HUNT_SECONDS = 3.5f;
constexpr float VOLLEY_SECONDS = 2.8f;
constexpr float BOSS_SPEED = 38;
const Color BOLT = { 190, 120, 255, 255 };
const Color FIRE = { 255, 150, 60, 255 };
const Color GORE = { 120, 220, 120, 255 };

// The Cyclops the Ghosts circle: built first, because LDtk lists it first.
rmp::Handle<rmp::Object> boss;


float distance(Vector2 a, Vector2 c) { return Vector2Distance(a, c); }

rmp::Object &monster(rmp::Scene &s, const rmp::MapObject &o, std::string_view sheet, int hp) {
    auto &m = s.spawn({ .position = o.position, .layer = 4 });
    m.sprite.sheet = rmp::assets::load_sheet(sheet);
    m.collider = rmp::rect({ BODY, BODY });
    m.solid = true;
    m.collision_layer = layer::MONSTER;
    m.collision_mask = layer::HERO | layer::SWING | layer::MONSTER | layer::WALL;
    m.add<b::Hitbox>({ .damage = 1, .hits = layer::HERO, .knockback = TOUCH_KNOCKBACK });
    m.add<b::Health>({ .hp = hp,
                       .invulnerable_for = 0.25f,
                       .on_death = [](rmp::Object &self) {
                           rmp::audio::play("hit", { .pitch = 0.7f });
                           sparks(self.scene(), self.position, GORE, 14);
                       } });
    m.add<b::Animator>();
    return m;
}

// A bolt of the wizard's or the cyclops's: a violet spark that hurts the hero
// and ends at a wall.
void bolt(rmp::Scene &s, Vector2 at, Vector2 velocity) {
    auto &shot = s.spawn({ .position = at, .shape = rmp::circle(BOLT_RADIUS),
                           .velocity = velocity, .layer = 7 });
    shot.shape.color = BOLT;
    shot.collision_layer = layer::TRAP;
    shot.collision_mask = layer::HERO;
    shot.add<b::Hitbox>({ .damage = 1, .hits = layer::HERO, .knockback = 160,
                          .destroy_on_hit = true });
    shot.add<b::Projectile>({ .destroy_on_wall = true });
    shot.add<b::Lifespan>({ .seconds = BOLT_LIFE });
    shot.add<Light>({ .radius = BOLT_GLOW, .color = Color{ 150, 80, 255, 170 }, .flicker = false });
}

} // namespace

bool boss_lives() { return static_cast<bool>(boss); }

void add_monsters(rmp::Scene &scene, rmp::Handle<rmp::Object> hero) {
    rmp::Tilemap &map = scene.map;
    boss = {};

    map.on_object("Slime", [hero](rmp::Scene &s, const rmp::MapObject &o) {
        auto &slime = monster(s, o, "slime.aseprite", 2);
        b::Chase chase = b::Chase::from(o);
        chase.target = hero;
        slime.add<b::Pathfind>({ .clearance = 4 });
        slime.add(std::move(chase));
    });

    map.on_object("Bat", [](rmp::Scene &s, const rmp::MapObject &o) {
        auto &bat = monster(s, o, "bat.aseprite", 1);
        bat.solid = false; // it flies
        bat.add(b::Patrol::from(o));
    });

    map.on_object("Spider", [hero](rmp::Scene &s, const rmp::MapObject &o) {
        auto &spider = monster(s, o, "spider.aseprite", 3);
        b::Chase chase = b::Chase::from(o);
        chase.target = hero;
        spider.add(b::Patrol::from(o));
        spider.add(std::move(chase));
        // Hurt down to its last point, it runs from you for a while.
        b::StateMachine mind;
        mind.states.push_back({ .name = "hunt", .enables = b::kinds<b::Patrol, b::Chase>() });
        mind.states.push_back(
            { .name = "flee",
              .on_update = [hero](rmp::Object &self, float) {
                  if (const auto h = hero.get()) {
                      const Vector2 away = Vector2Normalize(Vector2Subtract(self.position, h->position));
                      self.velocity = Vector2Scale(away, FLEE_SPEED);
                  }
              } });
        mind.transitions.push_back({ .from = "hunt", .to = "flee", .when = [](rmp::Object &self) {
                                         return self.get<b::Health>()->hp == 1;
                                     } });
        mind.transitions.push_back({ .from = "flee", .to = "hunt", .when = [](rmp::Object &self) {
                                         return self.get<b::StateMachine>()->seconds_in_state() >
                                             FLEE_SECONDS;
                                     } });
        spider.add(std::move(mind));
    });

    map.on_object("Wizard", [hero](rmp::Scene &s, const rmp::MapObject &o) {
        auto &wizard = monster(s, o, "wizard.aseprite", 2);
        wizard.solid = false;
        wizard.immovable = true;
        b::Shooter staff = b::Shooter::from(o);
        staff.automatic = true;
        staff.target = hero;
        staff.on_shoot = [](rmp::Scene &scene, Vector2 at, Vector2 velocity) {
            bolt(scene, at, velocity);
        };
        wizard.add(std::move(staff));
        wizard.add<Light>({ .radius = 22, .color = Color{ 90, 40, 160, 90 } });
    });

    map.on_object("Cyclops", [hero](rmp::Scene &s, const rmp::MapObject &o) {
        auto &cyclops = monster(s, o, "cyclops.aseprite", o.property_int("hp", 14));
        cyclops.scale = { 2, 2 };
        cyclops.collider = rmp::rect({ BODY * 1.4f, BODY * 1.4f });
        boss = cyclops.handle();
        cyclops.get<b::Health>()->on_death = [](rmp::Object &self) {
            sparks(self.scene(), self.position, FIRE, 40);
            self.scene().camera.shake(6, 0.6f);
            static_cast<LevelScene &>(self.scene()).announce("The Sanctum falls silent");
        };
        cyclops.add<b::Pathfind>({ .target = hero, .speed = BOSS_SPEED, .clearance = 6 });
        cyclops.add<b::Shooter>({ .every = 0.3f, .speed = 90, .count = 6, .spread = 360,
                                  .rotate_per_shot = 17,
                                  .on_shoot = [](rmp::Scene &scene, Vector2 at, Vector2 v) {
                                      bolt(scene, at, v);
                                  } });
        // Asleep until you come near; then it hunts you, and stops to throw
        // a spiral of bolts, and hunts again.
        b::StateMachine mind;
        mind.states.push_back({ .name = "asleep" });
        mind.states.push_back({ .name = "hunt", .enables = b::kinds<b::Pathfind>() });
        mind.states.push_back({ .name = "volley",
                                .on_enter = [](rmp::Object &self) { self.velocity = {}; },
                                .enables = b::kinds<b::Shooter>() });
        mind.transitions.push_back({ .from = "asleep", .to = "hunt", .when = [hero](rmp::Object &self) {
                                         const auto h = hero.get();
                                         return h && distance(h->position, self.position) < WAKE_DISTANCE;
                                     } });
        mind.transitions.push_back({ .from = "hunt", .to = "volley", .when = [](rmp::Object &self) {
                                         return self.get<b::StateMachine>()->seconds_in_state() >
                                             HUNT_SECONDS;
                                     } });
        mind.transitions.push_back({ .from = "volley", .to = "hunt", .when = [](rmp::Object &self) {
                                         return self.get<b::StateMachine>()->seconds_in_state() >
                                             VOLLEY_SECONDS;
                                     } });
        cyclops.add(std::move(mind));
        cyclops.get<b::Shooter>()->automatic = true;
        cyclops.add<Light>({ .radius = 40, .color = Color{ 160, 40, 30, 90 } });
    });

    map.on_object("Ghost", [](rmp::Scene &s, const rmp::MapObject &o) {
        auto &ghost = monster(s, o, "ghost.aseprite", 1);
        ghost.solid = false;
        b::Orbit orbit = b::Orbit::from(o);
        orbit.centre = boss;
        orbit.point = o.position;
        ghost.add(std::move(orbit));
        ghost.sprite.tint = Color{ 255, 255, 255, 200 };
    });
}

} // namespace game
