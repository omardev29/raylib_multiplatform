// ---------------------------------------------------------------------------
// Tiled maps: parsing, drawing, solid tiles, and object layers into objects.
//
// The public surface is include/rmp/tilemap.h and cute_tiled appears in none of
// it -- same arrangement as Clay behind rmp::ui, and for the same reason:
// changing parser one day should touch nobody's code.
//
// WHICH TILE IS SOLID. Two conventions, both of them Tiled's own and neither
// invented here:
//
//   a LAYER whose class is `solid`  -> every non-empty tile in it is solid.
//     The "I draw the level, and separately I paint where you cannot walk" way.
//   a TILE with a boolean `solid` property in the tileset.
//     The "this tile is a wall, always" way.
//
// Both are used in practice and supporting both is ten lines, so both.
//
// TILESET IMAGES. Tiled stores a path relative to the .tmx; we keep the FILE
// NAME and put it through rmp::assets::load_texture, so it arrives the same way
// every other asset does -- out of resources.rres in a release and out of
// resources/ while developing, without the map knowing which. The practical
// consequence, and it is the one people trip over: the tileset's PNG has to be
// in resources/, and if it is not, the warning says the name it looked for.
// ---------------------------------------------------------------------------

#include <rmp/tilemap.h>

#include <rmp/assets.h>
#include <rmp/scene.h>

#include "tilemap_internal.h"

#include <cute_tiled.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace rmp {

namespace {

constexpr unsigned kFlipMask = 0xE0000000u; // Tiled's three flip bits

struct Tileset {
    int first_gid = 0;
    int last_gid = 0;
    int columns = 0;
    int tile_width = 0;
    int tile_height = 0;
    rmp::Texture texture;
    std::vector<bool> solid; // by local tile id
};

struct Layer {
    std::string name;
    bool solid_layer = false;
    int width = 0;
    int height = 0;
    std::vector<int> gids;
};

} // namespace

// The parsed map. Behind a void* in the public header so that cute_tiled and
// <vector> stay out of it.
struct MapData {
    cute_tiled_map_t *raw = nullptr;
    int width = 0;
    int height = 0;
    int tile_width = 0;
    int tile_height = 0;
    std::vector<Layer> layers;
    std::vector<Tileset> tilesets;
    std::vector<MapObject> objects;
    std::vector<std::pair<std::string, Callback<Scene &, const MapObject &>>> factories;
};

namespace {

MapData *as_data(void *p) { return static_cast<MapData *>(p); }
const MapData *as_data(const void *p) { return static_cast<const MapData *>(p); }

// Tiled writes a path relative to the map; the asset layer works in names.
const char *file_name_of(const char *path) {
    if (path == nullptr) return "";
    const char *last = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

const cute_tiled_property_t *find_property(const void *raw_object, const char *key) {
    if (raw_object == nullptr || key == nullptr) return nullptr;
    const auto *object = static_cast<const cute_tiled_object_t *>(raw_object);
    for (int i = 0; i < object->property_count; i++) {
        const cute_tiled_property_t &p = object->properties[i];
        if (p.name.ptr != nullptr && std::strcmp(p.name.ptr, key) == 0) return &p;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// MapObject
// ---------------------------------------------------------------------------

bool MapObject::property_bool(const char *key, bool fallback) const {
    const cute_tiled_property_t *p = find_property(raw, key);
    if (p == nullptr || p->type != CUTE_TILED_PROPERTY_BOOL) return fallback;
    return p->data.boolean != 0;
}

int MapObject::property_int(const char *key, int fallback) const {
    const cute_tiled_property_t *p = find_property(raw, key);
    if (p == nullptr) return fallback;
    if (p->type == CUTE_TILED_PROPERTY_INT) return p->data.integer;
    // A float where an int was asked for is a rounding, not a failure: Tiled
    // will happily save 3 as 3.0 and nobody means anything by it.
    if (p->type == CUTE_TILED_PROPERTY_FLOAT) {
        return static_cast<int>(p->data.floating);
    }
    return fallback;
}

float MapObject::property_float(const char *key, float fallback) const {
    const cute_tiled_property_t *p = find_property(raw, key);
    if (p == nullptr) return fallback;
    if (p->type == CUTE_TILED_PROPERTY_FLOAT) return p->data.floating;
    if (p->type == CUTE_TILED_PROPERTY_INT) {
        return static_cast<float>(p->data.integer);
    }
    return fallback;
}

const char *MapObject::property_string(const char *key, const char *fallback) const {
    const cute_tiled_property_t *p = find_property(raw, key);
    if (p == nullptr) return fallback;
    if (p->type != CUTE_TILED_PROPERTY_STRING && p->type != CUTE_TILED_PROPERTY_FILE) {
        return fallback;
    }
    return p->data.string.ptr != nullptr ? p->data.string.ptr : fallback;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

namespace tilemap::detail {

void *parse_map(const void *bytes, int size, const char *name) {
    if (bytes == nullptr || size <= 0) return nullptr;

    cute_tiled_map_t *raw = cute_tiled_load_map_from_memory(bytes, size, nullptr);
    if (raw == nullptr) {
        const char *why =
            cute_tiled_error_reason != nullptr ? cute_tiled_error_reason : "unknown";
        // The compression case gets its own message, because the parser's --
        // "Compression is not yet supported" -- does not say what to DO, and
        // what to do is one setting in the editor.
        if (std::strstr(why, "Compression") != nullptr ||
            std::strstr(why, "compression") != nullptr) {
            TraceLog(LOG_WARNING,
                     "MAP: [%s] has its tile layers saved compressed or as base64, "
                     "and this reads CSV.",
                     name != nullptr ? name : "");
            TraceLog(LOG_WARNING,
                     "MAP: in Tiled: Map > Map Properties > Tile Layer Format > CSV, "
                     "then save again. CSV is the default for a new map.");
        } else {
            TraceLog(LOG_WARNING, "MAP: [%s] could not be read: %s",
                     name != nullptr ? name : "", why);
        }
        return nullptr;
    }

    auto *data = new MapData{};
    data->raw = raw;
    data->width = raw->width;
    data->height = raw->height;
    data->tile_width = raw->tilewidth;
    data->tile_height = raw->tileheight;

    for (cute_tiled_tileset_t *set = raw->tilesets; set != nullptr; set = set->next) {
        Tileset out;
        out.first_gid = set->firstgid;
        out.last_gid = set->firstgid + set->tilecount - 1;
        out.columns = set->columns > 0 ? set->columns : 1;
        out.tile_width = set->tilewidth;
        out.tile_height = set->tileheight;
        out.solid.assign(
            static_cast<std::size_t>(set->tilecount > 0 ? set->tilecount : 0), false);

        for (cute_tiled_tile_descriptor_t *tile = set->tiles; tile != nullptr;
             tile = tile->next) {
            for (int i = 0; i < tile->property_count; i++) {
                const cute_tiled_property_t &p = tile->properties[i];
                if (p.name.ptr == nullptr) continue;
                if (std::strcmp(p.name.ptr, "solid") != 0) continue;
                if (p.type != CUTE_TILED_PROPERTY_BOOL) continue;
                const auto id = static_cast<std::size_t>(tile->tile_index);
                if (id < out.solid.size()) out.solid[id] = p.data.boolean != 0;
            }
        }

        const char *image = file_name_of(set->image.ptr);
        if (image[0] != '\0') {
            out.texture = rmp::assets::load_texture(image);
            if (!out.texture.valid()) {
                TraceLog(LOG_WARNING,
                         "MAP: the tileset image \"%s\" is not in resources/. The map "
                         "loads and draws nothing for that tileset.",
                         image);
            }
        }
        data->tilesets.push_back(std::move(out));
    }

    for (cute_tiled_layer_t *layer = raw->layers; layer != nullptr; layer = layer->next) {
        const char *type = layer->type.ptr != nullptr ? layer->type.ptr : "";
        const char *klass = layer->class_.ptr != nullptr ? layer->class_.ptr : "";

        if (std::strcmp(type, "objectgroup") == 0) {
            // REVERSED on the way in. cute_tiled builds its object list by
            // prepending, so it hands them back last-first -- and "the first
            // object in the layer" is a thing a level designer means, so the
            // order a game sees has to be the order in the file. The layers
            // themselves come out in file order already; only this list does
            // not, which is exactly the sort of asymmetry a test finds and
            // reading does not.
            const std::size_t before = data->objects.size();
            for (cute_tiled_object_t *object = layer->objects; object != nullptr;
                 object = object->next) {
                MapObject out;
                out.name = object->name.ptr != nullptr ? object->name.ptr : "";
                out.type = object->type.ptr != nullptr ? object->type.ptr : "";
                out.size = Vector2{ object->width, object->height };
                // TILED GIVES THE CORNER and rmp::Object's position is the
                // CENTRE. Converting here rather than at every call site is
                // most of what this struct is for.
                out.position = Vector2{ object->x + object->width / 2,
                                        object->y + object->height / 2 };
                out.rotation = object->rotation;
                out.gid = object->gid;
                out.raw = object;
                data->objects.push_back(out);
            }
            std::reverse(data->objects.begin() + static_cast<std::ptrdiff_t>(before),
                         data->objects.end());
            continue;
        }

        if (std::strcmp(type, "tilelayer") != 0) continue;

        Layer out;
        out.name = layer->name.ptr != nullptr ? layer->name.ptr : "";
        out.solid_layer = std::strcmp(klass, "solid") == 0;
        out.width = layer->width;
        out.height = layer->height;
        out.gids.reserve(static_cast<std::size_t>(layer->data_count));
        for (int i = 0; i < layer->data_count; i++) {
            // The top three bits are Tiled's flip flags, not part of the id.
            out.gids.push_back(
                static_cast<int>(static_cast<unsigned>(layer->data[i]) & ~kFlipMask));
        }
        data->layers.push_back(std::move(out));
    }

    return data;
}

void free_map(void *p) {
    if (p == nullptr) return;
    MapData *data = as_data(p);
    if (data->raw != nullptr) cute_tiled_free_map(data->raw);
    delete data;
}

int object_count(const void *p) {
    return p == nullptr ? 0 : static_cast<int>(as_data(p)->objects.size());
}

const MapObject *object_at(const void *p, int index) {
    if (p == nullptr) return nullptr;
    const MapData *data = as_data(p);
    if (index < 0 || static_cast<std::size_t>(index) >= data->objects.size()) {
        return nullptr;
    }
    return &data->objects[static_cast<std::size_t>(index)];
}

} // namespace tilemap::detail

// ---------------------------------------------------------------------------
// Tilemap
// ---------------------------------------------------------------------------

Tilemap::~Tilemap() { tilemap::detail::free_map(data_); }

Tilemap::Tilemap(Tilemap &&other) noexcept : data_(other.data_) { other.data_ = nullptr; }

Tilemap &Tilemap::operator=(Tilemap &&other) noexcept {
    if (this != &other) {
        tilemap::detail::free_map(data_);
        data_ = other.data_;
        other.data_ = nullptr;
    }
    return *this;
}

void Tilemap::adopt(void *data) {
    tilemap::detail::free_map(data_);
    data_ = data;
}

Rectangle Tilemap::bounds() const {
    if (!valid()) return Rectangle{};
    const MapData *data = as_data(data_);
    return Rectangle{ 0, 0, static_cast<float>(data->width * data->tile_width),
                      static_cast<float>(data->height * data->tile_height) };
}

Vector2 Tilemap::tile_size() const {
    if (!valid()) return Vector2{};
    const MapData *data = as_data(data_);
    return Vector2{ static_cast<float>(data->tile_width),
                    static_cast<float>(data->tile_height) };
}

int Tilemap::layer_count() const {
    return valid() ? static_cast<int>(as_data(data_)->layers.size()) : 0;
}

int Tilemap::object_count() const { return tilemap::detail::object_count(data_); }

int Tilemap::tile_at(int layer_index, int column, int row) const {
    if (!valid()) return 0;
    const MapData *data = as_data(data_);
    if (layer_index < 0 || static_cast<std::size_t>(layer_index) >= data->layers.size()) {
        return 0;
    }
    const Layer &layer = data->layers[static_cast<std::size_t>(layer_index)];
    if (column < 0 || row < 0 || column >= layer.width || row >= layer.height) return 0;
    const auto at =
        static_cast<std::size_t>(row) * static_cast<std::size_t>(layer.width) +
        static_cast<std::size_t>(column);
    return at < layer.gids.size() ? layer.gids[at] : 0;
}

bool Tilemap::solid_at(Vector2 world_position) const {
    if (!valid()) return false;
    const MapData *data = as_data(data_);
    if (data->tile_width <= 0 || data->tile_height <= 0) return false;
    if (world_position.x < 0 || world_position.y < 0) return false;

    const int column = static_cast<int>(world_position.x) / data->tile_width;
    const int row = static_cast<int>(world_position.y) / data->tile_height;

    for (std::size_t i = 0; i < data->layers.size(); i++) {
        const Layer &layer = data->layers[i];
        const int gid = tile_at(static_cast<int>(i), column, row);
        if (gid == 0) continue;
        // Convention one: the whole layer is the collision layer.
        if (layer.solid_layer) return true;
        // Convention two: this particular tile is marked solid in its tileset.
        for (const Tileset &set : data->tilesets) {
            if (gid < set.first_gid || gid > set.last_gid) continue;
            const auto local = static_cast<std::size_t>(gid - set.first_gid);
            if (local < set.solid.size() && set.solid[local]) return true;
        }
    }
    return false;
}

bool Tilemap::solid_in(Rectangle world_rect) const {
    if (!valid()) return false;
    const MapData *data = as_data(data_);
    if (data->tile_width <= 0 || data->tile_height <= 0) return false;

    // Every tile the rectangle touches, not just the four corners: a rectangle
    // wider than a tile can straddle a solid one with all four corners in empty
    // space, and that is the bug everybody writes the first time.
    const int first_col = static_cast<int>(world_rect.x) / data->tile_width;
    const int first_row = static_cast<int>(world_rect.y) / data->tile_height;
    const int last_col =
        static_cast<int>(world_rect.x + world_rect.width) / data->tile_width;
    const int last_row =
        static_cast<int>(world_rect.y + world_rect.height) / data->tile_height;

    for (int row = first_row; row <= last_row; row++) {
        for (int column = first_col; column <= last_col; column++) {
            const Vector2 middle{ static_cast<float>(column * data->tile_width) +
                                      static_cast<float>(data->tile_width) / 2,
                                  static_cast<float>(row * data->tile_height) +
                                      static_cast<float>(data->tile_height) / 2 };
            if (solid_at(middle)) return true;
        }
    }
    return false;
}

void Tilemap::on_object(const char *type, Callback<Scene &, const MapObject &> factory) {
    if (!valid() || type == nullptr) return;
    MapData *data = as_data(data_);
    const std::string key(type);
    for (auto &entry : data->factories) {
        if (entry.first == key) {
            // Replaces. Two factories for one class is never what anybody
            // means, and stacking them would spawn the object twice.
            entry.second = std::move(factory);
            return;
        }
    }
    data->factories.emplace_back(key, std::move(factory));
}

void Tilemap::spawn_objects(Scene &into) {
    if (!valid()) return;
    MapData *data = as_data(data_);
    for (const MapObject &object : data->objects) {
        bool made = false;
        for (auto &entry : data->factories) {
            if (entry.first != object.type) continue;
            entry.second(into, object);
            made = true;
            break;
        }
        if (made) continue;

        // No factory: a plain object with the position, the size, a collider
        // and `solid`. That is the right default for a wall or a platform drawn
        // in the editor, which is most of what an unregistered class is.
        auto &plain = into.spawn({ .position = object.position, .size = object.size });
        plain.rotation = object.rotation;
        plain.solid = true;
        plain.immovable = true;
        plain.visible = false; // the tiles are the picture; this is the shape
    }
}

void Tilemap::draw() const {
    if (!valid()) return;
    const MapData *data = as_data(data_);
    for (const Layer &layer : data->layers) {
        for (int row = 0; row < layer.height; row++) {
            for (int column = 0; column < layer.width; column++) {
                const auto at = static_cast<std::size_t>(row) *
                        static_cast<std::size_t>(layer.width) +
                    static_cast<std::size_t>(column);
                if (at >= layer.gids.size()) continue;
                const int gid = layer.gids[at];
                if (gid == 0) continue;

                for (const Tileset &set : data->tilesets) {
                    if (gid < set.first_gid || gid > set.last_gid) continue;
                    if (!set.texture.valid()) break;
                    const int local = gid - set.first_gid;
                    // The division is integer on purpose -- it is the row the
                    // tile sits on in the tileset -- and the cast comes after,
                    // which is what clang-tidy wants said out loud.
                    const int tile_column = local % set.columns;
                    const int tile_row = local / set.columns;
                    const Rectangle source{
                        static_cast<float>(tile_column * set.tile_width),
                        static_cast<float>(tile_row * set.tile_height),
                        static_cast<float>(set.tile_width),
                        static_cast<float>(set.tile_height)
                    };
                    const Vector2 at_world{ static_cast<float>(column * data->tile_width),
                                            static_cast<float>(row * data->tile_height) };
                    DrawTextureRec(set.texture, source, at_world, WHITE);
                    break;
                }
            }
        }
    }
}

} // namespace rmp
