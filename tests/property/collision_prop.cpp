// ---------------------------------------------------------------------------
// The collision pass and the raycast, held over generated worlds: hundreds of
// objects that meet, and among them the ones a game breaks by accident -- at
// NaN, at an infinity, at 1e30, sized or scaled negative or not a number.
//
// The two differentials of tests/collision_test.cpp, with any world instead of
// one: the grid finds exactly the pairs the O(n^2) loop finds, and the grid's
// ray exactly the hits testing every object finds. Then what neither of those
// can see: a shape that is not a number touches nothing, a frame never makes
// a finite object infinite, and a fast mover does not pass through a wall.
//
// Each counterexample is a named case in tests/collision_test.cpp.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"
#include "object_gen.h"

#include "../../src/rmp/object_internal.h"

#include <rmp/object.h>
#include <rmp/scene.h>

#include <doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <set>
#include <utility>
#include <vector>

namespace {

namespace world = rmp::test::world;

// A delta a frame can have: the app clamps the real one, so nothing hostile.
rc::Gen<float> delta() {
    return rc::gen::element(0.0F, 1.0F / 240, 1.0F / 60, 1.0F / 30, 0.25F);
}

// A ray: two coordinates anywhere, or a short one from anywhere -- the kind
// that starts near FLT_MAX and is still a ray -- or one aimed at an object of
// the world, so that most rays hit something and the grid's walk has cells to
// get wrong: a ray into empty space agrees with every other about nothing.
//
// The targets are copied in: RapidCheck runs a generator again while it
// shrinks, after the case that drew from it has returned.
rc::Gen<rmp::RayQuery> ray(std::vector<rmp::Handle<world::Tagged>> targets) {
    return rc::gen::exec([targets = std::move(targets)] {
        rmp::RayQuery query;
        query.from = { *world::coordinate(), *world::coordinate() };
        const int how = *rc::gen::inRange(0, 3);
        if (how == 0) {
            query.to = { *world::coordinate(), *world::coordinate() };
        } else if (how == 1 || targets.empty()) {
            // Straight along an axis as often as not: a ray whose x does not
            // move multiplies an overflowed distance by 0.
            const rc::Gen<int> step = rc::gen::weightedOneOf<int>(
                { { 1, rc::gen::just(0) }, { 2, rc::gen::inRange(-50, 51) } });
            query.to = { query.from.x + static_cast<float>(*step),
                         query.from.y + static_cast<float>(*step) };
        } else {
            const auto &target =
                targets[*rc::gen::inRange<std::size_t>(0, targets.size())];
            // Short of it, at it, or through it and out the far side.
            const float reach = static_cast<float>(*rc::gen::element(1, 2, 2, 4)) / 2;
            query.from = { *world::coordinate(false), *world::coordinate(false) };
            query.to = { query.from.x + (target->position.x - query.from.x) * reach,
                         query.from.y + (target->position.y - query.from.y) * reach };
        }
        query.mask = *rc::gen::element(0xFFFFFFFFU, 1U, 2U);
        query.solid_only = *rc::gen::arbitrary<bool>();
        return query;
    });
}

// Everything a hit says, for comparing two of them to the bit.
bool same(const rmp::RayHit &a, const rmp::RayHit &b) {
    return a.object == b.object && a.point.x == b.point.x && a.point.y == b.point.y &&
        a.normal.x == b.normal.x && a.normal.y == b.normal.y && a.distance == b.distance;
}

constexpr std::size_t MOST_HITS = 64;

} // namespace

TEST_SUITE("property: collision") {
    // covers: rmp::Object::world_collider() const
    TEST_CASE("the grid is the O(n^2) loop, in any world") {
        const world::Hushed hushed;
        rmp::test::check(
            "collision: the grid finds exactly the pairs the O(n^2) loop finds", [] {
                const world::Fresh fresh;
                world::World scene;
                world::spawn_all(scene, *world::things(40));
                rmp::objects::detail::update(scene, *delta());
                RC_ASSERT(world::pairs_of(scene, true) == world::pairs_of(scene, false));
            });
        rmp::test::check(
            "collision: a pair is two live objects whose layers meet, each pair once",
            [] {
                const world::Fresh fresh;
                world::World scene;
                const auto handles = world::spawn_all(scene, *world::things(40));
                rmp::objects::detail::update(scene, *delta());
                const auto pairs = world::pairs_of(scene, true);
                const std::set<std::pair<int, int>> unique(pairs.begin(), pairs.end());
                RC_ASSERT(unique.size() == pairs.size());
                for (const auto &[a, b] : pairs) {
                    RC_ASSERT(a != b);
                    const auto &one = *handles[static_cast<std::size_t>(a)];
                    const auto &two = *handles[static_cast<std::size_t>(b)];
                    RC_ASSERT(((one.collision_mask & two.collision_layer) != 0 ||
                               (two.collision_mask & one.collision_layer) != 0));
                }
            });
        rmp::test::check(
            "collision: a shape that is not a finite number touches nothing", [] {
                const world::Fresh fresh;
                world::World scene;
                const auto handles = world::spawn_all(scene, *world::things(40));
                rmp::objects::detail::update(scene, *delta());
                for (const auto &[a, b] : world::pairs_of(scene, true)) {
                    RC_ASSERT(world::finite(
                        handles[static_cast<std::size_t>(a)]->world_collider()));
                    RC_ASSERT(world::finite(
                        handles[static_cast<std::size_t>(b)]->world_collider()));
                }
            });
    }

    TEST_CASE("a frame keeps what is finite finite") {
        const world::Hushed hushed;
        rmp::test::check(
            "collision: the collision pass never makes a finite position or velocity "
            "infinite",
            [] {
                const world::Fresh fresh;
                world::World scene;
                const auto handles = world::spawn_all(scene, *world::things(30));
                rmp::objects::detail::update(scene, *delta());
                std::vector<bool> was(handles.size());
                for (std::size_t i = 0; i < handles.size(); ++i) {
                    was[i] = world::finite(handles[i]->position) &&
                        world::finite(handles[i]->velocity);
                }
                rmp::objects::detail::collide(scene);
                for (std::size_t i = 0; i < handles.size(); ++i) {
                    if (!was[i]) continue;
                    RC_ASSERT(world::finite(handles[i]->position));
                    RC_ASSERT(world::finite(handles[i]->velocity));
                }
            });
    }

    TEST_CASE("two shapes near the top of the floats") {
        const world::Hushed hushed;
        rmp::test::check(
            "collision: a shape pushed out of a wall at the edge of the floats stays a "
            "number, and so does its speed",
            [] {
                const world::Fresh fresh;
                world::World scene;
                const float top = std::numeric_limits<float>::max();
                // The wall: immovable, its right edge at FLT_MAX.
                const float wall_half = *rc::gen::element(5e37F, 1e38F, 1.5e38F);
                const bool wall_round = *rc::gen::arbitrary<bool>();
                auto &wall = scene.spawn<world::Tagged>(
                    { .position = { top - wall_half, 0 },
                      .shape = wall_round
                          ? rmp::circle(wall_half)
                          : rmp::rect({ 2 * wall_half, 2 * wall_half }) });
                wall.id = 0;
                wall.solid = true;
                wall.immovable = true;
                // The mover: overlapping it, somewhere across it, as fast as a
                // float goes in any direction.
                const float half = *rc::gen::element(5e37F, 1e38F, 1.5e38F);
                const bool round = *rc::gen::arbitrary<bool>();
                const float across =
                    *rc::gen::element(-0.75F, -0.5F, -0.25F, 0.0F, 0.25F, 0.5F, 0.75F);
                const float up = *rc::gen::element(-0.5F, -0.25F, 0.0F, 0.25F, 0.5F);
                // Half the time straight at the wall's centre, on both axes: the
                // speed INTO a slanted contact is then more than a float holds.
                const bool at_it = *rc::gen::arbitrary<bool>();
                const Vector2 velocity = at_it
                    ? Vector2{ across > 0 ? -3e38F : 3e38F, up > 0 ? -3e38F : 3e38F }
                    : Vector2{ *rc::gen::element(-3e38F, 0.0F, 3e38F),
                               *rc::gen::element(-3e38F, 0.0F, 3e38F) };
                auto &mover = scene.spawn<world::Tagged>(
                    { .position = { wall.position.x + across * wall_half,
                                    up * wall_half },
                      .shape =
                          round ? rmp::circle(half) : rmp::rect({ 2 * half, 2 * half }),
                      .velocity = velocity });
                mover.id = 1;
                mover.solid = true;
                RC_PRE(world::finite(wall.world_collider()) &&
                       world::finite(mover.world_collider()));
                rmp::objects::detail::collide(scene);
                RC_ASSERT(world::finite(mover.position));
                RC_ASSERT(world::finite(mover.velocity));
                RC_ASSERT(wall.position.x == top - wall_half);
            });
    }

    TEST_CASE("a push against a speed near FLT_MAX") {
        const world::Hushed hushed;
        rmp::test::check(
            "collision: a slanted push takes away a speed near FLT_MAX and leaves a "
            "number",
            [] {
                const world::Fresh fresh;
                world::World scene;
                // Two discs touching on a slant, as small as a game's: only the
                // speed is past anything sensible, and the speed INTO the
                // contact -- its two halves added -- is past what a float holds.
                const float r = static_cast<float>(*rc::gen::inRange(2, 21));
                const float gap = static_cast<float>(*rc::gen::inRange(1, 10)) / 10;
                const Vector2 slant{ static_cast<float>(*rc::gen::element(-1, 1)),
                                     static_cast<float>(*rc::gen::element(-1, 1)) };
                const float reach = (2 * r - gap) / std::numbers::sqrt2_v<float>;
                auto &still = scene.spawn<world::Tagged>(
                    { .position = { 0, 0 }, .shape = rmp::circle(r) });
                still.id = 0;
                still.solid = true;
                still.immovable = *rc::gen::arbitrary<bool>();
                const float fast =
                    *rc::gen::element(3e38F, std::numeric_limits<float>::max());
                auto &mover = scene.spawn<world::Tagged>(
                    { .position = { slant.x * reach, slant.y * reach },
                      .shape = rmp::circle(r),
                      .velocity = { -slant.x * fast, -slant.y * fast } });
                mover.id = 1;
                mover.solid = true;
                rmp::objects::detail::collide(scene);
                RC_ASSERT(world::finite(mover.velocity));
                RC_ASSERT(world::finite(still.velocity));
                RC_ASSERT(world::finite(mover.position));
            });
    }

    TEST_CASE("a fast mover does not pass through a wall") {
        const world::Hushed hushed;
        rmp::test::check(
            "collision: a mover that crosses a wall in one frame is told it hit it", [] {
                const world::Fresh fresh;
                world::World scene;
                const float wall_width = static_cast<float>(*rc::gen::inRange(1, 41));
                const float wall_height = static_cast<float>(*rc::gen::inRange(20, 201));
                const bool round = *rc::gen::arbitrary<bool>();
                const float size = static_cast<float>(*rc::gen::inRange(1, 21));
                const float before = static_cast<float>(*rc::gen::inRange(1, 101));
                const float after = static_cast<float>(*rc::gen::inRange(1, 1001));
                const float room = (wall_height - size) / 2 - 1;
                RC_PRE(room > 0);
                const float y =
                    static_cast<float>(*rc::gen::inRange(-100, 101)) / 100 * room;
                const bool solid = *rc::gen::arbitrary<bool>();

                auto &wall = scene.spawn<world::Tagged>(
                    { .position = { 0, 0 },
                      .shape = rmp::rect({ wall_width, wall_height }) });
                wall.id = 0;
                wall.solid = solid;
                wall.immovable = true;
                const float from = -(wall_width / 2 + size / 2 + before);
                const float to = wall_width / 2 + size / 2 + after;
                constexpr float STEP = 1.0F / 60;
                auto &mover = scene.spawn<world::Tagged>(
                    { .position = { from, y },
                      .shape = round ? rmp::circle(size / 2) : rmp::rect({ size, size }),
                      .velocity = { (to - from) / STEP, 0 } });
                mover.id = 1;
                mover.solid = solid;

                rmp::objects::detail::update(scene, STEP);
                RC_ASSERT(mover.position.x > wall_width / 2 + size / 2);
                const std::vector<std::pair<int, int>> hit{ { 0, 1 } };
                RC_ASSERT(world::pairs_of(scene, true) == hit);
                rmp::objects::detail::collide(scene);
                // Both solid: put back where it met the wall, not past it.
                if (solid)
                    RC_ASSERT(mover.position.x <= -(wall_width / 2 + size / 2) + 0.01F);
            });
    }

    // covers: rmp::RayHit::operator bool() const
    TEST_CASE("the grid's ray is every object's ray, for any ray") {
        const world::Hushed hushed;
        rmp::test::check(
            "collision: the grid's raycast finds exactly what testing every object finds",
            [] {
                const world::Fresh fresh;
                world::World scene;
                const auto handles = world::spawn_all(scene, *world::things(40));
                rmp::objects::detail::update(scene, *delta());
                const rmp::RayQuery query = *ray(handles);
                std::array<rmp::RayHit, MOST_HITS> grid{};
                std::array<rmp::RayHit, MOST_HITS> every{};
                const int found =
                    rmp::objects::detail::raycast_for_tests(scene, query, true, grid);
                const int expected =
                    rmp::objects::detail::raycast_for_tests(scene, query, false, every);
                RC_ASSERT(found == expected);
                for (int i = 0; i < found; ++i) {
                    RC_ASSERT(same(grid[static_cast<std::size_t>(i)],
                                   every[static_cast<std::size_t>(i)]));
                }
                const rmp::RayHit first = scene.raycast(query);
                RC_ASSERT(static_cast<bool>(first) == (found > 0));
                if (found > 0) RC_ASSERT(same(first, grid[0]));
            });
        rmp::test::check(
            "collision: a hit is a finite point on the ray, nearest first, on an object "
            "the query lets through",
            [] {
                const world::Fresh fresh;
                world::World scene;
                const auto handles = world::spawn_all(scene, *world::things(40));
                rmp::objects::detail::update(scene, *delta());
                const std::size_t ignored =
                    *rc::gen::inRange<std::size_t>(0, handles.size() + 1);
                rmp::RayQuery query = *ray(handles);
                if (ignored < handles.size()) query.ignore = handles[ignored]->handle();
                std::array<rmp::RayHit, MOST_HITS> hits{};
                const int found = scene.raycast_all(query, hits);
                float last = 0;
                for (int i = 0; i < found; ++i) {
                    const rmp::RayHit &hit = hits[static_cast<std::size_t>(i)];
                    RC_ASSERT(static_cast<bool>(hit));
                    RC_ASSERT(hit.object != query.ignore);
                    RC_ASSERT((hit.object->collision_layer & query.mask) != 0);
                    if (query.solid_only) RC_ASSERT(hit.object->solid);
                    RC_ASSERT(world::finite(hit.object->world_collider()));
                    RC_ASSERT(world::finite(hit.point));
                    RC_ASSERT(world::finite(hit.normal));
                    RC_ASSERT(std::isfinite(hit.distance));
                    RC_ASSERT(hit.distance >= last);
                    last = hit.distance;
                    const float length =
                        std::hypot(query.to.x - query.from.x, query.to.y - query.from.y);
                    RC_ASSERT(hit.distance <= (length * 1.0001F) + 0.001F);
                }
            });
    }
}
