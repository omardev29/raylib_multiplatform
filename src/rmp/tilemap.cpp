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

#include "internal.h" // RMP_REPORT_ONCE_KEYED, g_failed_count
#include "tilemap_internal.h"

#include <cute_tiled.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace rmp {

using tilemap::detail::Layer;
using tilemap::detail::MapData;
using tilemap::detail::ObjectInfo;
using tilemap::detail::Property;
using tilemap::detail::Tileset;

namespace {

// An external tileset, fetched the way every other asset is: through
// rmp::assets, so it arrives out of resources.rres in a release and out of
// resources/ in development without the map knowing which.
//
// Wrapped in a one-key map and given to cute_tiled's MAP parser rather than to
// its own cute_tiled_load_external_tileset_from_memory, which patches the
// strings of the tileset it has just failed to parse -- a null dereference, so
// an XML .tsx (which is what Tiled writes by default) would take the process
// down. The map parser checks, and comes back nullptr.
//
// Returns the wrapper map, which OWNS the tileset; the caller frees it with
// cute_tiled_free_map once it has copied what it needs.
cute_tiled_map_t *load_external_tileset(const char *source) {
    const std::vector<unsigned char> bytes =
        rmp::assets::load_data(tilemap::detail::file_name_of(source));
    if (bytes.empty()) return nullptr;
    const int size = static_cast<int>(bytes.size());
    std::string document;
    if (size > 0) {
        document.reserve(static_cast<std::size_t>(size) + 16);
        document = "{\"tilesets\":[";
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        document.append(reinterpret_cast<const char *>(bytes.data()),
                        static_cast<std::size_t>(size));
        document += "]}";
    }
    if (document.empty()) return nullptr;
    return cute_tiled_load_map_from_memory(document.data(),
                                           static_cast<int>(document.size()), nullptr);
}

// The tileset a gid belongs to, or nullptr. The ranges never overlap: Tiled
// hands out firstgid in order and each set claims tilecount of them.
const Tileset *tileset_for(const MapData &data, int gid) {
    for (const Tileset &set : data.tilesets) {
        if (gid >= set.first_gid && gid <= set.last_gid) return &set;
    }
    return nullptr;
}

// Where that tile is in the tileset image. MARGIN AND SPACING ARE PART OF IT:
// they are the first two fields of Tiled's own import dialog and what every
// atlas packer produces, and without them the grid drifts by one gutter per
// column -- so the tile drawn is a slice of two neighbours, and by the far
// corner of the sheet it is different art altogether.
Rectangle source_in(const Tileset &set, int gid) {
    const int local = gid - set.first_gid;
    // The division is integer on purpose -- it is the row the tile sits on in
    // the tileset -- and the cast comes after, which is what clang-tidy wants
    // said out loud.
    const int tile_column = local % set.columns;
    const int tile_row = local / set.columns;
    return Rectangle{
        static_cast<float>(set.margin + tile_column * (set.tile_width + set.spacing)),
        static_cast<float>(set.margin + tile_row * (set.tile_height + set.spacing)),
        static_cast<float>(set.tile_width), static_cast<float>(set.tile_height)
    };
}

// Where that tile's top-left corner goes in the world, which is NOT simply the
// cell's own corner. Tiled anchors a tile layer by the BOTTOM-left, so a
// tileset whose tiles are taller than the map's grid -- trees, walls, anything
// drawn standing up -- keeps its foot in the cell and reaches upwards out of
// it. Drawn from the cell's corner instead, the whole layer sits one tile too
// low and the tops are cut off.
Vector2 origin_in(const MapData &data, const Tileset *set, int column, int row) {
    const int overhang = set == nullptr ? 0 : set->tile_height - data.tile_height;
    return Vector2{ static_cast<float>(column * data.tile_width),
                    static_cast<float>(row * data.tile_height - overhang) };
}

const Property *find_property(const void *raw_object, const char *key) {
    if (raw_object == nullptr || key == nullptr) return nullptr;
    const auto *info = static_cast<const ObjectInfo *>(raw_object);
    for (const Property &p : info->properties) {
        if (p.key == key) return &p;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// MapObject
// ---------------------------------------------------------------------------

bool MapObject::property_bool(const char *key, bool fallback) const {
    const Property *p = find_property(raw, key);
    if (p == nullptr || p->kind != Property::Kind::BOOL) return fallback;
    return p->boolean;
}

int MapObject::property_int(const char *key, int fallback) const {
    const Property *p = find_property(raw, key);
    if (p == nullptr) return fallback;
    if (p->kind == Property::Kind::INT) return p->integer;
    // A float where an int was asked for is a rounding, not a failure: Tiled
    // will happily save 3 as 3.0 and nobody means anything by it.
    if (p->kind == Property::Kind::FLOAT) return static_cast<int>(p->floating);
    return fallback;
}

float MapObject::property_float(const char *key, float fallback) const {
    const Property *p = find_property(raw, key);
    if (p == nullptr) return fallback;
    if (p->kind == Property::Kind::FLOAT) return p->floating;
    if (p->kind == Property::Kind::INT) return static_cast<float>(p->integer);
    return fallback;
}

const char *MapObject::property_string(const char *key, const char *fallback) const {
    const Property *p = find_property(raw, key);
    if (p == nullptr || p->kind != Property::Kind::STRING) return fallback;
    return p->text.c_str();
}

Vector2 MapObject::property_point(const char *key, Vector2 fallback) const {
    return property_point(key, 0, fallback);
}

Vector2 MapObject::property_point(const char *key, int index, Vector2 fallback) const {
    const Property *p = find_property(raw, key);
    if (p == nullptr || p->kind != Property::Kind::POINT || index < 0 ||
        static_cast<std::size_t>(index) >= p->points.size()) {
        return fallback;
    }
    return p->points[static_cast<std::size_t>(index)];
}

int MapObject::property_count(const char *key) const {
    const Property *p = find_property(raw, key);
    if (p == nullptr) return 0;
    return p->kind == Property::Kind::POINT ? static_cast<int>(p->points.size()) : 1;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

namespace tilemap::detail {

MapPtr parse_tiled(const void *bytes, int size, const char *name) {
    if (bytes == nullptr || size <= 0) return { nullptr, &free_map };

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
        return { nullptr, &free_map };
    }

    // Everything is copied out of cute_tiled's tree into MapData, and the tree
    // goes when this function returns: nothing after parsing points into it.
    const std::unique_ptr<cute_tiled_map_t, void (*)(cute_tiled_map_t *)> tree(
        raw, &cute_tiled_free_map);
    auto data = std::make_unique<MapData>();
    data->width = raw->width;
    data->height = raw->height;
    data->tile_width = raw->tilewidth;
    data->tile_height = raw->tileheight;

    for (cute_tiled_tileset_t *set = raw->tilesets; set != nullptr; set = set->next) {
        // AN EXTERNAL TILESET. Tiled's New Tileset dialog does not embed by
        // default, so the map holds nothing but {"firstgid":1,"source":"x.tsj"}
        // and cute_tiled leaves every other field of it zero. That used to be
        // a map that loaded, reported the right bounds, drew nothing and was
        // solid nowhere, without one line in the log mentioning tilesets --
        // the most likely first experience a real Tiled user has.
        const char *source = set->source.ptr != nullptr ? set->source.ptr : "";
        cute_tiled_map_t *external = nullptr;
        const cute_tiled_tileset_t *from = set;
        if (source[0] != '\0' && set->tilecount <= 0) {
            external = load_external_tileset(source);
            if (external != nullptr && external->tilesets != nullptr &&
                external->tilesets->tilecount > 0) {
                from = external->tilesets;
            } else {
                RMP_REPORT_ONCE_KEYED(
                    source,
                    "MAP: [%s] keeps its tileset in \"%s\", and that file could not "
                    "be read. Tiled saves .tsx as XML and this reads JSON: in Tiled, "
                    "Tileset > Embed In Map and save the map again, or save the "
                    "tileset itself as .tsj. Until then that tileset draws nothing "
                    "and none of its tiles are solid.",
                    name != nullptr ? name : "", source);
                // Counted, so the CI boot gate sees it: a game shipped with a
                // tileset it cannot read comes back red rather than empty.
                rmp::assets::detail::g_failed_count++;
            }
        }

        Tileset out;
        out.first_gid = set->firstgid; // the MAP's, never the tileset file's
        out.last_gid = set->firstgid + from->tilecount - 1;
        out.columns = from->columns > 0 ? from->columns : 1;
        out.tile_width = from->tilewidth;
        out.tile_height = from->tileheight;
        out.margin = from->margin;
        out.spacing = from->spacing;
        out.solid.assign(
            static_cast<std::size_t>(from->tilecount > 0 ? from->tilecount : 0), false);

        for (cute_tiled_tile_descriptor_t *tile = from->tiles; tile != nullptr;
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

        const char *image = tilemap::detail::file_name_of(from->image.ptr);
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
        if (external != nullptr) cute_tiled_free_map(external);
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
                auto info = std::make_unique<ObjectInfo>();
                info->name = object->name.ptr != nullptr ? object->name.ptr : "";
                info->type = object->type.ptr != nullptr ? object->type.ptr : "";
                for (int i = 0; i < object->property_count; i++) {
                    const cute_tiled_property_t &tp = object->properties[i];
                    if (tp.name.ptr == nullptr) continue;
                    Property prop;
                    prop.key = tp.name.ptr;
                    switch (tp.type) {
                        case CUTE_TILED_PROPERTY_BOOL:
                            prop.kind = Property::Kind::BOOL;
                            prop.boolean = tp.data.boolean != 0;
                            break;
                        case CUTE_TILED_PROPERTY_INT:
                            prop.kind = Property::Kind::INT;
                            prop.integer = tp.data.integer;
                            break;
                        case CUTE_TILED_PROPERTY_FLOAT:
                            prop.kind = Property::Kind::FLOAT;
                            prop.floating = tp.data.floating;
                            break;
                        case CUTE_TILED_PROPERTY_STRING:
                        case CUTE_TILED_PROPERTY_FILE:
                            prop.kind = Property::Kind::STRING;
                            prop.text =
                                tp.data.string.ptr != nullptr ? tp.data.string.ptr : "";
                            break;
                        default:
                            continue; // colours and the rest: not something a game reads
                    }
                    info->properties.push_back(std::move(prop));
                }
                MapObject &out = data->add_object(std::move(info));
                out.size = Vector2{ object->width, object->height };
                // The top three bits are Tiled's flip flags and not part of
                // the id, exactly as in the tile-layer loop below. Press X in
                // the editor and a gid of 1 comes back as 0x80000001, which
                // read as an int is -2147483647 -- so a factory switching on
                // the gid falls through to its default, while `gid == 0 means
                // not a tile object` still passes and nothing looks wrong.
                out.gid =
                    static_cast<int>(static_cast<unsigned>(object->gid) & ~kFlipMask);
                // TILED GIVES A CORNER and rmp::Object's position is the
                // CENTRE. Converting here rather than at every call site is
                // most of what this struct is for -- and WHICH corner depends
                // on what the object is. A rectangle, an ellipse, a point or a
                // polygon is placed by its TOP-left; a TILE OBJECT, the one
                // you stamp with a tile, hangs from its BOTTOM-left, which is
                // where the cursor was when you placed it. Reading y as the
                // top for both put every stamped chest, torch and door exactly
                // one tile into the floor.
                const float top = out.gid != 0 ? object->y - object->height : object->y;
                out.position =
                    Vector2{ object->x + object->width / 2, top + object->height / 2 };
                out.rotation = object->rotation;
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

    return { data.release(), &free_map };
}

MapPtr parse_map(const void *bytes, int size, const char *name, const char *level) {
    const std::string file = name != nullptr ? name : "";
    // A level file on its own is half a project: its tilesets and layer
    // definitions live in the .ldtk.
    if (file.ends_with(".ldtkl")) {
        TraceLog(
            LOG_WARNING,
            "MAP: [%s] is one level of an LDtk project, which cannot be read without "
            "the project. Load the .ldtk with the level's name: "
            "load_map(\"world.ldtk\", \"Level_0\").",
            file.c_str());
        return { nullptr, &free_map };
    }
    const bool ldtk = file.ends_with(".ldtk");
    MapPtr out = ldtk ? parse_ldtk(bytes, size, name, level != nullptr ? level : "")
                      : parse_tiled(bytes, size, name);
    if (out != nullptr) out->source = file;
    return out;
}

const char *file_name_of(const char *path) {
    if (path == nullptr) return "";
    const char *last = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}

} // namespace tilemap::detail

// The MapObject the readers hand out: its strings and its properties live in
// `info`, which the map keeps alive and which never moves.
MapObject &tilemap::detail::MapData::add_object(std::unique_ptr<ObjectInfo> info) {
    MapObject out;
    out.name = info->name.c_str();
    out.type = info->type.c_str();
    out.iid = info->iid.c_str();
    out.raw = info.get();
    infos.push_back(std::move(info));
    objects.push_back(out);
    return objects.back();
}

namespace tilemap::detail {

// The deleter behind MapPtr: this is the one translation unit that knows what
// a MapData is. A unique_ptr doing the freeing, so there is no `delete` here.
void free_map(MapData *map) { const std::unique_ptr<MapData> owner(map); }

int object_count(const MapData *data) {
    return data == nullptr ? 0 : static_cast<int>(data->objects.size());
}

const MapObject *object_at(const MapData *data, int index) {
    if (data == nullptr) return nullptr;
    if (index < 0 || static_cast<std::size_t>(index) >= data->objects.size()) {
        return nullptr;
    }
    return &data->objects[static_cast<std::size_t>(index)];
}

float smallest_cell(const MapData *data) {
    if (data == nullptr) return 0;
    float smallest = 0;
    for (const Layer &layer : data->layers) {
        const int w = layer.cell_width > 0 ? layer.cell_width : data->tile_width;
        const int h = layer.cell_height > 0 ? layer.cell_height : data->tile_height;
        const auto cell = static_cast<float>(std::min(w, h));
        if (cell > 0 && (smallest == 0 || cell < smallest)) smallest = cell;
    }
    return smallest;
}

Rectangle tile_source(const MapData *data, int gid) {
    if (data == nullptr) return Rectangle{};
    const Tileset *set = tileset_for(*data, gid);
    return set == nullptr ? Rectangle{} : source_in(*set, gid);
}

Vector2 tile_origin(const MapData *data, int gid, int column, int row) {
    if (data == nullptr) return Vector2{};
    return origin_in(*data, tileset_for(*data, gid), column, row);
}

} // namespace tilemap::detail

// ---------------------------------------------------------------------------
// Tilemap
// ---------------------------------------------------------------------------

// Out of line, all four, because MapData is incomplete in the header and a
// unique_ptr to an incomplete type cannot be destroyed there.
Tilemap::Tilemap() = default;
Tilemap::~Tilemap() = default;
Tilemap::Tilemap(Tilemap &&other) noexcept = default;
Tilemap &Tilemap::operator=(Tilemap &&other) noexcept = default;

void Tilemap::adopt(tilemap::detail::MapPtr data) { data_ = std::move(data); }

namespace {

// A layer's cell size: its own when it has one (LDtk layers each have a grid),
// the map's otherwise (every Tiled layer).
int cell_w(const MapData &data, const Layer &layer) {
    return layer.cell_width > 0 ? layer.cell_width : data.tile_width;
}
int cell_h(const MapData &data, const Layer &layer) {
    return layer.cell_height > 0 ? layer.cell_height : data.tile_height;
}

// The cell of `layer` under a world position, or false when it is outside.
bool cell_under(const MapData &data, const Layer &layer, Vector2 world, int *column,
                int *row) {
    const int w = cell_w(data, layer);
    const int h = cell_h(data, layer);
    if (w <= 0 || h <= 0) return false;
    const float x = world.x - data.origin.x - layer.offset.x;
    const float y = world.y - data.origin.y - layer.offset.y;
    if (x < 0 || y < 0) return false;
    *column = static_cast<int>(x) / w;
    *row = static_cast<int>(y) / h;
    return *column < layer.width && *row < layer.height;
}

std::size_t cell_index(const Layer &layer, int column, int row) {
    return static_cast<std::size_t>(row) * static_cast<std::size_t>(layer.width) +
        static_cast<std::size_t>(column);
}

int gid_at(const Layer &layer, int column, int row) {
    if (column < 0 || row < 0 || column >= layer.width || row >= layer.height) return 0;
    const std::size_t at = cell_index(layer, column, row);
    return at < layer.gids.size() ? layer.gids[at] : 0;
}

// Whether one cell of one layer stops you. Asked of THAT layer only: LDtk
// layers can each have their own grid, and a cell of one is not a cell of
// another.
bool solid_cell(const MapData &data, const Layer &layer, int column, int row) {
    if (column < 0 || row < 0 || column >= layer.width || row >= layer.height)
        return false;
    // LDtk's IntGrid: the cells painted with the value called `solid`.
    const std::size_t at = cell_index(layer, column, row);
    if (at < layer.solid_cells.size() && layer.solid_cells[at]) return true;
    const int gid = gid_at(layer, column, row);
    if (gid == 0) return false;
    // The whole layer is the collision layer (Tiled's class `solid`).
    if (layer.solid_layer) return true;
    // This particular tile is marked solid in its tileset.
    for (const Tileset &set : data.tilesets) {
        if (gid < set.first_gid || gid > set.last_gid) continue;
        const auto local = static_cast<std::size_t>(gid - set.first_gid);
        return local < set.solid.size() && set.solid[local];
    }
    return false;
}

} // namespace

Rectangle Tilemap::bounds() const {
    if (!valid()) return Rectangle{};
    const MapData *data = data_.get();
    const Vector2 size = data->size_px.x > 0
        ? data->size_px
        : Vector2{ static_cast<float>(data->width * data->tile_width),
                   static_cast<float>(data->height * data->tile_height) };
    return Rectangle{ data->origin.x, data->origin.y, size.x, size.y };
}

Vector2 Tilemap::tile_size() const {
    if (!valid()) return Vector2{};
    const MapData *data = data_.get();
    return Vector2{ static_cast<float>(data->tile_width),
                    static_cast<float>(data->tile_height) };
}

int Tilemap::layer_count() const {
    return valid() ? static_cast<int>(data_.get()->layers.size()) : 0;
}

int Tilemap::object_count() const { return tilemap::detail::object_count(data_.get()); }

const char *Tilemap::level() const { return valid() ? data_.get()->level.c_str() : ""; }

const char *Tilemap::neighbour_at(Vector2 world_position) const {
    if (!valid()) return "";
    const MapData *data = data_.get();
    // Still inside this level is no neighbour -- which is what makes "walked
    // out, so change" a single if. Half-open, like the cells: a point on the
    // right or bottom edge is already the next level's.
    const Rectangle here = bounds();
    if (world_position.x >= here.x && world_position.y >= here.y &&
        world_position.x < here.x + here.width &&
        world_position.y < here.y + here.height) {
        return "";
    }
    for (const tilemap::detail::LevelInfo &level : data->levels) {
        const Rectangle r = level.world;
        if (world_position.x >= r.x && world_position.y >= r.y &&
            world_position.x < r.x + r.width && world_position.y < r.y + r.height) {
            return level.name.c_str();
        }
    }
    return "";
}

int Tilemap::tile_at(int layer_index, int column, int row) const {
    if (!valid()) return 0;
    const MapData *data = data_.get();
    if (layer_index < 0 || static_cast<std::size_t>(layer_index) >= data->layers.size()) {
        return 0;
    }
    return gid_at(data->layers[static_cast<std::size_t>(layer_index)], column, row);
}

bool Tilemap::solid_at(Vector2 world_position) const {
    if (!valid()) return false;
    const MapData *data = data_.get();
    for (const Layer &layer : data->layers) {
        int column = 0;
        int row = 0;
        if (!cell_under(*data, layer, world_position, &column, &row)) continue;
        if (solid_cell(*data, layer, column, row)) return true;
    }
    return false;
}

bool Tilemap::solid_in(Rectangle world_rect) const {
    if (!valid()) return false;
    const MapData *data = data_.get();
    // Every cell the rectangle covers, not just the four corners: a rectangle
    // wider than a tile can straddle a solid one with all four corners in empty
    // space, and that is the bug everybody writes the first time. Each layer
    // in its own grid, because LDtk layers can have different ones.
    //
    // COVERS, not touches: the right and bottom edges are open, so a box
    // resting exactly on a floor -- bottom edge on the floor's top -- is not
    // in the floor, and one flush against a wall is not in the wall. That is
    // what lets the collision pass stop an object AT a cell instead of
    // calling it stuck there. A rectangle with no area asks about the cell
    // its corner is in, as solid_at() does.
    //
    // The span is clamped to the layer before it becomes an int: a rectangle
    // a million cells wide asks about the cells there are, and a NaN or an
    // infinity asks about none.
    if (!(world_rect.width >= 0) || !(world_rect.height >= 0)) return false;
    for (const Layer &layer : data->layers) {
        const int w = cell_w(*data, layer);
        const int h = cell_h(*data, layer);
        if (w <= 0 || h <= 0 || layer.width <= 0 || layer.height <= 0) continue;
        const float left =
            (world_rect.x - data->origin.x - layer.offset.x) / static_cast<float>(w);
        const float top =
            (world_rect.y - data->origin.y - layer.offset.y) / static_cast<float>(h);
        const float right = left + (world_rect.width / static_cast<float>(w));
        const float bottom = top + (world_rect.height / static_cast<float>(h));
        if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
            !std::isfinite(bottom)) {
            continue;
        }
        const auto cells_x = static_cast<float>(layer.width);
        const auto cells_y = static_cast<float>(layer.height);
        const float first_x = std::floor(left);
        const float first_y = std::floor(top);
        const float last_x = std::max(first_x, std::ceil(right) - 1);
        const float last_y = std::max(first_y, std::ceil(bottom) - 1);
        if (last_x < 0 || last_y < 0 || first_x >= cells_x || first_y >= cells_y)
            continue;
        const int first_col = static_cast<int>(std::max(first_x, 0.0f));
        const int first_row = static_cast<int>(std::max(first_y, 0.0f));
        const int last_col = static_cast<int>(std::min(last_x, cells_x - 1));
        const int last_row = static_cast<int>(std::min(last_y, cells_y - 1));
        for (int row = first_row; row <= last_row; row++) {
            for (int column = first_col; column <= last_col; column++) {
                if (solid_cell(*data, layer, column, row)) return true;
            }
        }
    }
    return false;
}

void Tilemap::on_object(const char *type, Callback<Scene &, const MapObject &> factory) {
    if (!valid() || type == nullptr) return;
    MapData *data = data_.get();
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
    MapData *data = data_.get();
    for (const MapObject &object : data->objects) {
        bool made = false;
        for (auto &entry : data->factories) {
            if (entry.first != object.type) continue;
            entry.second(into, object);
            made = true;
            break;
        }
        if (made) continue;

        // No factory: a plain object with the position, the size and a
        // collider. `solid` too when TILED GAVE IT AN AREA, which is the right
        // default for a wall or a platform drawn in the editor.
        //
        // But an object layer holds the other half of a level as well -- spawn
        // points, camera targets, waypoints, audio emitters -- and a Tiled
        // point has width and height 0, as does a polyline. A zero-sized
        // object is a marker by construction, and making one solid puts an
        // invisible collider exactly where the designer meant a label.
        const bool has_area = object.size.x > 0 && object.size.y > 0;
        // And an LDtk entity is a thing, never a shape: walls there are IntGrid.
        const auto *info = static_cast<const ObjectInfo *>(object.raw);
        const bool shape = info == nullptr || info->solid_area;
        auto &plain = into.spawn({ .position = object.position, .size = object.size });
        plain.rotation = object.rotation;
        plain.solid = has_area && shape;
        plain.immovable = true;
        plain.visible = false; // the tiles are the picture; this is the shape
    }
}

void Tilemap::draw() const {
    if (!valid()) return;
    const MapData *data = data_.get();
    for (const Layer &layer : data->layers) {
        if (!layer.visible || layer.opacity <= 0) continue;
        const Vector2 base{ data->origin.x + layer.offset.x,
                            data->origin.y + layer.offset.y };
        const auto alpha = [&](float a) {
            const float value = layer.opacity * a * 255.0f;
            return static_cast<unsigned char>(value < 0 ? 0
                                                        : (value > 255 ? 255 : value));
        };

        if (!layer.placed.empty()) {
            // LDtk: every tile where it was put, bottom to top, each flipped
            // and faded on its own. A negative source size is how raylib
            // mirrors a region.
            for (const tilemap::detail::PlacedTile &tile : layer.placed) {
                const Tileset *set = tileset_for(*data, tile.gid);
                if (set == nullptr || !set->texture.valid()) continue;
                Rectangle src = source_in(*set, tile.gid);
                if (tile.flip_x) src.width = -src.width;
                if (tile.flip_y) src.height = -src.height;
                DrawTextureRec(set->texture, src,
                               Vector2{ base.x + tile.at.x, base.y + tile.at.y },
                               Color{ 255, 255, 255, alpha(tile.alpha) });
            }
            continue;
        }

        const Color tint{ 255, 255, 255, alpha(1.0f) };
        for (int row = 0; row < layer.height; row++) {
            for (int column = 0; column < layer.width; column++) {
                const int gid = gid_at(layer, column, row);
                if (gid == 0) continue;
                const Tileset *set = tileset_for(*data, gid);
                if (set == nullptr || !set->texture.valid()) continue;
                const Vector2 at = origin_in(*data, set, column, row);
                DrawTextureRec(set->texture, source_in(*set, gid),
                               Vector2{ base.x + at.x, base.y + at.y }, tint);
            }
        }
    }
}

} // namespace rmp
