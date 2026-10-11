#pragma once
// ---------------------------------------------------------------------------
// Worlds of objects for the properties of rmp::Object and its collision pass
// (tests/property/object_prop.cpp, tests/property/collision_prop.cpp).
//
//   rmp::test::world::Fresh           the object storage emptied before a
//                                     case and after it, however it ends
//   rmp::test::world::World           a scene no stack knows: spawn() only
//                                     needs `this` as a key
//   rmp::test::world::Tagged          an object that knows which one it is
//   rmp::test::world::thing()         one object, as spawn() takes it: a box,
//                                     a disc or a point, anywhere -- sometimes
//                                     at NaN, an infinity or 1e30, with a size
//                                     or scale that is negative or not a number
//   rmp::test::world::things()        a world of them, ids in creation order
//
// Ordinary values most of the time, so the objects meet each other and the
// grid has something to do; gen.h's hostile ones the rest, so every property
// also sees what a division by zero in a game hands the framework.
// ---------------------------------------------------------------------------

#include "../../src/rmp/object_internal.h"
#include "gen.h"

#include <rmp/object.h>
#include <rmp/scene.h>

#include <rapidcheck.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <ostream>
#include <string_view>
#include <utility>
#include <vector>

namespace rmp::test::world {

// The storage is global: a case starts from nothing, and leaves nothing behind
// for the next -- RC_ASSERT throws, so this has to be a destructor.
struct Fresh {
    Fresh() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_pointer_for_tests();
    }
    Fresh(const Fresh &) = delete;
    Fresh &operator=(const Fresh &) = delete;
    Fresh(Fresh &&) = delete;
    Fresh &operator=(Fresh &&) = delete;
    ~Fresh() { rmp::objects::detail::reset_for_tests(); }
};

class World : public rmp::Scene {
public:
    World() = default;
    World(const World &) = delete;
    World &operator=(const World &) = delete;
    World(World &&) = delete;
    World &operator=(World &&) = delete;
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

class Tagged : public rmp::Object {
public:
    int id = -1;
};

inline bool finite(Vector2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }
inline bool finite(const Rectangle &r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
        std::isfinite(r.height);
}

// Finite, and near the top of what a float holds: where a sum of two of them
// is an infinity. gen.h's FLT_MAX is past it -- a shape there has no edges a
// float can say -- and its 1e30 is too far below.
inline rc::Gen<float> near_the_top() {
    const float top = std::numeric_limits<float>::max();
    return rc::gen::element(1e38F, -1e38F, 1.5e38F, -1.5e38F, 3e38F, -3e38F, top / 2,
                            -top / 2);
}

// An ordinary value, or -- in a hostile world -- now and then one of gen.h's
// hostile floats or one near the top.
inline rc::Gen<float> sometimes_hostile(const rc::Gen<float> &ordinary, bool hostile) {
    if (!hostile) return ordinary;
    return rc::gen::weightedOneOf<float>({
        { 8, ordinary },
        { 2, rmp::test::gen::hostile_float() },
        { 1, near_the_top() },
    });
}

// A coordinate: mostly a few hundred units either way, in tenths.
inline rc::Gen<float> coordinate(bool hostile = true) {
    return sometimes_hostile(
        rc::gen::map(rc::gen::inRange(-4000, 4001),
                     [](int tenths) { return static_cast<float>(tenths) / 10.0F; }),
        hostile);
}

// A size: mostly one to eighty units.
inline rc::Gen<float> extent(bool hostile = true) {
    return sometimes_hostile(
        rc::gen::map(rc::gen::inRange(1, 81),
                     [](int units) { return static_cast<float>(units); }),
        hostile);
}

// A scale: mostly 1, sometimes mirrored, halved, doubled or 0.
inline rc::Gen<float> factor(bool hostile = true) {
    return sometimes_hostile(rc::gen::weightedOneOf<float>({
                                 { 6, rc::gen::just(1.0F) },
                                 { 2, rc::gen::element(-1.0F, 0.5F, 2.0F, -2.0F, 0.0F) },
                             }),
                             hostile);
}

// A speed: up to a few thousand units a second, fast enough to cross an
// object in one frame.
inline rc::Gen<float> speed(bool hostile = true) {
    return sometimes_hostile(
        rc::gen::map(rc::gen::inRange(-3000, 3001),
                     [](int units) { return static_cast<float>(units); }),
        hostile);
}

enum class Kind { BOX, DISC, POINT, DISC_IN_BOX };

// One object, as the properties spawn it.
struct Thing {
    Kind kind = Kind::BOX;
    Vector2 position{};
    Vector2 size{};
    float radius = 0;
    Vector2 offset{};
    Vector2 scale{ 1, 1 };
    Vector2 velocity{};
    unsigned layer = 1;
    unsigned mask = 1;
    bool solid = false;
    bool immovable = false;
};

inline rc::Gen<Thing> thing(bool hostile = true) {
    return rc::gen::exec([hostile] {
        Thing t;
        t.kind = *rc::gen::element(Kind::BOX, Kind::DISC, Kind::POINT, Kind::DISC_IN_BOX);
        t.position = Vector2{ *coordinate(hostile), *coordinate(hostile) };
        t.size = Vector2{ *extent(hostile), *extent(hostile) };
        t.radius = *extent(hostile);
        if (*rc::gen::inRange(0, 4) == 0)
            t.offset = Vector2{ *coordinate(hostile) / 4, *coordinate(hostile) / 4 };
        t.scale = Vector2{ *factor(hostile), *factor(hostile) };
        t.velocity = Vector2{ *speed(hostile), *speed(hostile) };
        t.layer = *rc::gen::element(1U, 2U, 3U, 0U, 0xFFFFFFFFU);
        t.mask = *rc::gen::element(1U, 2U, 3U, 0U, 0xFFFFFFFFU);
        t.solid = *rc::gen::arbitrary<bool>();
        t.immovable = *rc::gen::arbitrary<bool>();
        return t;
    });
}

// How RapidCheck shows one in a counterexample.
// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
inline void showValue(const Thing &t, std::ostream &os) {
    constexpr std::array<std::string_view, 4> KINDS{ "box", "disc", "point",
                                                     "disc-in-box" };
    os << "{" << KINDS[static_cast<std::size_t>(t.kind)] << " at (" << t.position.x
       << ", " << t.position.y << ") size (" << t.size.x << ", " << t.size.y
       << ") radius " << t.radius << " offset (" << t.offset.x << ", " << t.offset.y
       << ") scale (" << t.scale.x << ", " << t.scale.y << ") velocity (" << t.velocity.x
       << ", " << t.velocity.y << ") layer " << t.layer << " mask " << t.mask
       << (t.solid ? " solid" : "") << (t.immovable ? " immovable" : "") << "}";
}

// Half of them ordinary worlds, where the grid has cells to walk and pairs to
// find; half hostile, where one object at 1e30 makes the cells enormous and
// one at NaN has to be nowhere. Mixing the two in every world would leave the
// grid's walk untested: one far object, and every object shares one cell.
inline rc::Gen<std::vector<Thing>> things(int most) {
    return rc::gen::exec([most] {
        const bool hostile = *rc::gen::arbitrary<bool>();
        const int count = *rc::gen::inRange(0, most + 1);
        std::vector<Thing> out;
        out.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) out.push_back(*thing(hostile));
        return out;
    });
}

inline Tagged &spawn(World &world, const Thing &t, int id) {
    rmp::Shape shape;
    if (t.kind == Kind::BOX || t.kind == Kind::DISC_IN_BOX) shape = rmp::rect(t.size);
    if (t.kind == Kind::DISC) shape = rmp::circle(t.radius);
    shape.offset = t.offset;
    auto &made = world.spawn<Tagged>({ .position = t.position,
                                       .shape = shape,
                                       .velocity = t.velocity,
                                       .scale = t.scale });
    if (t.kind == Kind::DISC_IN_BOX) made.collider = rmp::circle(t.radius);
    made.id = id;
    made.collision_layer = t.layer;
    made.collision_mask = t.mask;
    made.solid = t.solid;
    made.immovable = t.immovable;
    return made;
}

// The world, spawned in order: ids 0, 1, 2... and the handles, in that order.
inline std::vector<rmp::Handle<Tagged>> spawn_all(World &world,
                                                  const std::vector<Thing> &all) {
    std::vector<rmp::Handle<Tagged>> out;
    out.reserve(all.size());
    for (std::size_t i = 0; i < all.size(); ++i)
        out.push_back(spawn(world, all[i], static_cast<int>(i)).handle<Tagged>());
    return out;
}

inline int id_of(const rmp::Object &object) {
    return static_cast<const Tagged &>(object).id;
}

// The touching pairs, as ids, each (lower, higher), sorted: with the grid, or
// with the O(n^2) loop it is an optimisation of.
inline std::vector<std::pair<int, int>> pairs_of(const rmp::Scene &scene, bool use_grid) {
    constexpr std::size_t CAPACITY = 4096;
    // NOLINTNEXTLINE(custom-rmp-raw-pointer): touching_pairs_for_tests' out array
    std::vector<rmp::Object *> raw(CAPACITY * 2, nullptr);
    const int n = rmp::objects::detail::touching_pairs_for_tests(
        scene, use_grid, raw.data(), static_cast<int>(CAPACITY));
    std::vector<std::pair<int, int>> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int a = id_of(*raw[static_cast<std::size_t>(i) * 2]);
        const int b = id_of(*raw[(static_cast<std::size_t>(i) * 2) + 1]);
        out.emplace_back(a < b ? a : b, a < b ? b : a);
    }
    std::ranges::sort(out);
    return out;
}

} // namespace rmp::test::world

namespace rmp {

// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
inline void showValue(Edge edge, std::ostream &os) {
    constexpr std::array<std::string_view, 5> NAMES{ "NONE", "CLAMP", "BOUNCE", "WRAP",
                                                     "DESTROY" };
    os << "Edge::" << NAMES[static_cast<std::size_t>(edge)];
}

} // namespace rmp

// raylib's, so RapidCheck looks for it where raylib's types are.
// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
inline void showValue(const Rectangle &r, std::ostream &os) {
    os << "{" << r.x << ", " << r.y << ", " << r.width << ", " << r.height << "}";
}
