#pragma once
// ---------------------------------------------------------------------------
// rmp/tilemap.h — a level designed in Tiled.
//
//     class GameScene : public rmp::Scene {
//         void _ready() override {
//             map = rmp::assets::load_map("level1.json");
//             map.on_object("enemy", [](rmp::Scene &s, const rmp::MapObject &o) {
//                 auto &e = s.spawn<Goblin>({ .position = o.position });
//                 e.hp = o.property_int("hp", 20);
//             });
//             map.spawn_objects(*this);
//         }
//     };
//
// AND THERE IS NO draw(). `map` is a field of rmp::Scene, so the scene draws it
// underneath everything, collides against its solid tiles, and `object.bounds`
// left empty comes to mean THE MAP'S bounds instead of the view's. An empty map
// costs nothing and a menu scene simply never touches it.
//
// That is what turns Tiled from "a parser" into "where you design the level":
// you place the enemies in the editor, give them properties, and they turn up
// in the game.
//
// TILED MUST SAVE THE LAYERS AS CSV. That is the default for a new map, so most
// people never find out; the ones who do find out clearly, because the check is
// explicit and the message names the setting -- Map > Map Properties > Tile
// Layer Format > CSV -- instead of failing somewhere inside the file.
//
// The parser is thirdparty/cute_tiled, and like Clay it appears in no public
// header: it lives behind this class, in src/rmp/tilemap.cpp. Changing parsers
// one day touches nobody's code.
// ---------------------------------------------------------------------------

#include <raylib.h>
#include <rmp/config.h>
#include <rmp/object.h> // rmp::Callback, rmp::Object

namespace rmp {

class Scene;

// ---------------------------------------------------------------------------
// One object from a Tiled object layer.
//
// `const char *` rather than std::string_view for the names and the property
// keys, for the same reason rmp::assets::load_texture takes one: <string_view>
// is 77 ms in every translation unit that includes this header, the strings
// here are NUL-terminated inside the parsed map anyway, and a std::string
// caller writes .c_str() once.
// ---------------------------------------------------------------------------

struct MapObject {
    const char *name = "";
    const char *type = ""; // Tiled's `class`, which it used to call `type`
    Vector2 position{}; // THE CENTRE, like rmp::Object. Tiled gives a corner.
    Vector2 size{};
    float rotation = 0;
    int gid = 0; // 0 when it is not a tile object

    [[nodiscard]] bool property_bool(const char *key, bool fallback = false) const;
    [[nodiscard]] int property_int(const char *key, int fallback = 0) const;
    [[nodiscard]] float property_float(const char *key, float fallback = 0) const;
    [[nodiscard]] const char *property_string(const char *key,
                                              const char *fallback = "") const;

    // The parsed object this came from. Ours; it is what the property lookups
    // read, and it is only public because MapObject has to stay an aggregate.
    const void *raw = nullptr;
};

// ---------------------------------------------------------------------------

class Tilemap {
public:
    Tilemap() = default;
    ~Tilemap();

    // A map is owned by one scene. Copying it would give two scenes that both
    // think they own the same factories.
    Tilemap(const Tilemap &) = delete;
    Tilemap &operator=(const Tilemap &) = delete;
    Tilemap(Tilemap &&other) noexcept;
    Tilemap &operator=(Tilemap &&other) noexcept;

    [[nodiscard]] bool valid() const { return data_ != nullptr; }
    explicit operator bool() const { return valid(); }

    [[nodiscard]] Rectangle bounds() const; // in world units
    [[nodiscard]] Vector2 tile_size() const;
    [[nodiscard]] int layer_count() const;
    [[nodiscard]] int object_count() const;

    // ---- object layers -> objects in the scene -----------------------------
    //
    // A class with no factory registered comes out as a plain rmp::Object with
    // the position, the size, a collider and `solid` already set, which is the
    // right default for a wall or a trigger drawn in the editor.
    //
    // Registering the same class twice REPLACES, because two factories for one
    // class is never what anybody means.
    void on_object(const char *type, Callback<Scene &, const MapObject &> factory);
    void spawn_objects(Scene &into);

    // ---- queries, for whatever you want to do yourself ---------------------
    [[nodiscard]] int tile_at(int layer, int column, int row) const;
    [[nodiscard]] bool solid_at(Vector2 world_position) const;
    [[nodiscard]] bool solid_in(Rectangle world_rect) const;

    // Drawn by the scene, underneath everything. Here because a game that wants
    // the map somewhere else in its own order can call it.
    void draw() const;

    // Ours. Set by rmp::assets::load_map; TAKES OWNERSHIP.
    void adopt(void *data);

private:
    void *data_ = nullptr;
};

} // namespace rmp
