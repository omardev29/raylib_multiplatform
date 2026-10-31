// What is not alive: gems and potions (Collectible), chests, levers, the
// altar and the stairs (Area), runes (Draggable), spikes (Hitbox and Timer),
// the blades round their pillar (Orbit and Hitbox), the gate a lever opens,
// and the braziers that light it all.

#include "objects.h"

#include <rmp/assets.h>
#include <rmp/audio.h>
#include <rmp/behavior.h>
#include <rmp/random.h>

#include <cmath>
#include <raymath.h>

#include <string>
#include <utility>
#include <vector>

namespace game {

namespace {

namespace b = rmp::behavior;

constexpr float PICKUP = 10;
constexpr float SPILL_SPEED = 60;
constexpr float SPIKES_EVERY = 1.1f;
constexpr int POTION_HEALS = 2;
const Color GEM_LIGHT = { 60, 200, 255, 110 };
const Color SHINE = { 255, 214, 102, 255 };
const Color FLAME = { 255, 140, 40, 120 };

Rectangle tile_rect(int index) {
    constexpr int COLUMNS = 12;
    return Rectangle{ static_cast<float>(index % COLUMNS) * TILE,
                      static_cast<float>(index / COLUMNS) * TILE, TILE, TILE };
}

// An object drawn as one tile of tiles.png.
rmp::Object &thing(rmp::Scene &s, Vector2 at, int tile, int draw_layer = 2) {
    auto &t = s.spawn({ .position = at, .layer = draw_layer });
    t.sprite.texture = rmp::assets::load_texture("tiles.png");
    t.sprite.source = tile_rect(tile);
    t.collider = rmp::rect({ PICKUP, PICKUP });
    t.collision_layer = layer::PLACE;
    t.collision_mask = layer::HERO;
    return t;
}


// Slows what was thrown to a stop: a gem out of a chest.
struct Slide {
    float keep = 0.9f; // of its speed, every sixtieth of a second
    void _update(rmp::Object &self, float delta) {
        self.velocity = Vector2Scale(self.velocity, std::pow(keep, delta * 60));
    }
};

// The gates, by their LDtk iid, so that a lever can find the one it opens.
std::vector<std::pair<std::string, rmp::Handle<rmp::Object>>> gates;

void gem(rmp::Scene &s, Vector2 at, Vector2 velocity = {}) {
    auto &g = thing(s, at, tiles::GEM, 3);
    g.velocity = velocity;
    g.collision_layer = layer::PICKUP;
    g.add<b::Collectible>({ .collected_by = layer::HERO,
                            .sound = "coin",
                            .magnet = 26,
                            .on_collect = [](rmp::Object &self, rmp::Object &) {
                                ++run().gems;
                                float_text(self.scene(), self.position, "+1", SHINE);
                            } });
    g.add<Light>({ .radius = 14, .color = GEM_LIGHT, .flicker = false });
    if (velocity.x != 0 || velocity.y != 0) g.add<Slide>();
}

void spill_gems(rmp::Scene &s, Vector2 at, int count) {
    for (int i = 0; i < count; ++i) {
        const float angle = rmp::random::range(0.0f, 6.2831f);
        gem(s, at, Vector2{ std::cos(angle) * SPILL_SPEED, std::sin(angle) * SPILL_SPEED });
    }
}

} // namespace

void add_things(LevelScene &scene, rmp::Handle<rmp::Object> hero) {
    rmp::Tilemap &map = scene.map;
    (void)hero;
    gates.clear();

    map.on_object("Gem", [](rmp::Scene &s, const rmp::MapObject &o) { gem(s, o.position); });

    map.on_object("Potion", [](rmp::Scene &s, const rmp::MapObject &o) {
        auto &p = thing(s, o.position, tiles::POTION, 3);
        p.collision_layer = layer::PICKUP;
        p.add<b::Collectible>({ .collected_by = layer::HERO,
                                .sound = "potion",
                                .on_collect = [](rmp::Object &self, rmp::Object &) {
                                    run().hp = std::min(run().max_hp, run().hp + POTION_HEALS);
                                    float_text(self.scene(), self.position, "+2",
                                               Color{ 255, 110, 110, 255 });
                                } });
        p.add<Light>({ .radius = 12, .color = Color{ 255, 60, 60, 90 } });
    });

    map.on_object("Brazier", [](rmp::Scene &s, const rmp::MapObject &o) {
        auto &fire = thing(s, o.position, tiles::BRAZIER, 1);
        fire.collision_mask = 0;
        fire.add<Light>({ .radius = 70, .color = FLAME });
    });

    map.on_object("Chest", [](rmp::Scene &s, const rmp::MapObject &o) {
        auto &chest = thing(s, o.position, tiles::CHEST);
        const int count = o.property_int("gems", 5);
        chest.add<b::Area>({ .on_enter = [count](rmp::Object &self, rmp::Object &who) {
            if (!is_hero(who) || self.sprite.source.x == tile_rect(tiles::CHEST_OPEN).x) return;
            self.sprite.source = tile_rect(tiles::CHEST_OPEN);
            rmp::audio::play("chest");
            spill_gems(self.scene(), self.position, count);
        } });
    });

    map.on_object("Gate", [](rmp::Scene &s, const rmp::MapObject &o) {
        auto &gate = thing(s, o.position, tiles::GATE);
        gate.collider = rmp::rect({ TILE, TILE });
        gate.solid = true;
        gate.immovable = true;
        gate.collision_layer = layer::WALL;
        gate.collision_mask = layer::HERO | layer::MONSTER;
        gate.layer = 3;
        gates.emplace_back(std::string(o.iid), gate.handle());
    });

    map.on_object("Lever", [&scene](rmp::Scene &s, const rmp::MapObject &o) {
        auto &lever = thing(s, o.position, tiles::LEVER);
        const std::string opens(o.property_string("opens"));
        lever.add<b::Area>({ .on_enter = [&scene, opens](rmp::Object &self, rmp::Object &who) {
            if (!is_hero(who) || self.sprite.source.x == tile_rect(tiles::LEVER_PULLED).x) return;
            self.sprite.source = tile_rect(tiles::LEVER_PULLED);
            rmp::audio::play("door");
            scene.camera.shake(2, 0.4f);
            scene.announce("A gate grinds open");
            for (const auto &[iid, handle] : gates) {
                const auto gate = handle.get();
                if (iid != opens || !gate) continue;
                sparks(scene, gate->position, Color{ 200, 180, 160, 255 }, 16);
                gate->destroy();
            }
        } });
    });

    map.on_object("Spikes", [](rmp::Scene &s, const rmp::MapObject &o) {
        auto &spikes = thing(s, o.position, tiles::SPIKES, 1);
        spikes.collider = rmp::rect({ 12, 12 });
        spikes.collision_layer = layer::TRAP;
        spikes.add(b::Hitbox::from(o));
        // Up and down: a Timer flips the Hitbox on and off, and the tile with it.
        spikes.add<b::Timer>({ .seconds = SPIKES_EVERY, .on_timeout = [](rmp::Object &self) {
            const bool up = !self.enabled<b::Hitbox>();
            self.enable<b::Hitbox>(up);
            self.sprite.tint = up ? WHITE : Color{ 120, 110, 110, 255 };
        } });
    });

    map.on_object("Blades", [](rmp::Scene &s, const rmp::MapObject &o) {
        auto &pillar = thing(s, o.position, tiles::PILLAR, 3);
        pillar.collider = rmp::rect({ TILE, TILE });
        pillar.solid = true;
        pillar.immovable = true;
        pillar.collision_layer = layer::WALL;
        const int count = o.property_int("blades", 3);
        for (int i = 0; i < count; ++i) {
            auto &blade = thing(s, o.position, tiles::BLADE, 4);
            blade.collision_layer = layer::TRAP;
            blade.add(b::Hitbox::from(o));
            b::Orbit orbit = b::Orbit::from(o);
            orbit.point = o.position;
            orbit.face = b::Face::OUTWARD;
            orbit.index = i;
            orbit.of = count;
            blade.add(std::move(orbit));
            blade.add<Light>({ .radius = 9, .color = Color{ 220, 230, 255, 70 } });
        }
    });

    map.on_object("Altar", [&scene](rmp::Scene &s, const rmp::MapObject &o) {
        auto &altar = thing(s, o.position, tiles::ALTAR);
        altar.collider = rmp::rect({ 22, 22 });
        altar.add<b::Area>();
        altar.add<Light>({ .radius = 26, .color = Color{ 255, 230, 140, 70 } });
        (void)scene;
    });

    map.on_object("Rune", [&scene](rmp::Scene &s, const rmp::MapObject &o) {
        auto &rune = thing(s, o.position, tiles::RUNE, 3);
        rune.collider = rmp::rect({ 14, 14 });
        rune.add<Light>({ .radius = 14, .color = Color{ 255, 230, 120, 110 } });
        // Picked up with the pointer and laid on the altar: a heart more.
        rune.add<b::Draggable>({ .drop_on = layer::PLACE,
                                 .on_drop = [&scene](rmp::Object &self, rmp::Object &) {
                                     rmp::audio::play("rune");
                                     ++run().max_hp;
                                     run().hp = run().max_hp;
                                     sparks(scene, self.position, SHINE, 20);
                                     scene.announce("The altar blesses you");
                                     self.destroy();
                                 } });
    });

    map.on_object("Stairs", [&scene](rmp::Scene &s, const rmp::MapObject &o) {
        auto &stairs = thing(s, o.position, tiles::STAIRS, 1);
        const std::string to(o.property_string("to", "Sanctum"));
        stairs.add<b::Area>({ .on_inside = [&scene, to](rmp::Object &, rmp::Object &who) {
            if (is_hero(who)) scene.go_down(to);
        } });
        stairs.add<Light>({ .radius = 22, .color = Color{ 120, 160, 255, 80 } });
    });

    map.on_object("Exit", [&scene](rmp::Scene &s, const rmp::MapObject &o) {
        auto &exit = thing(s, o.position, tiles::STAIRS, 1);
        exit.collider = rmp::rect({ 18, 18 });
        exit.visible = false;
        // Shut until the Cyclops is gone.
        exit.add<b::Area>({ .on_enter = [&scene](rmp::Object &self, rmp::Object &who) {
            if (is_hero(who) && self.visible) scene.win();
        } });
        exit.add<Light>({ .radius = 36, .color = Color{ 255, 220, 120, 130 } });
        exit.enable<Light>(false);
        exit.add<b::Timer>({ .seconds = 0.25f, .on_timeout = [](rmp::Object &self) {
            if (self.visible || boss_lives()) return;
            self.visible = true;
            self.enable<Light>(true);
            sparks(self.scene(), self.position, SHINE, 24);
        } });
    });
}

} // namespace game
