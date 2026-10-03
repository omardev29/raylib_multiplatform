// ---------------------------------------------------------------------------
// LDtk projects: one level of a .ldtk into the same MapData Tiled fills.
//
// LDtk is the editor this framework TESTS AND MAINTAINS; the Tiled reader in
// tilemap.cpp stays as it is. The format is the one LDtk documents for 1.5
// (https://ldtk.io/json), read through cJSON. Everything after parsing --
// drawing, solids, factories -- is tilemap.cpp's and does not know which
// editor made the map.
//
// WHAT IS READ, AND WHERE EACH THING GOES:
//
//   level             one per map. load_map("world.ldtk", "Level_2") picks it
//                     by identifier, and no name means the first. A level in
//                     its own file (Project settings > "Save levels to separate
//                     files") is a .ldtkl, fetched by FILE NAME through
//                     rmp::assets, like the tileset images.
//   world position    a level's worldX/worldY, in GridVania and Free layouts.
//                     The map's origin is there, and every position the map
//                     hands out -- objects, solids, bounds() -- is in world
//                     units. The linear layouts have no world (LDtk writes -1)
//                     and their levels all start at 0,0.
//   layers            layerInstances, which LDtk lists TOP FIRST -- the
//                     reverse of the editor's panel and of what a draw loop
//                     wants. Reversed here, which is the mistake every LDtk
//                     reader makes once. Entity layers become objects; the
//                     rest become layers, with their own grid, offset,
//                     opacity and visibility.
//   tiles             gridTiles and autoLayerTiles, each where it was put
//                     (`px`), flipped by its two bits of `f`, faded by `a`, in
//                     the order the array gives, which is the order to draw.
//   solid             an IntGrid value whose identifier is `solid`, and any
//                     tile tagged `solid` with its tileset's enum. Compared
//                     without case: LDtk's default identifier style
//                     capitalises, so the value typed as solid is saved as
//                     Solid.
//   entities          MapObjects. The position is the CENTRE, as for Tiled
//                     and rmp::Object: LDtk gives the pivot (`px` and
//                     `__pivot`), and the conversion is here so that nobody
//                     writes it. An entity is a thing and not a shape -- its
//                     box is only how big it looks in the editor, and walls
//                     are IntGrid -- so one without a factory comes out
//                     non-solid.
//   fields            Int, Float, Bool, String and Multilines as themselves;
//                     an Enum as its value's name; a Color as "#rrggbb"; a
//                     Point as the world position of the centre of its cell,
//                     and Array<Point> as a list of them; an EntityRef as the
//                     iid of the entity it names. A null field is a field
//                     nobody set and reads as the fallback. Other arrays,
//                     FilePath and Tile wait for a game that needs them.
//
// What is NOT read: level backgrounds, parallax, the IntGrid values other than
// solid, and level fields. Each is a few lines when a game asks for it.
// ---------------------------------------------------------------------------

#include <rmp/assets.h>
#include <rmp/tilemap.h>

#include "internal.h" // RMP_REPORT_ONCE_KEYED, g_failed_count
#include "json_internal.h" // Json, CNumbers
#include "tilemap_internal.h"

#include <raylib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rmp::tilemap::detail {

namespace {

using rmp::detail::CNumbers;
using rmp::detail::Json;

// Bigger than any level or project an editor makes, and small enough that a
// broken or hostile file cannot make the reader allocate gigabytes. BUDGETS FOR
// THE WHOLE FILE, not limits per layer or per tileset: a limit per layer let a
// 1.5 KB file of a dozen 4096 x 4096 layers take three quarters of a gigabyte.
// 16M cells is all the layers of a 65 536 x 65 536 px level at 16 px; 4M tiles
// is every tileset of a project, each a 32 768 px square of 16 px tiles.
constexpr long long kMaxCells = 1LL << 24;
constexpr long long kMaxTiles = 1LL << 22;

// A coordinate in cells, or false when it is not a finite number inside
// 0..limit: a float past INT_MAX, an infinity or a NaN cast to int is
// undefined behaviour, and a file can hold any of them.
bool cell_of(double pixels, int cell, int limit, int *out) {
    if (cell <= 0) return false;
    const double c = std::floor(pixels / cell);
    if (!std::isfinite(c) || c < 0 || c >= limit) return false;
    *out = static_cast<int>(c);
    return true;
}

// ---- reading cJSON without trusting it -------------------------------------
//
// Every lookup survives the wrong type: a hand-edited project with a string
// where a number goes reads as the fallback, never as a crash.

const cJSON *member(const cJSON *object, const char *key) {
    return cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, key)
                                  : nullptr;
}

double number(const cJSON *object, const char *key, double fallback) {
    const cJSON *item = member(object, key);
    return item != nullptr && cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

// An int out of a double, clamped: cJSON reads every number as a double, and
// a value past INT_MAX cast straight to int is undefined behaviour.
int as_int(double value) {
    if (std::isnan(value)) return 0;
    constexpr double kMax = 2147483647.0;
    constexpr double kMin = -2147483648.0;
    return static_cast<int>(std::clamp(value, kMin, kMax));
}

// A float out of a double, clamped and with NaN as 0: a double past FLT_MAX
// converted to float is undefined behaviour in C++, IEEE or not.
float as_float(double value) {
    if (std::isnan(value)) return 0;
    constexpr double kMax = 3.4e38;
    return static_cast<float>(std::clamp(value, -kMax, kMax));
}

int integer(const cJSON *object, const char *key, int fallback) {
    const cJSON *item = member(object, key);
    return item != nullptr && cJSON_IsNumber(item) ? as_int(item->valuedouble) : fallback;
}

const char *text(const cJSON *object, const char *key) {
    const cJSON *item = member(object, key);
    return item != nullptr && cJSON_IsString(item) && item->valuestring != nullptr
        ? item->valuestring
        : "";
}

const cJSON *array(const cJSON *object, const char *key) {
    const cJSON *item = member(object, key);
    return cJSON_IsArray(item) ? item : nullptr;
}

// Element `i` of a [x, y] pair, as LDtk writes px, src and __pivot.
double pair_at(const cJSON *object, const char *key, int i, double fallback) {
    const cJSON *item = cJSON_GetArrayItem(array(object, key), i);
    return item != nullptr && cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

bool same_word(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
    }
    return *a == *b;
}

// ---- the project, as far as one level needs it -----------------------------

struct TilesetRef {
    int uid = 0;
    std::size_t index = 0; // into MapData::tilesets
    std::string image; // the file name, "" for LDtk's own icon atlas
    bool used = false;
};

struct Project {
    const char *name = ""; // the file, for the messages
    std::vector<TilesetRef> tilesets;
    const cJSON *layer_defs = nullptr;
    long long cells_left = kMaxCells; // what the level's layers may still allocate
};

// Every tileset of the project gets a range of gids, used or not, so a gid
// means the same tile whichever level is loaded. Only the ones a layer of
// this level draws with load their image (see load_used_textures).
void read_tilesets(const cJSON *defs, Project *project, MapData *data) {
    // Gids start at 1 and never pass kMaxTiles + 1, so none of the sums below
    // can overflow an int whatever the file says.
    int next_gid = 1;
    long long tiles_left = kMaxTiles;
    const cJSON *set = nullptr;
    cJSON_ArrayForEach(set, array(defs, "tilesets")) {
        Tileset out;
        const int columns = integer(set, "__cWid", 0);
        const int rows = integer(set, "__cHei", 0);
        const long long area =
            columns > 0 && rows > 0 ? static_cast<long long>(columns) * rows : 0;
        const int count = area <= tiles_left ? static_cast<int>(area) : 0;
        if (area > tiles_left) {
            TraceLog(LOG_WARNING,
                     "MAP: [%s] has tilesets of more tiles than any real project; one of "
                     "%lld tiles draws nothing.",
                     project->name, area);
        }
        tiles_left -= count;
        out.first_gid = next_gid;
        out.last_gid = next_gid + count - 1;
        next_gid += count;
        out.columns = columns > 0 ? columns : 1;
        out.tile_width = integer(set, "tileGridSize", 0);
        out.tile_height = out.tile_width;
        out.margin = integer(set, "padding", 0);
        out.spacing = integer(set, "spacing", 0);
        out.solid.assign(static_cast<std::size_t>(count), false);

        // The tiles tagged `solid` with the tileset's enum.
        const cJSON *tag = nullptr;
        cJSON_ArrayForEach(tag, array(set, "enumTags")) {
            if (!same_word(text(tag, "enumValueId"), "solid")) continue;
            const cJSON *id = nullptr;
            cJSON_ArrayForEach(id, array(tag, "tileIds")) {
                if (!cJSON_IsNumber(id)) continue;
                const int local = as_int(id->valuedouble);
                if (local >= 0 && local < count)
                    out.solid[static_cast<std::size_t>(local)] = true;
            }
        }

        TilesetRef ref;
        ref.uid = integer(set, "uid", -1);
        ref.index = data->tilesets.size();
        ref.image = file_name_of(text(set, "relPath"));
        project->tilesets.push_back(std::move(ref));
        data->tilesets.push_back(std::move(out));
    }
}

TilesetRef *tileset_by_uid(Project *project, int uid) {
    for (TilesetRef &ref : project->tilesets) {
        if (ref.uid == uid) return &ref;
    }
    return nullptr;
}

void load_used_textures(const Project &project, MapData *data) {
    for (const TilesetRef &ref : project.tilesets) {
        if (!ref.used) continue;
        if (ref.image.empty()) {
            // LDtk's built-in icon atlas has no file to ship.
            RMP_REPORT_ONCE_KEYED(
                project.name,
                "MAP: [%s] draws a layer with LDtk's built-in icons, which have no "
                "image file to load; that layer draws nothing. Use a tileset of your "
                "own for anything the game shows.",
                project.name);
            continue;
        }
        Tileset &set = data->tilesets[ref.index];
        set.texture = rmp::assets::load_texture(ref.image);
        if (!set.texture.valid()) {
            TraceLog(
                LOG_WARNING,
                "MAP: the tileset image \"%s\" of [%s] is not in resources/. The map "
                "loads and draws nothing for that tileset.",
                ref.image.c_str(), project.name);
        }
    }
}

// The IntGrid values of a layer definition called `solid`, as a lookup by value.
std::vector<bool> solid_values(const Project &project, int layer_def_uid) {
    std::vector<bool> solid;
    const cJSON *def = nullptr;
    cJSON_ArrayForEach(def, project.layer_defs) {
        if (integer(def, "uid", -1) != layer_def_uid) continue;
        const cJSON *value = nullptr;
        cJSON_ArrayForEach(value, array(def, "intGridValues")) {
            if (!same_word(text(value, "identifier"), "solid")) continue;
            const int v = integer(value, "value", -1);
            // IntGrid values are small (1, 2, 3...); a bogus one is ignored
            // rather than allocated for.
            if (v < 0 || v > 4096) continue;
            if (static_cast<std::size_t>(v) >= solid.size()) {
                solid.resize(static_cast<std::size_t>(v) + 1, false);
            }
            solid[static_cast<std::size_t>(v)] = true;
        }
        break;
    }
    return solid;
}

// ---- one layer --------------------------------------------------------------

void read_tiles(const cJSON *tiles, const Tileset &set, Layer *layer) {
    const cJSON *tile = nullptr;
    cJSON_ArrayForEach(tile, tiles) {
        const int id = integer(tile, "t", -1);
        // Against the COUNT, not first_gid + id against last_gid: a tile id
        // near INT_MAX overflowed that sum and passed the check.
        if (id < 0 || id > set.last_gid - set.first_gid) continue;
        PlacedTile placed;
        placed.gid = set.first_gid + id;
        placed.at = Vector2{ as_float(pair_at(tile, "px", 0, 0)),
                             as_float(pair_at(tile, "px", 1, 0)) };
        const int flips = integer(tile, "f", 0);
        placed.flip_x = (flips & 1) != 0;
        placed.flip_y = (flips & 2) != 0;
        placed.alpha = as_float(number(tile, "a", 1.0));
        layer->placed.push_back(placed);

        // The grid view of the same tiles: the top one of each cell, for
        // tile_at(), and the cell marked solid if ANY tile in it is tagged
        // so -- an auto-layer puts grass over ground, and the grass on top
        // must not make the ground walkable.
        int column = 0;
        int row = 0;
        if (!cell_of(pair_at(tile, "px", 0, 0), layer->cell_width, layer->width,
                     &column) ||
            !cell_of(pair_at(tile, "px", 1, 0), layer->cell_height, layer->height,
                     &row)) {
            continue;
        }
        const std::size_t at =
            static_cast<std::size_t>(row) * static_cast<std::size_t>(layer->width) +
            static_cast<std::size_t>(column);
        layer->gids[at] = placed.gid;
        if (set.solid[static_cast<std::size_t>(id)]) layer->solid_cells[at] = true;
    }
}

// A field of an entity into a Property, or false when it is null or of a kind
// that is not read.
bool read_field(const cJSON *field, Vector2 cell_origin, float grid, Property *out) {
    const char *type = text(field, "__type");
    const cJSON *value = member(field, "__value");
    if (value == nullptr || cJSON_IsNull(value)) return false;
    out->key = text(field, "__identifier");
    const std::string kind(type);

    // The centre of cell {cx, cy} of the entity's layer, in the world.
    const auto point = [&](const cJSON *p, Vector2 *at) {
        if (!cJSON_IsNumber(member(p, "cx")) || !cJSON_IsNumber(member(p, "cy")))
            return false;
        *at = Vector2{ cell_origin.x + (as_float(number(p, "cx", 0)) + 0.5f) * grid,
                       cell_origin.y + (as_float(number(p, "cy", 0)) + 0.5f) * grid };
        return true;
    };

    if (kind == "Int" && cJSON_IsNumber(value)) {
        out->kind = Property::Kind::INT;
        out->integer = as_int(value->valuedouble);
        return true;
    }
    if (kind == "Float" && cJSON_IsNumber(value)) {
        out->kind = Property::Kind::FLOAT;
        out->floating = as_float(value->valuedouble);
        return true;
    }
    if (kind == "Bool" && cJSON_IsBool(value)) {
        out->kind = Property::Kind::BOOL;
        out->boolean = cJSON_IsTrue(value) != 0;
        return true;
    }
    // Strings, colours ("#rrggbb") and enums (their value's name) are text.
    const bool textual = kind == "String" || kind == "Multilines" || kind == "Color" ||
        kind.starts_with("LocalEnum.") || kind.starts_with("ExternEnum.");
    if (textual && cJSON_IsString(value) && value->valuestring != nullptr) {
        out->kind = Property::Kind::STRING;
        out->text = value->valuestring;
        return true;
    }
    if (kind == "EntityRef") {
        const char *iid = text(value, "entityIid");
        if (iid[0] == '\0') return false;
        out->kind = Property::Kind::STRING;
        out->text = iid;
        return true;
    }
    if (kind == "Point") {
        Vector2 at{};
        if (!point(value, &at)) return false;
        out->kind = Property::Kind::POINT;
        out->points.push_back(at);
        return true;
    }
    if (kind == "Array<Point>" && cJSON_IsArray(value)) {
        out->kind = Property::Kind::POINT;
        const cJSON *p = nullptr;
        cJSON_ArrayForEach(p, value) {
            Vector2 at{};
            if (point(p, &at)) out->points.push_back(at);
        }
        return true; // an empty list is a list: property_count() says 0
    }
    return false;
}

void read_entities(const cJSON *layer, Vector2 base, float grid, MapData *data) {
    const cJSON *entity = nullptr;
    cJSON_ArrayForEach(entity, array(layer, "entityInstances")) {
        auto info = std::make_unique<ObjectInfo>();
        info->type = text(entity, "__identifier");
        info->iid = text(entity, "iid");
        info->solid_area = false; // a thing, not a shape: walls are IntGrid
        const cJSON *field = nullptr;
        cJSON_ArrayForEach(field, array(entity, "fieldInstances")) {
            Property prop;
            if (read_field(field, base, grid, &prop))
                info->properties.push_back(std::move(prop));
        }
        const auto w = as_float(number(entity, "width", 0));
        const auto h = as_float(number(entity, "height", 0));
        // `px` is where the PIVOT is, and the pivot is a fraction of the
        // size: 0.5,1 (the default) is the middle of the bottom edge.
        const auto pivot_x = as_float(pair_at(entity, "__pivot", 0, 0.5));
        const auto pivot_y = as_float(pair_at(entity, "__pivot", 1, 1.0));
        const auto px = as_float(pair_at(entity, "px", 0, 0));
        const auto py = as_float(pair_at(entity, "px", 1, 0));

        MapObject &out = data->add_object(std::move(info));
        out.size = Vector2{ w, h };
        out.position = Vector2{ base.x + px - (pivot_x * w) + (w / 2),
                                base.y + py - (pivot_y * h) + (h / 2) };
    }
}

void read_layer(const cJSON *layer, Project *project, MapData *data) {
    const std::string type = text(layer, "__type");
    const int grid = integer(layer, "__gridSize", 0);
    const Vector2 offset{ as_float(number(layer, "__pxTotalOffsetX", 0)),
                          as_float(number(layer, "__pxTotalOffsetY", 0)) };
    if (grid <= 0) return;

    if (type == "Entities") {
        read_entities(layer,
                      Vector2{ data->origin.x + offset.x, data->origin.y + offset.y },
                      static_cast<float>(grid), data);
        return;
    }
    if (type != "IntGrid" && type != "Tiles" && type != "AutoLayer") return;

    Layer out;
    out.name = text(layer, "__identifier");
    out.width = std::max(0, integer(layer, "__cWid", 0));
    out.height = std::max(0, integer(layer, "__cHei", 0));
    const long long area = static_cast<long long>(out.width) * out.height;
    if (area > project->cells_left) {
        TraceLog(LOG_WARNING,
                 "MAP: layer \"%s\" of [%s] is %d x %d cells, more than any real level "
                 "has in all its layers; it is left out.",
                 out.name.c_str(), project->name, out.width, out.height);
        return;
    }
    project->cells_left -= area;
    out.cell_width = grid;
    out.cell_height = grid;
    out.offset = offset;
    out.opacity = as_float(number(layer, "__opacity", 1.0));
    const cJSON *visible = member(layer, "visible");
    out.visible = !cJSON_IsBool(visible) || cJSON_IsTrue(visible) != 0;
    const std::size_t cells =
        static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height);
    out.gids.assign(cells, 0);
    out.solid_cells.assign(cells, false);

    if (type == "IntGrid") {
        const std::vector<bool> solid =
            solid_values(*project, integer(layer, "layerDefUid", -1));
        std::size_t at = 0;
        const cJSON *value = nullptr;
        cJSON_ArrayForEach(value, array(layer, "intGridCsv")) {
            if (at >= cells) break;
            const int v = cJSON_IsNumber(value) ? as_int(value->valuedouble) : 0;
            if (v > 0 && static_cast<std::size_t>(v) < solid.size() &&
                solid[static_cast<std::size_t>(v)]) {
                out.solid_cells[at] = true;
            }
            at++;
        }
    }

    const cJSON *uid = member(layer, "__tilesetDefUid");
    TilesetRef *ref =
        cJSON_IsNumber(uid) ? tileset_by_uid(project, as_int(uid->valuedouble)) : nullptr;
    if (ref != nullptr) {
        const Tileset &set = data->tilesets[ref->index];
        const std::size_t before = out.placed.size();
        read_tiles(array(layer, "gridTiles"), set, &out);
        read_tiles(array(layer, "autoLayerTiles"), set, &out);
        ref->used = ref->used || out.placed.size() > before;
    }
    data->layers.push_back(std::move(out));
}

// ---- levels and worlds -------------------------------------------------------

// The levels of a project, wherever they are: in `levels` for a single world,
// in each of `worlds` when the project has several. Paired with the layout of
// the world they belong to.
struct LevelRef {
    const cJSON *level = nullptr;
    std::string layout;
    int world = 0;
};

std::vector<LevelRef> every_level(const cJSON *root) {
    std::vector<LevelRef> out;
    const cJSON *level = nullptr;
    cJSON_ArrayForEach(level, array(root, "levels")) {
        out.push_back({ level, text(root, "worldLayout"), 0 });
    }
    int index = 1;
    const cJSON *world = nullptr;
    cJSON_ArrayForEach(world, array(root, "worlds")) {
        cJSON_ArrayForEach(level, array(world, "levels")) {
            out.push_back({ level, text(world, "worldLayout"), index });
        }
        index++;
    }
    return out;
}

bool has_world(const std::string &layout) {
    return layout == "GridVania" || layout == "Free";
}

// A level kept in its own file. Held in `owner`, which outlives the parse.
const cJSON *external_layers(const cJSON *level, const char *project_name, Json *owner) {
    const char *path = text(level, "externalRelPath");
    if (path[0] == '\0') return nullptr;
    const char *file = file_name_of(path);
    const std::vector<unsigned char> bytes = rmp::assets::load_data(file);
    if (bytes.empty()) {
        TraceLog(LOG_WARNING,
                 "MAP: [%s] keeps level \"%s\" in \"%s\", and that file is not in "
                 "resources/. Copy the project's .ldtkl files next to it.",
                 project_name, text(level, "identifier"), file);
        return nullptr;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    owner->reset(cJSON_ParseWithLength(reinterpret_cast<const char *>(bytes.data()),
                                       bytes.size()));
    if (*owner == nullptr) {
        TraceLog(LOG_WARNING, "MAP: \"%s\" (a level of [%s]) is not valid JSON.", file,
                 project_name);
        return nullptr;
    }
    const cJSON *layers = array(owner->get(), "layerInstances");
    if (layers == nullptr) {
        TraceLog(LOG_WARNING, R"(MAP: "%s" (a level of [%s]) has no "layerInstances".)",
                 file, project_name);
    }
    return layers;
}

void say_levels(const std::vector<LevelRef> &levels, const char *name,
                const char *wanted) {
    std::string list;
    for (const LevelRef &ref : levels) {
        if (!list.empty()) list += ", ";
        list += text(ref.level, "identifier");
        if (list.size() > 400) {
            list += ", ...";
            break;
        }
    }
    TraceLog(LOG_WARNING, "MAP: [%s] has no level called \"%s\". Its levels: %s", name,
             wanted, list.empty() ? "(none)" : list.c_str());
}

} // namespace

MapPtr parse_ldtk(const void *bytes, int size, const char *name, const char *level) {
    if (name == nullptr) name = "";
    if (level == nullptr) level = "";
    if (bytes == nullptr || size <= 0) return { nullptr, &free_map };

    // Every number of the project is read in the "C" locale, or a German
    // interface reads an opacity of 0.5 as 0.
    const CNumbers c_numbers;
    const Json root(cJSON_ParseWithLength(static_cast<const char *>(bytes),
                                          static_cast<std::size_t>(size)));
    if (root == nullptr || !cJSON_IsObject(root.get())) {
        TraceLog(LOG_WARNING,
                 "MAP: [%s] is not an LDtk project: it is not a JSON object.", name);
        return { nullptr, &free_map };
    }
    const cJSON *defs = member(root.get(), "defs");
    const std::vector<LevelRef> levels = every_level(root.get());
    if (!cJSON_IsObject(defs) || levels.empty()) {
        TraceLog(LOG_WARNING,
                 "MAP: [%s] is not an LDtk project, or one with no levels: it has no "
                 "\"defs\" or no \"levels\".",
                 name);
        return { nullptr, &free_map };
    }

    // Written by an LDtk older than the format this reads. Read anyway, and
    // said once: most of it is the same, and saving it again fixes the rest.
    const char *version = text(root.get(), "jsonVersion");
    char *rest = nullptr;
    const long major = std::strtol(version, &rest, 10);
    const long minor =
        rest != version && *rest == '.' ? std::strtol(rest + 1, nullptr, 10) : 0;
    if (rest != version && (major < 1 || (major == 1 && minor < 5))) {
        RMP_REPORT_ONCE_KEYED(
            name,
            "MAP: [%s] was saved by LDtk %s, and this reads the format of "
            "LDtk 1.5 and later. Open it in a current LDtk and save it.",
            name, version);
    }

    const LevelRef *chosen = nullptr;
    for (const LevelRef &ref : levels) {
        if (level[0] == '\0' || std::string(text(ref.level, "identifier")) == level) {
            chosen = &ref;
            break;
        }
    }
    if (chosen == nullptr) {
        say_levels(levels, name, level);
        return { nullptr, &free_map };
    }

    auto data = std::make_unique<MapData>();
    Project project;
    project.name = name;
    project.layer_defs = array(defs, "layers");
    read_tilesets(defs, &project, data.get());

    const cJSON *lv = chosen->level;
    const bool world = has_world(chosen->layout);
    data->level = text(lv, "identifier");
    data->size_px = Vector2{ static_cast<float>(std::max(0, integer(lv, "pxWid", 0))),
                             static_cast<float>(std::max(0, integer(lv, "pxHei", 0))) };
    if (world) {
        data->origin = Vector2{ as_float(number(lv, "worldX", 0)),
                                as_float(number(lv, "worldY", 0)) };
        for (const LevelRef &ref : levels) {
            if (ref.world != chosen->world) continue;
            LevelInfo info;
            info.name = text(ref.level, "identifier");
            info.world = Rectangle{ as_float(number(ref.level, "worldX", 0)),
                                    as_float(number(ref.level, "worldY", 0)),
                                    as_float(number(ref.level, "pxWid", 0)),
                                    as_float(number(ref.level, "pxHei", 0)) };
            data->levels.push_back(std::move(info));
        }
    }

    Json external(nullptr);
    const cJSON *layers = array(lv, "layerInstances");
    if (layers == nullptr) layers = external_layers(lv, name, &external);
    if (layers == nullptr) {
        if (text(lv, "externalRelPath")[0] == '\0') {
            TraceLog(LOG_WARNING, "MAP: level \"%s\" of [%s] has no layers.",
                     data->level.c_str(), name);
        }
        return { nullptr, &free_map };
    }
    // Top first in the file; bottom first here.
    for (int i = cJSON_GetArraySize(layers) - 1; i >= 0; i--) {
        read_layer(cJSON_GetArrayItem(layers, i), &project, data.get());
    }

    // The map's own grid: the first layer with cells, or the project default.
    const int fallback = integer(root.get(), "defaultGridSize", 16);
    data->tile_width = fallback > 0 ? fallback : 16;
    for (const Layer &layer : data->layers) {
        if (layer.cell_width > 0) {
            data->tile_width = layer.cell_width;
            break;
        }
    }
    data->tile_height = data->tile_width;
    // In whole numbers, so a pxWid near INT_MAX cannot round up past it.
    const int w = std::max(0, integer(lv, "pxWid", 0));
    const int h = std::max(0, integer(lv, "pxHei", 0));
    data->width = (w / data->tile_width) + (w % data->tile_width != 0 ? 1 : 0);
    data->height = (h / data->tile_height) + (h % data->tile_height != 0 ? 1 : 0);

    load_used_textures(project, data.get());
    return { data.release(), &free_map };
}

} // namespace rmp::tilemap::detail
