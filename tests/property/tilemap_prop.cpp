// ---------------------------------------------------------------------------
// rmp::Tilemap's queries and MapObject's properties, held to the campaign's
// rules for any map a reader can hand over and anything a game can ask:
//
//   - solid_in() is what its comment says, cell for cell: every cell a
//     rectangle COVERS, the right and bottom edges open, a rectangle with no
//     area asking about the cell its corner is in -- checked against a model
//     on maps whose every division is exact (map_gen.h's nice_grid());
//   - a rectangle inside another is never solid where the other is not, at
//     origins near float's end too (hostile_grid());
//   - solid_at(), solid_in() and tile_at() take anything: NaN, the
//     infinities, past int's range, negative sizes;
//   - a map moves, by construction and by assignment, and the one moved from
//     is the empty map, which answers zero, empty or false to everything;
//   - a property is read by its kind, and anything else is the fallback;
//   - draw() of any map with nothing loaded returns, and on_object() and
//     spawn_objects() make one thing per object, the last factory winning.
//
// The readers' own properties are tests/property/ldtk_prop.cpp's.
//
// covers: rmp::Tilemap::Tilemap()
// covers: rmp::Tilemap::Tilemap(Tilemap &&)
// covers: rmp::Tilemap::operator=(Tilemap &&)
// covers: rmp::Tilemap::~Tilemap()
// covers: rmp::Tilemap::valid() const
// covers: rmp::Tilemap::operator bool() const
// covers: rmp::Tilemap::bounds() const
// covers: rmp::Tilemap::tile_size() const
// covers: rmp::Tilemap::layer_count() const
// covers: rmp::Tilemap::object_count() const
// covers: rmp::Tilemap::level() const
// covers: rmp::Tilemap::solid_at(Vector2) const
// covers: rmp::Tilemap::solid_in(Rectangle) const
// covers: rmp::Tilemap::tile_at(int, int, int) const
// covers: rmp::Tilemap::draw() const
// covers: rmp::Tilemap::on_object(std::string_view, Callback<Scene &, const MapObject &>)
// covers: rmp::Tilemap::spawn_objects(Scene &)
// covers: rmp::MapObject::property_bool(std::string_view, bool) const
// covers: rmp::MapObject::property_int(std::string_view, int) const
// covers: rmp::MapObject::property_float(std::string_view, float) const
// covers: rmp::MapObject::property_string(std::string_view, std::string_view) const
// covers: rmp::MapObject::property_point(std::string_view, Vector2) const
// covers: rmp::MapObject::property_point(std::string_view, int, Vector2) const
// covers: rmp::MapObject::property_count(std::string_view) const
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"
#include "map_gen.h"

#include "../../src/rmp/object_internal.h"
#include "../../src/rmp/tilemap_internal.h"

#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/tilemap.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using rmp::test::maps::Box;
using rmp::test::maps::Grid;
using rmp::test::maps::GridLayer;
using rmp::test::maps::Point;
using Kind = rmp::tilemap::detail::Property::Kind;

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

// The cells of one axis a rectangle covers, by the comment on solid_in(), in
// whole quarter pixels so nothing is rounded: from the cell its start is in to
// the last cell its end passes into -- the end itself open -- or, with no
// extent, the cell its start is in.
struct Span {
    std::int64_t first = 0;
    std::int64_t last = -1;
};

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    return (a / b) - ((a % b != 0) && ((a < 0) != (b < 0)) ? 1 : 0);
}

Span span_of(float start, float extent, float base, int cell) {
    const auto quarters = [](double v) {
        return static_cast<std::int64_t>(std::llround(v * 4));
    };
    const std::int64_t from = quarters(start) - quarters(base);
    const std::int64_t size = quarters(extent);
    const std::int64_t unit = std::int64_t{ 4 } * cell;
    Span out;
    out.first = floor_div(from, unit);
    out.last = size == 0 ? out.first : floor_div(from + size + unit - 1, unit) - 1;
    out.last = std::max(out.first, out.last);
    return out;
}

// What solid_in() should say of a nice grid, cell by cell.
bool model_solid_in(const Grid &grid, Rectangle r) {
    for (const GridLayer &layer : grid.layers) {
        const Span columns =
            span_of(r.x, r.width, grid.origin_x + layer.offset_x, layer.cell_w);
        const Span rows =
            span_of(r.y, r.height, grid.origin_y + layer.offset_y, layer.cell_h);
        for (std::int64_t row = std::max<std::int64_t>(rows.first, 0);
             row <= std::min<std::int64_t>(rows.last, layer.height - 1); ++row) {
            for (std::int64_t column = std::max<std::int64_t>(columns.first, 0);
                 column <= std::min<std::int64_t>(columns.last, layer.width - 1);
                 ++column) {
                if (rmp::test::maps::solid_cell(layer, column, row)) return true;
            }
        }
    }
    return false;
}

// Whether `a` lies inside `b`, the way solid_in() reads a rectangle: an
// extent's far edge is the double sum of its start and its size, and an axis
// with no extent is the point at its start -- inside when it is inside b's
// half-open span, or on b's own point.
bool inside(Rectangle a, Rectangle b) {
    const auto axis = [](double a_at, double a_size, double b_at, double b_size) {
        if (a_at < b_at) return false;
        if (a_size > 0) return a_at + a_size <= b_at + b_size;
        return b_size > 0 ? a_at < b_at + b_size : a_at == b_at;
    };
    return a.width >= 0 && a.height >= 0 && b.width >= 0 && b.height >= 0 &&
        axis(a.x, a.width, b.x, b.width) && axis(a.y, a.height, b.y, b.height);
}

// A point anywhere: on the grid's quarter pixels, or one a game hands over by
// mistake.
rc::Gen<Point> point_near(float x, float y) {
    return rc::gen::exec([x, y] {
        const bool hostile = *rc::gen::inRange(0, 4) == 0;
        if (hostile) {
            return Point{ Vector2{ *rmp::test::gen::hostile_float(),
                                   *rmp::test::gen::hostile_float() } };
        }
        return Point{ Vector2{
            x + (static_cast<float>(*rc::gen::inRange(-640, 641)) / 4.0f),
            y + (static_cast<float>(*rc::gen::inRange(-640, 641)) / 4.0f) } };
    });
}

// ---- objects and their properties ----------------------------------------

struct PropertySpec {
    std::string key;
    Kind kind = Kind::STRING;
    bool boolean = false;
    int integer = 0;
    float floating = 0;
    std::string text;
    std::vector<Vector2> points;
};

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
void showValue(const PropertySpec &p, std::ostream &os) {
    os << "{";
    rc::show(p.key, os);
    os << " kind " << static_cast<int>(p.kind) << " " << p.boolean << " " << p.integer
       << " " << p.floating << " ";
    rc::show(p.text, os);
    os << " points " << p.points.size() << "}";
}

// A property as either reader keeps it: a float finite (the readers clamp),
// up to FLT_MAX -- which property_int() must still turn into an int.
rc::Gen<PropertySpec> property_spec() {
    return rc::gen::exec([] {
        PropertySpec out;
        out.key = *rc::gen::weightedOneOf<std::string>(
            { { 3, rc::gen::element<std::string>("hp", "speed", "name", "to", "") },
              { 1, rmp::test::gen::hostile_string() } });
        out.kind = *rc::gen::element(Kind::BOOL, Kind::INT, Kind::FLOAT, Kind::STRING,
                                     Kind::POINT);
        out.boolean = *rc::gen::arbitrary<bool>();
        out.integer = *rc::gen::arbitrary<int>();
        out.floating =
            rmp::tilemap::detail::finite_float(*rmp::test::gen::hostile_double());
        out.text = *rmp::test::gen::hostile_string();
        const int count = *rc::gen::inRange(0, 4);
        for (int i = 0; i < count; ++i) {
            out.points.push_back(Vector2{
                rmp::tilemap::detail::finite_float(*rmp::test::gen::hostile_double()),
                static_cast<float>(i) });
        }
        return out;
    });
}

// A map holding one object per entry, each with those properties, of a type
// from a small set so that factories meet several objects.
rmp::Tilemap map_of_objects(const std::vector<std::vector<PropertySpec>> &objects,
                            const std::vector<std::string> &types) {
    auto data = std::make_unique<rmp::tilemap::detail::MapData>();
    data->width = 2;
    data->height = 2;
    data->tile_width = 16;
    data->tile_height = 16;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        auto info = std::make_unique<rmp::tilemap::detail::ObjectInfo>();
        info->type = types[i % types.size()];
        info->solid_area = (i % 2) == 0;
        for (const PropertySpec &spec : objects[i]) {
            rmp::tilemap::detail::Property p;
            p.key = spec.key;
            p.kind = spec.kind;
            p.boolean = spec.boolean;
            p.integer = spec.integer;
            p.floating = spec.floating;
            p.text = spec.text;
            p.points = spec.points;
            info->properties.push_back(std::move(p));
        }
        rmp::MapObject &object = data->add_object(std::move(info));
        object.position = Vector2{ static_cast<float>(i) * 8, 4 };
        object.size = Vector2{ static_cast<float>(i % 3) * 8, 8 };
    }
    rmp::Tilemap map;
    rmp::tilemap::detail::Access::adopt(
        map,
        rmp::tilemap::detail::MapPtr(data.release(), &rmp::tilemap::detail::free_map));
    return map;
}

// The first property of an object by that key: what every lookup reads.
std::optional<PropertySpec> first_named(const std::vector<PropertySpec> &props,
                                        std::string_view key) {
    const auto found =
        std::ranges::find_if(props, [&](const PropertySpec &p) { return p.key == key; });
    if (found == props.end()) return std::nullopt;
    return *found;
}

int int_of(float value) {
    constexpr double LOWEST = std::numeric_limits<int>::min();
    constexpr double HIGHEST = std::numeric_limits<int>::max();
    return static_cast<int>(
        std::clamp(std::trunc(static_cast<double>(value)), LOWEST, HIGHEST));
}

bool same(Rectangle a, Rectangle b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// The empty map's answers.
void assert_empty(const rmp::Tilemap &map) {
    RC_ASSERT(!map.valid());
    RC_ASSERT(!static_cast<bool>(map));
    RC_ASSERT(same(map.bounds(), Rectangle{}));
    RC_ASSERT(map.tile_size().x == 0);
    RC_ASSERT(map.tile_size().y == 0);
    RC_ASSERT(map.layer_count() == 0);
    RC_ASSERT(map.object_count() == 0);
    RC_ASSERT(map.level().empty());
    RC_ASSERT(!map.solid_at(Vector2{}));
    RC_ASSERT(!map.solid_in(Rectangle{ -1e6f, -1e6f, 2e6f, 2e6f }));
    RC_ASSERT(map.tile_at(0, 0, 0) == 0);
    RC_ASSERT(map.neighbour_at(Vector2{}).empty());
}

} // namespace

namespace rmp::test::maps {
// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
inline void showValue(const GridLayer &layer, std::ostream &os) {
    showValue(Grid{ 0, 0, { layer } }, os);
}
} // namespace rmp::test::maps

TEST_SUITE("property: tilemap") {
    TEST_CASE("solid_in is every cell a rectangle covers, and only those") {
        rmp::test::check(
            "solid_in agrees with the cells a rectangle covers, on maps of exact numbers",
            [] {
                const Grid grid = *rmp::test::maps::nice_grid();
                const Box box = *rmp::test::maps::touching_box(grid);
                const rmp::Tilemap map = rmp::test::maps::to_map(grid);
                RC_ASSERT(map.solid_in(box.r) == model_solid_in(grid, box.r));
            });
    }

    TEST_CASE("a rectangle inside another is solid only where the other is") {
        rmp::test::check(
            "solid_in is monotone under rectangle inclusion, at any origin", [] {
                const Grid grid = *rmp::test::maps::hostile_grid();
                const Box outer = *rmp::test::maps::touching_box(grid);
                // A part of it: each edge moved in by a fraction of its size.
                const auto part = [](float at, float size, int from, int to) {
                    return std::pair{ at + (size * static_cast<float>(from) / 8.0f),
                                      size * static_cast<float>(to - from) / 8.0f };
                };
                const int x0 = *rc::gen::inRange(0, 9);
                const int x1 = *rc::gen::inRange(x0, 9);
                const int y0 = *rc::gen::inRange(0, 9);
                const int y1 = *rc::gen::inRange(y0, 9);
                const auto [x, w] = part(outer.r.x, outer.r.width, x0, x1);
                const auto [y, h] = part(outer.r.y, outer.r.height, y0, y1);
                const Rectangle inner{ x, y, w, h };
                RC_PRE(inside(inner, outer.r));
                const rmp::Tilemap map = rmp::test::maps::to_map(grid);
                if (map.solid_in(inner)) RC_ASSERT(map.solid_in(outer.r));
            });
    }

    TEST_CASE("a rectangle with no area asks about the cell its corner is in") {
        rmp::test::check(
            "solid_in of a rectangle with no area is solid_at of its corner", [] {
                const Grid grid = *rmp::test::maps::hostile_grid();
                const Vector2 p = *rc::gen::arbitrary<bool>()
                    ? (*rmp::test::maps::cell_point(grid)).p
                    : (*point_near(grid.origin_x, grid.origin_y)).p;
                const rmp::Tilemap map = rmp::test::maps::to_map(grid);
                RC_ASSERT(map.solid_in(Rectangle{ p.x, p.y, 0, 0 }) == map.solid_at(p));
            });
    }

    TEST_CASE("the queries take anything a game can hand them") {
        rmp::test::check(
            "solid_at, solid_in and tile_at take NaN, infinities and any int", [] {
                const Grid grid = *rmp::test::maps::hostile_grid();
                const rmp::Tilemap map = rmp::test::maps::to_map(grid);
                const Rectangle r = (*rmp::test::maps::hostile_box()).r;
                const bool in = map.solid_in(r);
                // Nothing is solid in a rectangle that is not one, and one
                // with an infinity or a NaN in it asks about no cell.
                const bool finite = std::isfinite(r.x) && std::isfinite(r.y) &&
                    std::isfinite(r.width) && std::isfinite(r.height);
                if (!finite || r.width < 0 || r.height < 0) RC_ASSERT(!in);
                const Vector2 p{ *rmp::test::gen::hostile_float(),
                                 *rmp::test::gen::hostile_float() };
                if (std::isnan(p.x) || std::isnan(p.y) || std::isinf(p.x) ||
                    std::isinf(p.y))
                    RC_ASSERT(!map.solid_at(p));
                const int layer = *rc::gen::arbitrary<int>();
                const int column = *rc::gen::arbitrary<int>();
                const int row = *rc::gen::arbitrary<int>();
                const int gid = map.tile_at(layer, column, row);
                if (layer < 0 || std::cmp_greater_equal(layer, grid.layers.size())) {
                    RC_ASSERT(gid == 0);
                } else {
                    const GridLayer &spec = grid.layers[static_cast<std::size_t>(layer)];
                    const bool on = column >= 0 && row >= 0 && column < spec.width &&
                        row < spec.height;
                    const int cell = on ? (row * spec.width) + column : 0;
                    RC_ASSERT(gid ==
                              (on ? spec.gids[static_cast<std::size_t>(cell)] : 0));
                }
            });
    }

    TEST_CASE("tile_at is each cell's tile on its own layer") {
        rmp::test::check(
            "tile_at reads every cell of every layer, and 0 one past each edge", [] {
                const Grid grid = *rmp::test::maps::nice_grid();
                const rmp::Tilemap map = rmp::test::maps::to_map(grid);
                for (std::size_t i = 0; i < grid.layers.size(); ++i) {
                    const GridLayer &layer = grid.layers[i];
                    const int at = static_cast<int>(i);
                    for (int row = -1; row <= layer.height; ++row) {
                        for (int column = -1; column <= layer.width; ++column) {
                            const bool on = column >= 0 && row >= 0 &&
                                column < layer.width && row < layer.height;
                            const int cell = on ? (row * layer.width) + column : 0;
                            const int want =
                                on ? layer.gids[static_cast<std::size_t>(cell)] : 0;
                            RC_ASSERT(map.tile_at(at, column, row) == want);
                        }
                    }
                }
            });
    }

    TEST_CASE("a map moves, and the one it moved from is empty") {
        rmp::test::check(
            "a map moved by construction or assignment answers as before, the source "
            "empty",
            [] {
                const Grid grid = *rmp::test::maps::hostile_grid();
                rmp::Tilemap first = rmp::test::maps::to_map(grid);
                const Rectangle bounds = first.bounds();
                const Vector2 tile = first.tile_size();
                const int layers = first.layer_count();
                const int objects = first.object_count();
                const std::string level(first.level());
                const Vector2 p = (*point_near(grid.origin_x, grid.origin_y)).p;
                const bool solid = first.solid_at(p);
                RC_ASSERT(first.valid());
                RC_ASSERT(static_cast<bool>(first));
                RC_ASSERT(layers == static_cast<int>(grid.layers.size()));
                RC_ASSERT(std::isfinite(bounds.width) && bounds.width >= 0);
                RC_ASSERT(std::isfinite(tile.x) && tile.x >= 0);

                rmp::Tilemap second(std::move(first));
                assert_empty(
                    first); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the promise
                rmp::Tilemap third;
                assert_empty(third);
                third = std::move(second);
                assert_empty(
                    second); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move): the promise
                RC_ASSERT(same(third.bounds(), bounds));
                RC_ASSERT(third.tile_size().x == tile.x);
                RC_ASSERT(third.layer_count() == layers);
                RC_ASSERT(third.object_count() == objects);
                RC_ASSERT(third.level() == level);
                RC_ASSERT(third.solid_at(p) == solid);
                // Assigned over a map it holds already: the old one goes.
                third = rmp::test::maps::to_map(grid);
                RC_ASSERT(third.layer_count() == layers);
                third = rmp::Tilemap();
                assert_empty(third);
            });
    }

    TEST_CASE("a property is read by its kind, and anything else is the fallback") {
        rmp::test::check(
            "every property lookup returns the property of its kind or the fallback", [] {
                const auto props = *rc::gen::container<std::vector<PropertySpec>>(
                    static_cast<std::size_t>(*rc::gen::inRange(0, 5)), property_spec());
                const rmp::Tilemap map = map_of_objects({ props }, { "thing" });
                const rmp::MapObject &object =
                    rmp::tilemap::detail::Access::data(map)->objects.front();
                // A key it has, or one it may not.
                const std::string key = props.empty() || *rc::gen::arbitrary<bool>()
                    ? *rmp::test::gen::hostile_string()
                    : props[static_cast<std::size_t>(*rc::gen::inRange<int>(
                                0, static_cast<int>(props.size())))]
                          .key;
                const int fallback_int = *rc::gen::arbitrary<int>();
                const float fallback_float = *rmp::test::gen::hostile_float();
                const bool fallback_bool = *rc::gen::arbitrary<bool>();
                const std::string fallback_text = *rmp::test::gen::hostile_string();
                const Vector2 fallback_point{ *rmp::test::gen::hostile_float(), 3 };
                const int index =
                    *rc::gen::oneOf(rc::gen::inRange(-2, 6), rc::gen::arbitrary<int>());

                const std::optional<PropertySpec> p = first_named(props, key);
                const Kind kind = p ? p->kind : Kind::STRING;
                const bool has = p.has_value();

                const int got_int = object.property_int(key, fallback_int);
                if (has && kind == Kind::INT) {
                    RC_ASSERT(got_int == p->integer);
                } else if (has && kind == Kind::FLOAT) {
                    RC_ASSERT(got_int == int_of(p->floating));
                } else {
                    RC_ASSERT(got_int == fallback_int);
                }

                const float got_float = object.property_float(key, fallback_float);
                if (has && kind == Kind::FLOAT) {
                    RC_ASSERT(got_float == p->floating);
                } else if (has && kind == Kind::INT) {
                    RC_ASSERT(got_float == static_cast<float>(p->integer));
                } else {
                    RC_ASSERT((got_float == fallback_float) ||
                              (std::isnan(got_float) && std::isnan(fallback_float)));
                }

                RC_ASSERT(object.property_bool(key, fallback_bool) ==
                          (has && kind == Kind::BOOL ? p->boolean : fallback_bool));

                const std::string_view text = object.property_string(key, fallback_text);
                RC_ASSERT(text ==
                          (has && kind == Kind::STRING
                               ? std::string_view(p->text)
                               : std::string_view(fallback_text)));

                const int count = object.property_count(key);
                RC_ASSERT(count ==
                          (!has
                               ? 0
                               : (kind == Kind::POINT ? static_cast<int>(p->points.size())
                                                      : 1)));
                const bool in_list = has && kind == Kind::POINT && index >= 0 &&
                    std::cmp_less(index, p->points.size());
                const Vector2 point = object.property_point(key, index, fallback_point);
                const Vector2 want =
                    in_list ? p->points[static_cast<std::size_t>(index)] : fallback_point;
                RC_ASSERT(point.y == want.y);
                RC_ASSERT(point.x == want.x ||
                          (std::isnan(point.x) && std::isnan(want.x)));
                const Vector2 first = object.property_point(key, fallback_point);
                const Vector2 first_want = object.property_point(key, 0, fallback_point);
                RC_ASSERT(first.y == first_want.y);
            });
    }

    TEST_CASE("draw with nothing loaded draws nothing, and changes nothing") {
        rmp::test::check("draw() of any map whose textures did not load returns", [] {
            const Grid grid = *rmp::test::maps::hostile_grid();
            // Layer opacities and tile alphas as the readers can leave them:
            // finite, up to FLT_MAX, below zero too.
            const float opacity =
                rmp::tilemap::detail::finite_float(*rmp::test::gen::hostile_double());
            const float alpha =
                rmp::tilemap::detail::finite_float(*rmp::test::gen::hostile_double());
            // Each layer drawn cell by cell, and each as LDtk's placed tiles.
            const rmp::Tilemap cells = rmp::test::maps::to_map(grid, opacity);
            const rmp::Tilemap placed = rmp::test::maps::to_map(grid, opacity, alpha);
            const int layers = cells.layer_count();
            cells.draw();
            placed.draw();
            RC_ASSERT(cells.layer_count() == layers);
            RC_ASSERT(placed.layer_count() == layers);
            const rmp::Tilemap empty;
            empty.draw();
            assert_empty(empty);
        });
    }

    TEST_CASE(
        "on_object and spawn_objects: one thing per object, the last factory wins") {
        rmp::test::check(
            "spawn_objects makes each object once, through the last factory of its type",
            [] {
                rmp::objects::detail::reset_for_tests();
                const int count = *rc::gen::inRange(0, 12);
                const std::vector<std::string> types{ "goblin", "coin", "door" };
                const std::vector<std::vector<PropertySpec>> objects(
                    static_cast<std::size_t>(count));
                rmp::Tilemap map = map_of_objects(objects, types);
                // Factories for some types, some registered twice, and some
                // for a type no object has.
                struct Calls {
                    std::vector<int> first;
                    std::vector<int> last;
                };
                Calls calls{ std::vector<int>(types.size() + 1, 0),
                             std::vector<int>(types.size() + 1, 0) };
                std::vector<int> registered(types.size() + 1, 0);
                const std::vector<std::string> keys{ "goblin", "coin", "door", "ghost" };
                const int registrations = *rc::gen::inRange(0, 7);
                for (int i = 0; i < registrations; ++i) {
                    const auto which = static_cast<std::size_t>(*rc::gen::inRange(0, 4));
                    const bool again = registered[which] > 0;
                    ++registered[which];
                    if (!again) {
                        map.on_object(
                            keys[which],
                            [&calls, which](rmp::Scene &, const rmp::MapObject &) {
                                ++calls.first[which];
                            });
                    } else {
                        map.on_object(
                            keys[which],
                            [&calls, which](rmp::Scene &, const rmp::MapObject &) {
                                ++calls.last[which];
                            });
                    }
                }
                // A key with a NUL inside is no type an object has.
                map.on_object(std::string_view("goblin\0x", 8),
                              [](rmp::Scene &, const rmp::MapObject &) {
                                  RC_FAIL("a NUL key matched");
                              });

                World world;
                map.spawn_objects(world);
                int plain = 0;
                for (std::size_t t = 0; t < types.size(); ++t) {
                    int of_type = 0;
                    for (int i = 0; i < count; ++i) {
                        if (static_cast<std::size_t>(i) % types.size() == t) ++of_type;
                    }
                    if (registered[t] == 0) {
                        plain += of_type;
                        RC_ASSERT(calls.first[t] + calls.last[t] == 0);
                        continue;
                    }
                    // The last registration made them all; a replaced one none.
                    RC_ASSERT(calls.first[t] == (registered[t] == 1 ? of_type : 0));
                    RC_ASSERT(calls.last[t] == (registered[t] == 1 ? 0 : of_type));
                }
                RC_ASSERT(calls.first[3] + calls.last[3] == 0);
                RC_ASSERT(world.object_count() == plain);
                // An empty map registers nothing and spawns nothing.
                rmp::Tilemap empty;
                empty.on_object("goblin", [](rmp::Scene &, const rmp::MapObject &) {
                    RC_FAIL("an empty map spawned");
                });
                empty.spawn_objects(world);
                RC_ASSERT(world.object_count() == plain);
                rmp::objects::detail::reset_for_tests();
            });
    }
}
