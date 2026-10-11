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

#include <algorithm>
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
            "collision: a slanted push against a speed near FLT_MAX, whichever way the "
            "speed points, leaves a number",
            [] {
                const world::Fresh fresh;
                world::World scene;
                // Two discs touching on a slant, as small as a game's: only the
                // speed is past anything sensible. Straight into the contact,
                // the speed INTO it -- its two halves added -- is past what a
                // float holds; across it, that speed is a number and the
                // velocity left once it is taken away is not: (FLT_MAX,
                // -FLT_MAX) against a contact along (3, 4) keeps 1.12 FLT_MAX
                // of x.
                const float r = static_cast<float>(*rc::gen::inRange(2, 21));
                const float gap = static_cast<float>(*rc::gen::inRange(1, 10)) / 10;
                const std::pair<int, int> leg = *rc::gen::element(
                    std::pair{ 1, 1 }, std::pair{ 3, 4 }, std::pair{ 4, 3 },
                    std::pair{ 1, 2 }, std::pair{ 5, 12 });
                const float sx = static_cast<float>(*rc::gen::element(-1, 1));
                const float sy = static_cast<float>(*rc::gen::element(-1, 1));
                const float hyp = std::hypot(static_cast<float>(leg.first),
                                             static_cast<float>(leg.second));
                const Vector2 slant{ sx * static_cast<float>(leg.first) / hyp,
                                     sy * static_cast<float>(leg.second) / hyp };
                const float reach = 2 * r - gap;
                auto &still = scene.spawn<world::Tagged>(
                    { .position = { 0, 0 }, .shape = rmp::circle(r) });
                still.id = 0;
                still.solid = true;
                still.immovable = *rc::gen::arbitrary<bool>();
                const float fast =
                    *rc::gen::element(3e38F, std::numeric_limits<float>::max());
                const bool into_it = *rc::gen::arbitrary<bool>();
                const Vector2 velocity = into_it
                    ? Vector2{ -slant.x * fast, -slant.y * fast }
                    : Vector2{ *rc::gen::element(-fast, -fast / 2, 0.0F, fast / 2, fast),
                               *rc::gen::element(-fast, -fast / 2, 0.0F, fast / 2,
                                                 fast) };
                auto &mover = scene.spawn<world::Tagged>(
                    { .position = { slant.x * reach, slant.y * reach },
                      .shape = rmp::circle(r),
                      .velocity = velocity });
                mover.id = 1;
                mover.solid = true;
                const std::vector<std::pair<int, int>> touching{ { 0, 1 } };
                RC_ASSERT(world::pairs_of(scene, true) == touching);
                rmp::objects::detail::collide(scene);
                RC_ASSERT(world::finite(mover.velocity));
                RC_ASSERT(world::finite(still.velocity));
                RC_ASSERT(world::finite(mover.position));
            });
    }

    TEST_CASE("the narrow phase, against a reference in double") {
        const world::Hushed hushed;
        rmp::test::check(
            "collision: a solid shape pushed out of another moves by the shortest way "
            "out, and loses the speed it had into the contact",
            [] {
                const world::Fresh fresh;
                world::World scene;
                // In quarters, so the reference below meets the same numbers.
                const auto quarters = [](int lo, int hi) {
                    return static_cast<float>(*rc::gen::inRange(lo * 4, hi * 4)) / 4;
                };
                struct Shape {
                    bool round;
                    float hw, hh, r; // half sizes, radius
                };
                const auto draw_shape = [&] {
                    Shape s{ *rc::gen::arbitrary<bool>(), quarters(2, 40),
                             quarters(2, 40), quarters(2, 40) };
                    return s;
                };
                const Shape still_shape = draw_shape();
                const Shape mover_shape = draw_shape();
                const Vector2 offset{ quarters(-10, 10), quarters(-10, 10) };
                const Vector2 at{ quarters(-60, 60), quarters(-60, 60) };
                const Vector2 moving{ quarters(-100, 100), quarters(-100, 100) };
                const bool still_first = *rc::gen::arbitrary<bool>();
                const bool both_move = *rc::gen::arbitrary<bool>();

                // The reference: where the mover's shape is, and the push that
                // separates it from the still one, pointing at the mover.
                const double cx = at.x + offset.x;
                const double cy = at.y + offset.y;
                double push_x = 0;
                double push_y = 0;
                bool touching = false;
                // Ties between two ways out are left out: either is right.
                bool clear_cut = true;
                const auto rect_rect = [&](double ax, double ay, double ahw, double ahh,
                                           double bhw, double bhh) {
                    // a's push out of a box at the origin.
                    const double px = ahw + bhw - std::fabs(ax);
                    const double py = ahh + bhh - std::fabs(ay);
                    if (px <= 0 || py <= 0) return;
                    touching = true;
                    clear_cut = std::fabs(px - py) > 0.01 && ax != 0 && ay != 0;
                    if (px < py) {
                        push_x = ax < 0 ? -px : px;
                    } else {
                        push_y = ay < 0 ? -py : py;
                    }
                };
                const auto disc_rect = [&](double ax, double ay, double r, double bhw,
                                           double bhh, double sign) {
                    // A disc at (ax, ay) pushed out of a box at the origin;
                    // `sign` -1 turns it into the box's push out of the disc.
                    const double nx = std::clamp(ax, -bhw, bhw);
                    const double ny = std::clamp(ay, -bhh, bhh);
                    const double dx = ax - nx;
                    const double dy = ay - ny;
                    const double d = std::hypot(dx, dy);
                    if (d > 0.01) {
                        if (d >= r - 0.01) return;
                        touching = true;
                        push_x = sign * dx / d * (r - d);
                        push_y = sign * dy / d * (r - d);
                        return;
                    }
                    if (d > 0) {
                        clear_cut = false;
                        touching = true;
                        return;
                    }
                    touching = true;
                    const std::array<double, 4> ways{ ax + bhw, bhw - ax, ay + bhh,
                                                      bhh - ay };
                    std::array<double, 4> sorted = ways;
                    std::ranges::sort(sorted);
                    clear_cut = sorted[1] - sorted[0] > 0.01;
                    const auto best = static_cast<std::size_t>(
                        std::ranges::min_element(ways) - ways.begin());
                    const double out = ways[best] + r;
                    const std::array<Vector2, 4> dirs{
                        { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } }
                    };
                    push_x = sign * dirs[best].x * out;
                    push_y = sign * dirs[best].y * out;
                };
                if (!still_shape.round && !mover_shape.round) {
                    rect_rect(cx, cy, mover_shape.hw, mover_shape.hh, still_shape.hw,
                              still_shape.hh);
                } else if (still_shape.round && mover_shape.round) {
                    const double d = std::hypot(cx, cy);
                    const double sum = still_shape.r + mover_shape.r;
                    if (d < sum - 0.01) {
                        touching = true;
                        clear_cut = d > 0.01;
                        if (clear_cut) {
                            push_x = cx / d * (sum - d);
                            push_y = cy / d * (sum - d);
                        }
                    } else if (d < sum + 0.01) {
                        clear_cut = false;
                    }
                } else if (mover_shape.round) {
                    disc_rect(cx, cy, mover_shape.r, still_shape.hw, still_shape.hh, 1);
                } else {
                    // The still disc pushed out of the moving box, from the
                    // box's point of view, and turned round.
                    disc_rect(-cx, -cy, still_shape.r, mover_shape.hw, mover_shape.hh,
                              -1);
                }
                RC_PRE(clear_cut);

                const auto make = [&](const Shape &s, Vector2 position, Vector2 off,
                                      Vector2 v) -> rmp::Object & {
                    rmp::Shape shape =
                        s.round ? rmp::circle(s.r) : rmp::rect({ 2 * s.hw, 2 * s.hh });
                    shape.offset = off;
                    auto &o = scene.spawn(
                        { .position = position, .shape = shape, .velocity = v });
                    o.solid = true;
                    return o;
                };
                rmp::Handle<rmp::Object> made_still;
                if (still_first)
                    made_still = make(still_shape, { 0, 0 }, {}, {}).handle();
                const rmp::Handle<rmp::Object> made_mover =
                    make(mover_shape, at, offset, moving).handle();
                if (!still_first)
                    made_still = make(still_shape, { 0, 0 }, {}, {}).handle();
                rmp::Object &still = *made_still;
                const rmp::Object &mover = *made_mover;
                still.immovable = !both_move;

                rmp::objects::detail::collide(scene);

                const double share = both_move ? 0.5 : 1.0;
                const double tolerance =
                    1e-3 * (1 + std::fabs(push_x) + std::fabs(push_y));
                RC_ASSERT(std::fabs(mover.position.x - (at.x + share * push_x)) <=
                          tolerance);
                RC_ASSERT(std::fabs(mover.position.y - (at.y + share * push_y)) <=
                          tolerance);
                RC_ASSERT(std::fabs(still.position.x + ((1 - share) * push_x)) <=
                          tolerance);
                RC_ASSERT(std::fabs(still.position.y + ((1 - share) * push_y)) <=
                          tolerance);
                // The velocity into the contact is taken away, and nothing else.
                double vx = moving.x;
                double vy = moving.y;
                const double length = std::hypot(push_x, push_y);
                if (touching && length > 0) {
                    const double ox = push_x / length;
                    const double oy = push_y / length;
                    const double into = (vx * ox) + (vy * oy);
                    if (into < 0) {
                        vx -= ox * into;
                        vy -= oy * into;
                    }
                }
                RC_ASSERT(std::fabs(mover.velocity.x - vx) <= 1e-3 * (1 + std::fabs(vx)));
                RC_ASSERT(std::fabs(mover.velocity.y - vy) <= 1e-3 * (1 + std::fabs(vy)));
            });
        rmp::test::check(
            "collision: a ray aimed through a shape hits it, or misses it, where and as "
            "the documented reference does",
            [] {
                const world::Fresh fresh;
                world::World scene;
                const auto quarters = [](int lo, int hi) {
                    return static_cast<float>(*rc::gen::inRange(lo * 4, hi * 4)) / 4;
                };
                const bool round = *rc::gen::arbitrary<bool>();
                const float hw = quarters(2, 40);
                const float hh = quarters(2, 40);
                const float r = quarters(2, 40);
                const Vector2 c{ quarters(-50, 50), quarters(-50, 50) };
                scene.spawn(
                    { .position = c,
                      .shape = round ? rmp::circle(r) : rmp::rect({ 2 * hw, 2 * hh }) });
                // Through a point well inside the shape, from anywhere, and on
                // past it: a ray that hits, but for one that starts inside a
                // circle and ends inside it too. One time in three it starts
                // inside -- drawn from anywhere, a ray starts inside a disc
                // about one time in a hundred: none of a run's 200 cases did,
                // and at seed 11 the 470th was one that ended inside as well.
                const auto well_inside = [&] {
                    const float fx = static_cast<float>(*rc::gen::inRange(-8, 9)) / 10;
                    const float fy = static_cast<float>(*rc::gen::inRange(-8, 9)) / 10;
                    return round ? Vector2{ c.x + fx * r * 0.7F, c.y + fy * r * 0.7F }
                                 : Vector2{ c.x + fx * hw, c.y + fy * hh };
                };
                const Vector2 inside = well_inside();
                const Vector2 from = *rc::gen::inRange(0, 3) == 0
                    ? well_inside()
                    : Vector2{ quarters(-150, 150), quarters(-150, 150) };
                const Vector2 to{ from.x + (inside.x - from.x) * 3,
                                  from.y + (inside.y - from.y) * 3 };
                const double dx = to.x - from.x;
                const double dy = to.y - from.y;
                RC_PRE(std::hypot(dx, dy) > 1);

                // The reference, in double: where the segment meets the edge,
                // as rmp-docs' collision page says it does (and gates on
                // ray_circle()'s line): a ray that starts inside a box hits it
                // at once, with the normal back down the ray; one that starts
                // inside a circle hits it where it leaves, with the normal
                // outwards there -- and misses it when it ends inside too.
                double t = 0;
                double nx = 0;
                double ny = 0;
                bool starts_inside = false;
                bool misses = false;
                if (round) {
                    const double mx = from.x - c.x;
                    const double my = from.y - c.y;
                    const double a = (dx * dx) + (dy * dy);
                    const double b = 2 * ((mx * dx) + (my * dy));
                    const double k = (mx * mx) + (my * my) - (static_cast<double>(r) * r);
                    // Too close to the edge to say which side: either is right.
                    RC_PRE(std::fabs(k) > 0.05 * r);
                    starts_inside = k < 0;
                    const double root = std::sqrt((b * b) - (4 * a * k));
                    t = starts_inside ? (-b + root) / (2 * a) : (-b - root) / (2 * a);
                    // Leaving right at the end of the segment: either is right.
                    RC_PRE(std::fabs(t - 1) > 1e-3);
                    misses = t > 1;
                    nx = (from.x + dx * t - c.x) / r;
                    ny = (from.y + dy * t - c.y) / r;
                } else {
                    starts_inside =
                        std::fabs(from.x - c.x) < hw && std::fabs(from.y - c.y) < hh;
                    RC_PRE(std::fabs(std::fabs(from.x - c.x) - hw) > 0.05 &&
                           std::fabs(std::fabs(from.y - c.y) - hh) > 0.05);
                    if (starts_inside) {
                        const double len = std::hypot(dx, dy);
                        nx = -dx / len;
                        ny = -dy / len;
                    } else {
                        const double tx = dx > 0
                            ? (c.x - hw - from.x) / dx
                            : (dx < 0 ? (c.x + hw - from.x) / dx : -1e9);
                        const double ty = dy > 0
                            ? (c.y - hh - from.y) / dy
                            : (dy < 0 ? (c.y + hh - from.y) / dy : -1e9);
                        RC_PRE(std::fabs(tx - ty) > 1e-3);
                        if (tx > ty) {
                            t = tx;
                            nx = dx > 0 ? -1 : 1;
                        } else {
                            t = ty;
                            ny = dy > 0 ? -1 : 1;
                        }
                    }
                }
                const rmp::RayHit hit = scene.raycast(from, to);
                if (misses) {
                    RC_ASSERT(!hit);
                    return;
                }
                RC_ASSERT(static_cast<bool>(hit));
                const double tolerance = 1e-3 * (1 + std::hypot(dx, dy));
                RC_ASSERT(std::fabs(hit.point.x - (from.x + dx * t)) <= tolerance);
                RC_ASSERT(std::fabs(hit.point.y - (from.y + dy * t)) <= tolerance);
                RC_ASSERT(std::fabs(hit.distance - (t * std::hypot(dx, dy))) <=
                          tolerance);
                RC_ASSERT(std::fabs(hit.normal.x - nx) <= 1e-3);
                RC_ASSERT(std::fabs(hit.normal.y - ny) <= 1e-3);
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
    // covers: rmp::Scene::raycast(Vector2, Vector2) const
    // covers: rmp::Scene::raycast(const RayQuery &) const
    // covers: rmp::Scene::raycast_all(const RayQuery &, std::span<RayHit>) const
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
                // The two-point form is the query with nothing filtered.
                std::array<rmp::RayHit, 1> unfiltered{};
                const int any =
                    scene.raycast_all({ .from = query.from, .to = query.to }, unfiltered);
                const rmp::RayHit plain = scene.raycast(query.from, query.to);
                RC_ASSERT(static_cast<bool>(plain) == (any > 0));
                if (any > 0) RC_ASSERT(same(plain, unfiltered[0]));
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
