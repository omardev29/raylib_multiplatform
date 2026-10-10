#pragma once
// ---------------------------------------------------------------------------
// rmp/tilemap.h — a level designed in LDtk (or in Tiled).
//
//     class GameScene : public rmp::Scene {
//         void _ready() override {
//             map = rmp::assets::load_map("world.ldtk");
//             map.on_object("enemy", [](rmp::Scene &s, const rmp::MapObject &o) {
//                 auto &e = s.spawn<Goblin>({ .position = o.position });
//                 e.hp = o.property_int("hp", 20);
//             });
//             map.spawn_objects(*this);
//         }
//     };
//
// AND THERE IS NO draw(). `map` is a field of rmp::Scene, so the scene draws it
// underneath everything, a solid object that moves is stopped by its solid
// cells (the floor holds the player up, a wall stops it, and Platformer counts
// the floor as ground), and `object.bounds` left empty comes to mean THE MAP'S
// bounds instead of the view's. An empty map
// costs nothing and a menu scene simply never touches it.
//
// That is what turns the editor from "a file format" into "where you design the
// level": you place the enemies in it, give them fields, and they turn up in
// the game.
//
// TWO EDITORS, ONE MAP, AND ONLY ONE OF THEM KEEPS MOVING. load_map() picks the
// reader by extension:
//
//   .ldtk  LDtk, the one this framework TESTS AND MAINTAINS. It is the more
//          modern editor -- auto-layers, IntGrid, entities with typed fields,
//          worlds of many levels, a documented JSON format, stable since 1.5 --
//          and keeping two readers at the same level of testing is more than
//          it is worth. src/rmp/ldtk.cpp, on cJSON.
//   .json  Tiled, through thirdparty/cute_tiled. It stays wired up for whoever
//          uses it, with the tests it has, and that is all: it gets nothing new
//          and no guarantees. Its layers must be saved as CSV, and the message
//          says where that setting is when they are not.
//
// Neither parser appears in this header. Whatever they read lands in one map,
// and nothing after parsing knows which editor made it.
//
// LDTK, IN ONE PARAGRAPH. A level is a map: load_map("world.ldtk") loads the
// first, load_map("world.ldtk", "Level_2") that one. Levels sit in WORLD
// coordinates, so bounds() is where the level is in the world and every
// position -- objects, solids, the camera -- is in those same units; that is
// what makes walking into the next level a matter of neighbour_at(). Solid is an
// IntGrid value whose identifier is `solid`, or a tile tagged `solid` with the
// tileset's enum. Entities are MapObjects, their fields are properties.
// ---------------------------------------------------------------------------

#include <raylib.h>
#include <rmp/config.h>
#include <rmp/object.h> // rmp::Callback, rmp::Object, rmp::Ref

#include <memory> // std::unique_ptr: the parsed map, behind a forward declaration
#include <string_view> // names and property keys

namespace rmp {

namespace tilemap::detail {
// The parsed map. A forward declaration so that the parsers and their vectors
// stay out of this header; src/rmp/tilemap_internal.h defines it. The owning pointer
// carries its deleter as a plain function, so a translation unit can hold and
// drop one without ever seeing the complete type.
struct MapData;
void free_map(MapData *map);
using MapPtr = std::unique_ptr<MapData, void (*)(MapData *)>;
// One parsed object of the map: its strings and its properties.
struct ObjectInfo;
// How the loader hands a Tilemap what it parsed, and how the engine and the
// tests read it back. src/rmp/tilemap_internal.h defines it.
struct Access;
} // namespace tilemap::detail

class Scene;

// ---------------------------------------------------------------------------
// One object of the map: an LDtk entity, or an object from a Tiled object layer.
//
// Its names, and the text a property holds, are views of the parsed map: valid
// while the map is loaded, which is all of spawn_objects() -- keep a
// std::string of one to hold it longer. Each one is followed by a NUL, so what
// the map wrote reaches raylib unchanged.
// ---------------------------------------------------------------------------

// One object placed in the map, as spawn_objects() hands it to the factory
// registered for its `type`: where it is, how big, and its properties.
struct MapObject {
    std::string_view name; // the Tiled object's name; "" for an LDtk entity
    std::string_view type; // the LDtk entity; Tiled's `class`
    std::string_view iid; // LDtk's unique id, what an EntityRef field holds
    Vector2 position{}; // THE CENTRE, like rmp::Object; the editors give a corner
    Vector2 size{}; // width and height in world units; {0,0} for a Tiled point
    float rotation = 0; // Tiled's, in degrees clockwise; 0 for an LDtk entity
    int gid = 0; // 0 when it is not a tile object

    // A property by name -- an LDtk field, a Tiled custom property -- or
    // `fallback` when the object has none by that name or it holds another
    // kind. property_int() also reads a float, cut towards zero, and
    // property_float() also reads an int.
    [[nodiscard]] bool property_bool(std::string_view key, bool fallback = false) const;
    [[nodiscard]] int property_int(std::string_view key, int fallback = 0) const;
    [[nodiscard]] float property_float(std::string_view key, float fallback = 0) const;
    // The map's text, or `fallback` as it was given: its lifetime is yours.
    [[nodiscard]] std::string_view property_string(std::string_view key,
                                                   std::string_view fallback = "") const;

    // An LDtk Point field, as the WORLD position of the centre of the cell it
    // names -- the same units as `position`, so a patrol route or a platform's
    // destination is used as it comes. A list of them (Array<Point>) is read
    // by index; property_count() is how many there are, and 1 for any other
    // field that exists. Enums read as text, colours as "#rrggbb", and an
    // EntityRef as the iid of the entity it points at.
    [[nodiscard]] Vector2 property_point(std::string_view key,
                                         Vector2 fallback = {}) const;
    [[nodiscard]] Vector2 property_point(std::string_view key, int index,
                                         Vector2 fallback = {}) const;
    [[nodiscard]] int property_count(std::string_view key) const;

private:
    // Made by the map, from what it parsed, and by nothing else.
    friend struct tilemap::detail::MapData;
    friend class Tilemap; // spawn_objects() asks the parsed object about its shape
    explicit MapObject(const tilemap::detail::ObjectInfo &parsed);
    [[nodiscard]] Ref<const tilemap::detail::ObjectInfo> info() const { return _info; }

    // The parsed object this came from: what the property lookups read.
    Ref<const tilemap::detail::ObjectInfo> _info;
};

// ---------------------------------------------------------------------------

// A level read from an LDtk or a Tiled file: its tile layers, which cells are
// solid, and the objects placed in it. Every Scene has one in `map`, which
// rmp::assets::load_map() fills; it moves and does not copy.
class Tilemap {
public:
    // Declared here and defined in tilemap.cpp, all of them: MapData is
    // incomplete in this header, and even a defaulted constructor written
    // here would instantiate the unique_ptr's deleter against it.
    Tilemap();
    ~Tilemap();

    // A map is owned by one scene. Copying it would give two scenes that both
    // think they own the same factories.
    Tilemap(const Tilemap &) = delete;
    Tilemap &operator=(const Tilemap &) = delete;
    Tilemap(Tilemap &&other) noexcept;
    Tilemap &operator=(Tilemap &&other) noexcept;

    // Whether a map is loaded: false for an empty one, and for one whose file
    // was missing or did not parse. Every query below then answers zero, empty
    // or false, and on_object() registers nothing.
    [[nodiscard]] bool valid() const { return _data != nullptr; }
    explicit operator bool() const { return valid(); }

    [[nodiscard]] Rectangle bounds() const; // in world units
    [[nodiscard]] Vector2 tile_size() const; // one cell, in world units
    [[nodiscard]] int layer_count() const; // how many tile layers; 0 is the bottom one
    [[nodiscard]] int object_count() const; // how many MapObjects it holds

    // ---- the levels of an LDtk world ---------------------------------------
    //
    // The level this map is, and the OTHER level of its world at a point --
    // "" while the point is still in this one, or in none. So walking into
    // the next level is
    //
    //     const std::string_view next = map.neighbour_at(player.position);
    //     if (!next.empty()) rmp::Scene::change<Level>(std::string(next), player.position);
    //
    // and the new scene loads that level and puts the player back where it
    // was: the coordinates are the world's, so they still mean the same place.
    // Tiled maps and LDtk's linear layouts have no world, and always say "".
    // Views of the parsed map, each followed by a NUL: valid while it is loaded.
    [[nodiscard]] std::string_view level() const;
    [[nodiscard]] std::string_view neighbour_at(Vector2 world_position) const;

    // ---- object layers -> objects in the scene -----------------------------
    //
    // A type with no factory registered comes out as a plain rmp::Object with
    // the position, the size and a collider -- and `solid` already set WHEN
    // THE EDITOR GAVE IT AN AREA, which is the right default for a wall or a
    // platform drawn in the editor. A point, and anything else whose width or
    // height is zero, is a marker rather than a shape and comes out non-solid:
    // an invisible collider at a spawn point is not what anybody drew.
    //
    // Registering the same class twice REPLACES, because two factories for one
    // class is never what anybody means.
    void on_object(std::string_view type, Callback<Scene &, const MapObject &> factory);
    void spawn_objects(Scene &into);

    // ---- queries, for whatever you want to do yourself ---------------------
    // The tile in one cell of one layer, as its global tile id: 0 when the
    // cell is empty, outside the layer, or there is no map. Layers count from
    // 0 in the order the file has them, and the cell is in that layer's own
    // grid. For LDtk it is the topmost tile of the cell.
    [[nodiscard]] int tile_at(int layer, int column, int row) const;
    // Whether a solid cell is under a point, in world units, on any layer: an
    // LDtk IntGrid cell painted with `solid`, any tile of a Tiled layer whose
    // class is `solid`, or a tile its tileset marks solid. False with no map.
    [[nodiscard]] bool solid_at(Vector2 world_position) const;
    // Whether a solid cell covers any of the rectangle. Only touching it is
    // not: a box standing on the floor is not in the floor.
    [[nodiscard]] bool solid_in(Rectangle world_rect) const;

    // Drawn by the scene, underneath everything. Here because a game that wants
    // the map somewhere else in its own order can call it.
    void draw() const;

private:
    // rmp::assets::load_map hands over what it parsed, through
    // tilemap::detail::Access; null empties the map. And the parsed map
    // itself, for the detail entry points that take it rather than the class.
    friend struct tilemap::detail::Access;
    void adopt(tilemap::detail::MapPtr data);
    [[nodiscard]] const tilemap::detail::MapData *data() const { return _data.get(); }

    tilemap::detail::MapPtr _data{ nullptr, &tilemap::detail::free_map };
};

} // namespace rmp
