// ---------------------------------------------------------------------------
// Arcade collision: shapes, a uniform grid, solid separation, the swept test
// against tunnelling, raycasting, and the pointer.
//
// WHY THIS IS OURS AND NOT BOX2D, because it is the right question. Box2D 3.x
// is plain C with CMake and no dependencies and would compile on all seventeen
// targets without a fight. The problem is not portability, it is that Box2D is
// not a collision library -- it is a model of the world. Bodies, fixtures,
// density, an iterative solver, its own fixed Step, and its own units (metres,
// not pixels, which is the classic first mistake). Adopting it means
// rmp::Object::position stops being where the position lives and becomes a
// mirror of a b2Body transform, and then everything else has to be re-expressed
// in its terms: Edge::CLAMP stops being a comparison and becomes a static body,
// on_drag stops writing position and becomes a mouse joint, velocity stops
// being a field.
//
// And the simple case gets harder, which is the rule that outranks the others
// answering by itself: a Pong ball goes from one line to a dynamic body, a
// circle shape, density, restitution 1, friction 0, linear damping 0 and
// gravity disabled on that body.
//
// So: arcade collision, about six hundred lines, tested without a GPU. The door
// to Box2D stays open in the only way that matters -- position, rotation and
// velocity are public fields, so a game that genuinely needs rigid bodies (a
// tower that collapses, a car, a ragdoll) can link Box2D, Step it in _update
// and write position from the body. rmp::Object does not stand in the way, and
// when a real game asks, it enters as an example and not as a layer.
//
// WHAT IS DELIBERATELY ABSENT: rotation in collision, polygons, friction,
// restitution, joints, and anything else that is the beginning of a physics
// engine.
// ---------------------------------------------------------------------------

#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>

#include "object_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace rmp {

namespace {

constexpr float kEpsilon = 0.0001f;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A shape placed in the world. The collision pass works with this and not with
// the AABB, which is what makes a circle collide as a circle -- the difference
// shows the moment a Pong ball meets the end of a paddle, because the bounce
// normal is the one of the side it actually hit rather than an approximation.
struct Placed {
    ShapeKind kind = ShapeKind::NONE;
    Vector2 centre{};
    Vector2 half{}; // RECTANGLE
    float radius = 0; // CIRCLE
};

Placed placed_of(const Object &object) {
    Placed p;
    const Shape &shape =
        object.collider.kind != ShapeKind::NONE ? object.collider : object.shape;

    if (shape.kind == ShapeKind::CIRCLE) {
        p.kind = ShapeKind::CIRCLE;
        // One axis, because a circle scaled unevenly is an ellipse and this
        // layer does not have ellipses. x is the one that wins, and the header
        // says the collider is a circle, so nothing is being hidden.
        p.radius = shape.radius * object.scale.x;
        p.centre = Vector2{ object.position.x + shape.offset.x,
                            object.position.y + shape.offset.y };
        return p;
    }
    if (shape.kind == ShapeKind::RECTANGLE) {
        p.kind = ShapeKind::RECTANGLE;
        p.half = Vector2{ shape.size.x * object.scale.x / 2,
                          shape.size.y * object.scale.y / 2 };
        p.centre = Vector2{ object.position.x + shape.offset.x,
                            object.position.y + shape.offset.y };
        return p;
    }

    // No shape and no collider: fall back to the sprite's frame, which is what
    // world_bounds() already works out. An object with neither is a point, and
    // a point collides with nothing -- it has no area, and saying it collides
    // with whatever it is inside would make every logic object a trigger.
    const Rectangle box = object.world_bounds();
    if (box.width <= 0 || box.height <= 0) return p; // kind stays NONE
    p.kind = ShapeKind::RECTANGLE;
    p.half = Vector2{ box.width / 2, box.height / 2 };
    p.centre = Vector2{ box.x + box.width / 2, box.y + box.height / 2 };
    return p;
}

Rectangle aabb_of(const Placed &p) {
    if (p.kind == ShapeKind::CIRCLE) {
        return Rectangle{ p.centre.x - p.radius, p.centre.y - p.radius, p.radius * 2,
                          p.radius * 2 };
    }
    return Rectangle{ p.centre.x - p.half.x, p.centre.y - p.half.y, p.half.x * 2,
                      p.half.y * 2 };
}

// The union of two boxes: a moving object's start and end, for the broad phase.
Rectangle merge(const Rectangle &a, const Rectangle &b) {
    const float x0 = a.x < b.x ? a.x : b.x;
    const float y0 = a.y < b.y ? a.y : b.y;
    const float x1 = (a.x + a.width) > (b.x + b.width) ? a.x + a.width : b.x + b.width;
    const float y1 =
        (a.y + a.height) > (b.y + b.height) ? a.y + a.height : b.y + b.height;
    return Rectangle{ x0, y0, x1 - x0, y1 - y0 };
}

bool boxes_overlap(const Rectangle &a, const Rectangle &b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
        b.y < a.y + a.height;
}

// ---------------------------------------------------------------------------
// The narrow phase. Three pairs and no more, and that is the whole reason there
// are two shapes and not five: these are the three that can be resolved
// honestly. A capsule that drew as a capsule and collided as a box would be a
// lie in the API, and that is the kind of lie that costs an afternoon.
//
// Each returns whether they overlap and, if so, the MINIMUM TRANSLATION VECTOR:
// the shortest push that separates them, pointing from `b` towards `a`.
// ---------------------------------------------------------------------------

bool overlap_rect_rect(const Placed &a, const Placed &b, Vector2 *mtv) {
    const float dx = a.centre.x - b.centre.x;
    const float dy = a.centre.y - b.centre.y;
    const float px = (a.half.x + b.half.x) - (dx < 0 ? -dx : dx);
    const float py = (a.half.y + b.half.y) - (dy < 0 ? -dy : dy);
    if (px <= 0 || py <= 0) return false;

    // The axis of least penetration, which is the one that makes a character
    // land on top of a platform instead of being shoved out sideways.
    if (px < py) {
        *mtv = Vector2{ dx < 0 ? -px : px, 0 };
    } else {
        *mtv = Vector2{ 0, dy < 0 ? -py : py };
    }
    return true;
}

bool overlap_circle_circle(const Placed &a, const Placed &b, Vector2 *mtv) {
    const float dx = a.centre.x - b.centre.x;
    const float dy = a.centre.y - b.centre.y;
    const float sum = a.radius + b.radius;
    const float d2 = dx * dx + dy * dy;
    if (d2 >= sum * sum) return false;

    const float d = std::sqrt(d2);
    if (d < kEpsilon) {
        // Exactly concentric. There is no direction to push in, so one is
        // chosen rather than dividing by zero and producing NaN positions that
        // spread to everything they touch afterwards.
        *mtv = Vector2{ sum, 0 };
        return true;
    }
    const float push = sum - d;
    *mtv = Vector2{ dx / d * push, dy / d * push };
    return true;
}

// `a` is the circle, `b` the rectangle. The MTV still points towards `a`.
bool overlap_circle_rect(const Placed &a, const Placed &b, Vector2 *mtv) {
    const float nearest_x =
        clampf(a.centre.x, b.centre.x - b.half.x, b.centre.x + b.half.x);
    const float nearest_y =
        clampf(a.centre.y, b.centre.y - b.half.y, b.centre.y + b.half.y);
    const float dx = a.centre.x - nearest_x;
    const float dy = a.centre.y - nearest_y;
    const float d2 = dx * dx + dy * dy;

    if (d2 > kEpsilon * kEpsilon) {
        // The centre is outside the rectangle: the nearest point is on an edge
        // or a corner, and the normal runs through it. This is the branch that
        // makes a Pong ball leave the corner of a paddle at the right angle.
        if (d2 >= a.radius * a.radius) return false;
        const float d = std::sqrt(d2);
        const float push = a.radius - d;
        *mtv = Vector2{ dx / d * push, dy / d * push };
        return true;
    }

    // The centre is inside the rectangle. The nearest exit is the nearest face,
    // and it always overlaps.
    const float left = a.centre.x - (b.centre.x - b.half.x);
    const float right = (b.centre.x + b.half.x) - a.centre.x;
    const float up = a.centre.y - (b.centre.y - b.half.y);
    const float down = (b.centre.y + b.half.y) - a.centre.y;
    float best = left;
    Vector2 dir{ -1, 0 };
    if (right < best) {
        best = right;
        dir = Vector2{ 1, 0 };
    }
    if (up < best) {
        best = up;
        dir = Vector2{ 0, -1 };
    }
    if (down < best) {
        best = down;
        dir = Vector2{ 0, 1 };
    }
    const float push = best + a.radius;
    *mtv = Vector2{ dir.x * push, dir.y * push };
    return true;
}

// Dispatch, with the shapes put in the order each test expects.
bool overlap(const Placed &a, const Placed &b, Vector2 *mtv) {
    if (a.kind == ShapeKind::NONE || b.kind == ShapeKind::NONE) return false;
    if (a.kind == ShapeKind::RECTANGLE && b.kind == ShapeKind::RECTANGLE) {
        return overlap_rect_rect(a, b, mtv);
    }
    if (a.kind == ShapeKind::CIRCLE && b.kind == ShapeKind::CIRCLE) {
        return overlap_circle_circle(a, b, mtv);
    }
    if (a.kind == ShapeKind::CIRCLE) return overlap_circle_rect(a, b, mtv);

    // b is the circle: run it the other way and flip the push, so that the MTV
    // still points at `a`.
    Vector2 flipped{};
    if (!overlap_circle_rect(b, a, &flipped)) return false;
    *mtv = Vector2{ -flipped.x, -flipped.y };
    return true;
}

// ---------------------------------------------------------------------------
// Rays
// ---------------------------------------------------------------------------

// Slab method. `t` comes back in [0,1] along from->to, and the normal is the
// face that was entered.
bool ray_rect(Vector2 from, Vector2 to, const Placed &box, float *t, Vector2 *normal) {
    const Vector2 d{ to.x - from.x, to.y - from.y };
    const float min_x = box.centre.x - box.half.x;
    const float max_x = box.centre.x + box.half.x;
    const float min_y = box.centre.y - box.half.y;
    const float max_y = box.centre.y + box.half.y;

    float near = 0;
    float far = 1;
    Vector2 axis{ 0, 0 };

    // x, then y, written out rather than looped: a Vector2 has named members
    // and indexing one through a cast is exactly the sort of cleverness that
    // stops compiling on a target nobody is looking at.
    for (int i = 0; i < 2; i++) {
        const float origin = i == 0 ? from.x : from.y;
        const float dir = i == 0 ? d.x : d.y;
        const float lo = i == 0 ? min_x : min_y;
        const float hi = i == 0 ? max_x : max_y;

        if (dir > -kEpsilon && dir < kEpsilon) {
            // Parallel to this pair of faces: either inside the slab for the
            // whole ray, or never.
            if (origin < lo || origin > hi) return false;
            continue;
        }
        float t0 = (lo - origin) / dir;
        float t1 = (hi - origin) / dir;
        Vector2 n0 = i == 0 ? Vector2{ -1, 0 } : Vector2{ 0, -1 };
        if (t0 > t1) {
            const float swap = t0;
            t0 = t1;
            t1 = swap;
            n0 = i == 0 ? Vector2{ 1, 0 } : Vector2{ 0, 1 };
        }
        if (t0 > near) {
            near = t0;
            axis = n0;
        }
        if (t1 < far) far = t1;
        if (near > far) return false;
    }

    *t = near;
    // Starting inside gives near == 0 and no face was crossed. The normal then
    // points back down the ray, which is the only answer that is not a lie.
    if (axis.x == 0 && axis.y == 0) {
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        axis = len > kEpsilon ? Vector2{ -d.x / len, -d.y / len } : Vector2{ 0, -1 };
    }
    *normal = axis;
    return true;
}

bool ray_circle(Vector2 from, Vector2 to, const Placed &circle, float *t,
                Vector2 *normal) {
    const Vector2 d{ to.x - from.x, to.y - from.y };
    const Vector2 m{ from.x - circle.centre.x, from.y - circle.centre.y };
    const float a = d.x * d.x + d.y * d.y;
    if (a < kEpsilon) return false;
    const float b = 2 * (m.x * d.x + m.y * d.y);
    const float c = m.x * m.x + m.y * m.y - circle.radius * circle.radius;

    const float disc = b * b - 4 * a * c;
    if (disc < 0) return false;
    const float root = std::sqrt(disc);
    float hit = (-b - root) / (2 * a);
    if (hit < 0) hit = (-b + root) / (2 * a); // the origin is inside
    if (hit < 0 || hit > 1) return false;

    *t = hit;
    const Vector2 point{ from.x + d.x * hit, from.y + d.y * hit };
    Vector2 n{ point.x - circle.centre.x, point.y - circle.centre.y };
    const float len = std::sqrt(n.x * n.x + n.y * n.y);
    *normal = len > kEpsilon ? Vector2{ n.x / len, n.y / len } : Vector2{ 0, -1 };
    return true;
}

bool ray_shape(Vector2 from, Vector2 to, const Placed &shape, float *t, Vector2 *normal) {
    if (shape.kind == ShapeKind::CIRCLE) return ray_circle(from, to, shape, t, normal);
    if (shape.kind == ShapeKind::RECTANGLE) return ray_rect(from, to, shape, t, normal);
    return false;
}

// The swept test, and the other half of the answer to tunnelling.
//
// [app] max_delta stops a stalled frame teleporting everything. It does not
// help with the second case, which happens at a perfectly normal frame rate: a
// bullet at 900 u/s moves 15 units a frame, and against a wall 10 units thick
// there is no frame on which the two overlap. The overlap test is never true
// and the bullet goes through.
//
// So a fast mover is tested against the SEGMENT it travelled rather than
// against the box it ended in. Minkowski: grow the stationary shape by the
// mover's extents and cast a ray from the old centre to the new one. For
// rect-against-rect and circle-against-circle that is exact. For a circle
// against a rectangle the true sum is a rounded rectangle and this uses the
// square one, which over-reports by at most a corner -- the error is a hit
// reported a fraction of a unit early, never one missed.
bool sweep(const Placed &mover, Vector2 from, const Placed &other, float *t,
           Vector2 *normal) {
    Placed grown = other;
    if (mover.kind == ShapeKind::CIRCLE) {
        if (other.kind == ShapeKind::CIRCLE) {
            grown.radius += mover.radius;
        } else {
            grown.half.x += mover.radius;
            grown.half.y += mover.radius;
        }
    } else {
        if (other.kind == ShapeKind::CIRCLE) {
            // A circle grown by a box is a rounded box; the box is the
            // conservative stand-in, same as above and in the same direction.
            grown.kind = ShapeKind::RECTANGLE;
            grown.half =
                Vector2{ other.radius + mover.half.x, other.radius + mover.half.y };
        } else {
            grown.half.x += mover.half.x;
            grown.half.y += mover.half.y;
        }
    }
    return ray_shape(from, mover.centre, grown, t, normal);
}

// ---------------------------------------------------------------------------
// The broad phase: a uniform grid.
//
// With three hundred objects the difference against the O(n^2) loop is already
// visible, and the grid is forty lines. It has to be CONSERVATIVE -- every
// pair that really overlaps must reach the narrow phase -- and
// tests/collision_test.cpp proves that the hard way, by running both over a
// thousand random objects and comparing the sets.
// ---------------------------------------------------------------------------

struct Entry {
    Object *object = nullptr;
    Placed shape;
    Rectangle swept; // start box merged with end box
    Vector2 from{}; // where its centre was before this frame's integration
    bool moved_fast = false;
};

struct Pair {
    int a = 0;
    int b = 0;
};

struct Grid {
    float cell = 64;
    int min_x = 0;
    int min_y = 0;
    int cols = 1;
    int rows = 1;
    std::vector<std::vector<int>> buckets;

    int index_of(int cx, int cy) const { return (cy - min_y) * cols + (cx - min_x); }
};

int cell_floor(float v, float cell) { return static_cast<int>(std::floor(v / cell)); }

void build_grid(const std::vector<Entry> &entries, Grid *grid) {
    if (entries.empty()) return;

    // The cell is the size of the average object, which is the choice that
    // keeps both failure modes away: cells much smaller than the objects put
    // every object in dozens of buckets, and cells much larger put every object
    // in one.
    float total = 0;
    for (const Entry &e : entries) {
        total += (e.swept.width > e.swept.height ? e.swept.width : e.swept.height);
    }
    grid->cell = clampf(total / static_cast<float>(entries.size()), 8.0f, 2048.0f);

    float x0 = entries[0].swept.x;
    float y0 = entries[0].swept.y;
    float x1 = x0;
    float y1 = y0;
    for (const Entry &e : entries) {
        if (e.swept.x < x0) x0 = e.swept.x;
        if (e.swept.y < y0) y0 = e.swept.y;
        if (e.swept.x + e.swept.width > x1) x1 = e.swept.x + e.swept.width;
        if (e.swept.y + e.swept.height > y1) y1 = e.swept.y + e.swept.height;
    }

    grid->min_x = cell_floor(x0, grid->cell);
    grid->min_y = cell_floor(y0, grid->cell);
    grid->cols = cell_floor(x1, grid->cell) - grid->min_x + 1;
    grid->rows = cell_floor(y1, grid->cell) - grid->min_y + 1;

    // A world spread over a huge area with few objects in it would ask for a
    // grid of millions of empty buckets. Past that point the O(n^2) loop is
    // cheaper than the allocation, so the grid says so by staying empty and the
    // caller falls back.
    const long long total_cells =
        static_cast<long long>(grid->cols) * static_cast<long long>(grid->rows);
    if (total_cells > 1'000'000LL) {
        grid->cols = 0;
        grid->rows = 0;
        return;
    }

    grid->buckets.assign(static_cast<std::size_t>(total_cells), {});
    for (std::size_t i = 0; i < entries.size(); i++) {
        const Rectangle &box = entries[i].swept;
        const int cx0 = cell_floor(box.x, grid->cell);
        const int cy0 = cell_floor(box.y, grid->cell);
        const int cx1 = cell_floor(box.x + box.width, grid->cell);
        const int cy1 = cell_floor(box.y + box.height, grid->cell);
        for (int cy = cy0; cy <= cy1; cy++) {
            for (int cx = cx0; cx <= cx1; cx++) {
                grid->buckets[static_cast<std::size_t>(grid->index_of(cx, cy))].push_back(
                    static_cast<int>(i));
            }
        }
    }
}

// Two objects interact when either one's mask touches the other's layer.
bool layers_interact(const Object &a, const Object &b) {
    return (a.collision_mask & b.collision_layer) != 0 ||
        (b.collision_mask & a.collision_layer) != 0;
}

void candidate_pairs(const std::vector<Entry> &entries, const Grid &grid,
                     std::vector<Pair> *out) {
    out->clear();
    const int n = static_cast<int>(entries.size());

    if (grid.buckets.empty()) {
        // No grid: either nothing to do, or a world too sparse to bucket. Every
        // pair, which is correct and is what the grid is an optimisation of.
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) out->push_back(Pair{ i, j });
        }
    } else {
        for (const std::vector<int> &bucket : grid.buckets) {
            for (std::size_t i = 0; i < bucket.size(); i++) {
                for (std::size_t j = i + 1; j < bucket.size(); j++) {
                    const int a = bucket[i] < bucket[j] ? bucket[i] : bucket[j];
                    const int b = bucket[i] < bucket[j] ? bucket[j] : bucket[i];
                    out->push_back(Pair{ a, b });
                }
            }
        }
        // An object spanning several cells meets the same neighbour in each of
        // them. Sorting and collapsing is cheaper than a hash set and keeps the
        // order deterministic, which matters: the differential test compares
        // this against the O(n^2) loop, and a set ordered by pointer would make
        // the comparison depend on the allocator.
        std::ranges::sort(*out, [](const Pair &x, const Pair &y) {
            return x.a != y.a ? x.a < y.a : x.b < y.b;
        });
        const auto dup = std::ranges::unique(
            *out, [](const Pair &x, const Pair &y) { return x.a == y.a && x.b == y.b; });
        out->erase(dup.begin(), dup.end());
    }
}

// What one touching pair amounts to. Detection produces these and changes
// nothing; resolution applies them afterwards.
//
// Splitting the two is not tidiness. Resolving as each pair is found makes the
// result depend on the ORDER the pairs came out in, which is the one thing a
// broad phase is allowed to change -- and then the grid and the O(n^2) loop can
// disagree while both being right, and the differential test in
// tests/collision_test.cpp could not exist. Detecting first makes the pair set
// a pure function of the world.
struct Touch {
    Object *a = nullptr;
    Object *b = nullptr;
    Vector2 mtv{};
    Object *rewind = nullptr; // the swept case: who to pull back to contact
    Vector2 rewind_to{};
};

std::vector<Touch> detect(const Scene &scene, bool use_grid) {
    std::vector<Touch> touching;

    std::vector<Entry> entries;
    for (Object *object : objects::detail::live_objects(scene)) {
        Entry e;
        e.object = object;
        e.shape = placed_of(*object);
        if (e.shape.kind == ShapeKind::NONE) continue; // nothing to collide with
        // The centre moves with the position, so the old centre is the new one
        // minus however far the position travelled.
        const Vector2 was = Storage::previous_position(*object);
        e.from = Vector2{ e.shape.centre.x - (object->position.x - was.x),
                          e.shape.centre.y - (object->position.y - was.y) };
        const Rectangle now = aabb_of(e.shape);

        // "Fast" is more than half its own smaller dimension in one step, which
        // is exactly the threshold at which an overlap test starts being able
        // to miss.
        const float travel_x = e.shape.centre.x - e.from.x;
        const float travel_y = e.shape.centre.y - e.from.y;
        const float travel2 = travel_x * travel_x + travel_y * travel_y;
        const float smaller = now.width < now.height ? now.width : now.height;
        e.moved_fast = travel2 > (smaller * smaller) / 4;

        if (e.moved_fast) {
            Placed start = e.shape;
            start.centre = e.from;
            e.swept = merge(now, aabb_of(start));
        } else {
            e.swept = now;
        }
        entries.push_back(e);
    }
    if (entries.size() < 2) return touching;

    std::vector<Pair> pairs;
    if (use_grid) {
        Grid grid;
        build_grid(entries, &grid);
        candidate_pairs(entries, grid, &pairs);
    } else {
        const int n = static_cast<int>(entries.size());
        for (int i = 0; i < n; i++) {
            for (int j = i + 1; j < n; j++) pairs.push_back(Pair{ i, j });
        }
    }

    for (const Pair &pair : pairs) {
        const Entry &ea = entries[static_cast<std::size_t>(pair.a)];
        const Entry &eb = entries[static_cast<std::size_t>(pair.b)];
        Object &a = *ea.object;
        Object &b = *eb.object;

        if (!layers_interact(a, b)) continue;
        if (!boxes_overlap(ea.swept, eb.swept)) continue;

        Touch touch;
        touch.a = &a;
        touch.b = &b;
        if (overlap(ea.shape, eb.shape, &touch.mtv)) {
            touching.push_back(touch);
            continue;
        }
        if (!ea.moved_fast && !eb.moved_fast) continue;

        // The swept test, for the pair the overlap test cannot see. The one
        // that travelled further is treated as the mover, because that is the
        // one more likely to have jumped the other.
        const float da =
            (ea.shape.centre.x - ea.from.x) * (ea.shape.centre.x - ea.from.x) +
            (ea.shape.centre.y - ea.from.y) * (ea.shape.centre.y - ea.from.y);
        const float db =
            (eb.shape.centre.x - eb.from.x) * (eb.shape.centre.x - eb.from.x) +
            (eb.shape.centre.y - eb.from.y) * (eb.shape.centre.y - eb.from.y);
        const bool a_moves = da >= db;
        const Entry &mover = a_moves ? ea : eb;
        const Entry &fixed = a_moves ? eb : ea;

        float t = 0;
        Vector2 normal{};
        if (!sweep(mover.shape, mover.from, fixed.shape, &t, &normal)) continue;

        // Where the mover should end up: at the moment of contact, not inside
        // or beyond. Recorded rather than applied, so detection stays pure.
        Object &moving = a_moves ? a : b;
        touch.rewind = &moving;
        touch.rewind_to =
            Vector2{ moving.position.x + (mover.from.x - mover.shape.centre.x) * (1 - t),
                     moving.position.y +
                         (mover.from.y - mover.shape.centre.y) * (1 - t) };
        // Contact with no depth: the MTV is the contact normal, pointing at a.
        touch.mtv = a_moves ? Vector2{ normal.x * kEpsilon, normal.y * kEpsilon }
                            : Vector2{ -normal.x * kEpsilon, -normal.y * kEpsilon };
        touching.push_back(touch);
    }
    return touching;
}

} // namespace

// ---------------------------------------------------------------------------
// The public halves
// ---------------------------------------------------------------------------

Rectangle Object::world_collider() const {
    const Placed p = placed_of(*this);
    if (p.kind == ShapeKind::NONE) return Rectangle{ position.x, position.y, 0, 0 };
    return aabb_of(p);
}

namespace objects::detail {

void collide(Scene &scene) {
    const std::vector<Touch> touching = detect(scene, true);

    // Rewinds first: an object caught by the swept test is put back where it
    // met the other one, and then separation works from a sane position.
    for (const Touch &touch : touching) {
        if (touch.rewind != nullptr && touch.rewind->alive()) {
            touch.rewind->position = touch.rewind_to;
        }
    }

    // Then separation, for the pairs that are both solid.
    for (const Touch &touch : touching) {
        Object &a = *touch.a;
        Object &b = *touch.b;
        if (!a.alive() || !b.alive()) continue;
        if (!a.solid || !b.solid) continue;
        if (!a.immovable && !b.immovable) {
            a.position.x += touch.mtv.x / 2;
            a.position.y += touch.mtv.y / 2;
            b.position.x -= touch.mtv.x / 2;
            b.position.y -= touch.mtv.y / 2;
        } else if (!a.immovable) {
            a.position.x += touch.mtv.x;
            a.position.y += touch.mtv.y;
        } else if (!b.immovable) {
            b.position.x -= touch.mtv.x;
            b.position.y -= touch.mtv.y;
        }
        // Two immovable objects overlapping is a mistake in the level, not
        // something to solve at runtime. Doing nothing is right: moving one
        // would be inventing an answer.
    }

    // Notification last, so that by the time the user's code runs, the
    // positions it reads are the resolved ones. The other order hands them an
    // overlap that is about to be undone.
    for (const Touch &touch : touching) {
        Object &a = *touch.a;
        Object &b = *touch.b;
        // An object destroyed by an earlier pair this frame is gone, and
        // handing it to somebody else's _collision would be handing out
        // something whose _end() has already run.
        if (!a.alive() || !b.alive()) continue;
        a._collision(b);
        if (a.alive() && b.alive()) b._collision(a);
        if (a.alive() && b.alive()) Storage::notify_collision(a, b);
        if (a.alive() && b.alive()) Storage::notify_collision(b, a);
    }
}

int touching_pairs_for_tests(const Scene &scene, bool use_grid, Object **out, int max) {
    const std::vector<Touch> touching = detect(scene, use_grid);
    int written = 0;
    for (const Touch &touch : touching) {
        if (written + 1 > max) break;
        const auto slot = static_cast<std::size_t>(written) * 2;
        out[slot] = touch.a;
        out[slot + 1] = touch.b;
        written++;
    }
    return written;
}

// ---------------------------------------------------------------------------
// The pointer
// ---------------------------------------------------------------------------

namespace {
Handle<Object> g_captured;
Vector2 g_press_at{};
} // namespace

void pointer(Scene &scene) {
    // The UI gets first refusal. Without this, a button drawn over the world
    // fires the gun underneath it, which is the single most common complaint
    // about hand-rolled input in a game with a HUD.
    if (rmp::input::consumed_pointer()) {
        g_captured = Handle<Object>();
        return;
    }

    const Vector2 at = rmp::input::pointer_screen();

    if (rmp::input::pointer_pressed()) {
        // Topmost first: the last thing drawn is the first thing clicked, which
        // is the order a player sees. draw_order() is layer then creation, so
        // walking it backwards is exactly that.
        const std::vector<Object *> &order = draw_order(scene);
        for (std::size_t i = order.size(); i > 0; i--) {
            Object *object = order[i - 1];
            if (!object->alive()) continue;
            if (!Storage::has_pointer_callback(*object)) continue;
            const Placed p = placed_of(*object);
            if (p.kind == ShapeKind::NONE) continue;
            Placed point;
            point.kind = ShapeKind::CIRCLE;
            point.centre = at;
            point.radius = 0;
            Vector2 ignored{};
            if (overlap(point, p, &ignored)) {
                g_captured = object->handle();
                g_press_at = at;
                break;
            }
        }
        return;
    }

    Object *captured = g_captured.get();
    if (captured == nullptr) return;

    if (rmp::input::pointer_down()) {
        const Vector2 moved = rmp::input::pointer_delta();
        if (moved.x != 0 || moved.y != 0) Storage::notify_drag(*captured, moved);
        return;
    }

    if (rmp::input::pointer_released()) {
        // A click is a press and a release on the same object, which is what
        // every UI on every platform means by one and what lets a player change
        // their mind by dragging off the thing before letting go.
        const Placed p = placed_of(*captured);
        Placed point;
        point.kind = ShapeKind::CIRCLE;
        point.centre = at;
        point.radius = 0;
        Vector2 ignored{};
        if (p.kind != ShapeKind::NONE && overlap(point, p, &ignored)) {
            Storage::notify_click(*captured);
        }
        g_captured = Handle<Object>();
    }
}

void reset_pointer_for_tests() { g_captured = Handle<Object>(); }

} // namespace objects::detail

// ---------------------------------------------------------------------------
// Raycasting
// ---------------------------------------------------------------------------

namespace {

// Everything the ray hits, nearest first. The grid is walked with a DDA so only
// the cells the line crosses are looked at.
int cast(const Scene &scene, const RayQuery &query, RayHit *out, int max) {
    if (max <= 0) return 0;

    std::vector<Entry> entries;
    for (Object *object : objects::detail::live_objects(scene)) {
        if (object == query.ignore) continue;
        if ((query.mask & object->collision_layer) == 0) continue;
        Entry e;
        e.object = object;
        e.shape = placed_of(*object);
        if (e.shape.kind == ShapeKind::NONE) continue;
        e.swept = aabb_of(e.shape);
        entries.push_back(e);
    }
    if (entries.empty()) return 0;

    Grid grid;
    build_grid(entries, &grid);

    std::vector<int> candidates;
    if (grid.buckets.empty()) {
        candidates.reserve(entries.size());
        for (std::size_t i = 0; i < entries.size(); i++) {
            candidates.push_back(static_cast<int>(i));
        }
    } else {
        // Amanatides-Woo: step cell by cell along the line, taking whichever
        // axis boundary comes first. Only the cells the ray actually crosses.
        const float cell = grid.cell;
        const Vector2 d{ query.to.x - query.from.x, query.to.y - query.from.y };
        int cx = cell_floor(query.from.x, cell);
        int cy = cell_floor(query.from.y, cell);
        const int cx_end = cell_floor(query.to.x, cell);
        const int cy_end = cell_floor(query.to.y, cell);
        const int step_x = d.x > 0 ? 1 : (d.x < 0 ? -1 : 0);
        const int step_y = d.y > 0 ? 1 : (d.y < 0 ? -1 : 0);

        const float big = 1e30f;
        const float t_delta_x = step_x == 0 ? big : cell / (d.x < 0 ? -d.x : d.x);
        const float t_delta_y = step_y == 0 ? big : cell / (d.y < 0 ? -d.y : d.y);
        float t_max_x = big;
        float t_max_y = big;
        if (step_x != 0) {
            const float boundary =
                (static_cast<float>(cx) + (step_x > 0 ? 1.0f : 0.0f)) * cell;
            t_max_x = (boundary - query.from.x) / d.x;
        }
        if (step_y != 0) {
            const float boundary =
                (static_cast<float>(cy) + (step_y > 0 ? 1.0f : 0.0f)) * cell;
            t_max_y = (boundary - query.from.y) / d.y;
        }

        // A bound on the steps, because a NaN or an infinity in the input would
        // otherwise be an infinite loop rather than an empty result.
        //
        // The bound is the RAY's own span and not the grid's size. Using the
        // grid's meant a ray that starts outside it ran out of steps before
        // reaching it: a shot from the origin at a target twenty units wide two
        // hundred units away got six steps to cross nine cells, and came back
        // as a miss. Two raycast tests and the differential caught it.
        const int span_x = cx_end > cx ? cx_end - cx : cx - cx_end;
        const int span_y = cy_end > cy ? cy_end - cy : cy - cy_end;
        const int limit = span_x + span_y + 2;
        for (int step = 0; step <= limit; step++) {
            if (cx >= grid.min_x && cy >= grid.min_y && cx < grid.min_x + grid.cols &&
                cy < grid.min_y + grid.rows) {
                const std::vector<int> &bucket =
                    grid.buckets[static_cast<std::size_t>(grid.index_of(cx, cy))];
                candidates.insert(candidates.end(), bucket.begin(), bucket.end());
            }
            if (cx == cx_end && cy == cy_end) break;
            if (t_max_x < t_max_y) {
                if (step_x == 0) break;
                t_max_x += t_delta_x;
                cx += step_x;
            } else {
                if (step_y == 0) break;
                t_max_y += t_delta_y;
                cy += step_y;
            }
        }
        std::ranges::sort(candidates);
        const auto dup = std::ranges::unique(candidates);
        candidates.erase(dup.begin(), dup.end());
    }

    // The index alongside the hit, so that two objects at the SAME distance come
    // back in creation order. Sorting by the object pointer instead made the
    // answer depend on where malloc happened to put things, which is not a
    // detail -- a ray starting inside two overlapping objects hits both at
    // distance zero, which is common, and the result then changed depending on
    // what had run earlier in the process. The differential test caught it by
    // passing on its own and failing in the suite.
    struct Ordered {
        RayHit hit;
        int order = 0;
    };
    std::vector<Ordered> hits;
    for (int i : candidates) {
        const Entry &e = entries[static_cast<std::size_t>(i)];
        float t = 0;
        Vector2 normal{};
        if (!ray_shape(query.from, query.to, e.shape, &t, &normal)) continue;
        RayHit hit;
        hit.object = e.object;
        hit.point = Vector2{ query.from.x + (query.to.x - query.from.x) * t,
                             query.from.y + (query.to.y - query.from.y) * t };
        hit.normal = normal;
        const float dx = hit.point.x - query.from.x;
        const float dy = hit.point.y - query.from.y;
        hit.distance = std::sqrt(dx * dx + dy * dy);
        hits.push_back(Ordered{ hit, i });
    }

    std::ranges::sort(hits, [](const Ordered &x, const Ordered &y) {
        if (x.hit.distance != y.hit.distance) return x.hit.distance < y.hit.distance;
        return x.order < y.order; // creation order, which is stable
    });

    const auto room = static_cast<std::size_t>(max);
    const std::size_t count = hits.size() < room ? hits.size() : room;
    for (std::size_t i = 0; i < count; i++) out[i] = hits[i].hit;
    return static_cast<int>(count);
}

} // namespace

RayHit Scene::raycast(Vector2 from, Vector2 to) const {
    return raycast(RayQuery{ .from = from, .to = to });
}

RayHit Scene::raycast(const RayQuery &query) const {
    RayHit hit;
    cast(*this, query, &hit, 1);
    return hit;
}

int Scene::raycast_all(const RayQuery &query, RayHit *out, int max) const {
    if (out == nullptr) return 0;
    return cast(*this, query, out, max);
}

} // namespace rmp
