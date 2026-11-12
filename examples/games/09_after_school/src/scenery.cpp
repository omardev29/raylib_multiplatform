// The town as every scene shows it: the map, and the trees and street lamps
// standing in it, which are entities so that they can be drawn over whoever
// walks behind them.

#include "school.h"

#include <rmp/assets.h>
#include <rmp/tilemap.h>

#include <array>
#include <string_view>

namespace game {

namespace {

// The two tiles of each in maps/tiles.png, top-left first.
constexpr Rectangle GREEN_TREE{ 16 * TILE, 8 * TILE, TILE, 2 * TILE };
constexpr Rectangle ORANGE_TREE{ 16 * TILE, 11 * TILE, TILE, 2 * TILE };
constexpr Rectangle STREET_LAMP{ 7 * TILE, 6 * TILE, TILE, 2 * TILE };

// Who and what else the map holds: the town scene's, and nobody's elsewhere.
constexpr std::array<std::string_view, 5> CAST{ "Player", "Person", "Home", "Place",
                                                "Flyer" };

// Two tiles standing on the lower one, in front of whoever is further up.
void stand(rmp::Scene &scene, const rmp::MapObject &o, Rectangle tiles) {
    auto &standing = scene.spawn({ .position = o.position });
    standing.sprite.texture = rmp::assets::load_texture("maps/tiles.png");
    standing.sprite.source = tiles;
    standing.sprite.origin = { 0.5f, 0.75f };
    standing.layer = static_cast<int>(o.position.y);
    // The map's cell under it is what stops you; the picture touches nothing.
    standing.collision_layer = 0;
    standing.collision_mask = 0;
}

} // namespace

void load_town(rmp::Scene &scene) {
    scene.map = rmp::assets::load_map("maps/world.ldtk", "Town");
    scene.map.on_object("Tree", [](rmp::Scene &into, const rmp::MapObject &o) {
        stand(into, o, o.property_string("kind") == "green" ? GREEN_TREE : ORANGE_TREE);
    });
    scene.map.on_object("Lamp", [](rmp::Scene &into, const rmp::MapObject &o) {
        stand(into, o, STREET_LAMP);
    });
    for (const std::string_view name : CAST) {
        scene.map.on_object(name, [](rmp::Scene &, const rmp::MapObject &) {});
    }
}

} // namespace game
