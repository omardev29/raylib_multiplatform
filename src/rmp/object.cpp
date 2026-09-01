// ---------------------------------------------------------------------------
// The object storage, the integrator, the edge rules and the draw pass.
//
// The public surface is include/rmp/object.h. What lives here is the half the
// user never calls, and the reason the header can stay free of <memory> and
// <vector>: a file with an entity in it should not pay 605 ms for the first and
// this one pays it once.
//
// THE STORAGE, and why it is a vector of unique_ptr and not something cleverer.
//
// What is genuinely hard to change later is not the container, it is the
// contract: spawn() returns T&, that reference is good for the whole frame, and
// between frames you use a handle with a generation. That contract requires
// pointers to be stable within a frame, which rules out the one option that
// would be meaningfully faster -- a contiguous array of a concrete type, where
// growing moves everything and every reference dies. The API closes that door
// on purpose, because the price of opening it is that `auto &bullet = spawn()`
// stops being safe and all user code has to speak in handles.
//
// Inside what the contract allows, this is the obvious implementation, and the
// arithmetic says it is enough: 2 000 objects are 2 000 pointer chases a frame,
// and even if every one were a full cache miss (~100 ns) that is 200 us against
// a 16.6 ms budget -- 1.2 % of the frame. Per-type pools come when a real game
// measures a reason, and when they do, spawn() still returns T& and a handle is
// still index plus generation, so it is a commit inside src/rmp/ and nothing
// else.
// ---------------------------------------------------------------------------

#include <rmp/object.h>
#include <rmp/scene.h>

#include "object_internal.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace rmp {

namespace {

struct Cell {
    std::unique_ptr<Object> object;
    // 0 is the "points at nothing" generation, so a default-constructed handle
    // is dead without anyone having to say so. Live slots start at 1 and the
    // number goes up every time the slot is freed, which is what makes a handle
    // to a dead object stay dead when the slot is reused.
    unsigned generation = 0;
    bool occupied = false;
};

std::vector<Cell> g_slots;
std::vector<unsigned> g_free;
// A vector and not an unordered_map keyed by Scene*, for two reasons that
// point the same way: iterating a hash map of POINTERS is nondeterministic --
// the order changes between runs with ASLR, and clang-tidy says so -- and the
// stack is three scenes deep on a bad day, so a linear scan over it is cheaper
// than hashing anyway.
struct SceneObjects {
    const Scene *scene = nullptr;
    std::vector<unsigned> indices;
};
std::vector<SceneObjects> g_by_scene;
std::vector<unsigned> g_pending_free;

// Scratch, reused every draw pass so the sort does not allocate every frame.
std::vector<Object *> g_draw_order;

std::vector<unsigned> *indices_for(const Scene *scene) {
    for (SceneObjects &entry : g_by_scene) {
        if (entry.scene == scene) return &entry.indices;
    }
    return nullptr;
}

std::vector<unsigned> &indices_for_or_add(const Scene *scene) {
    if (std::vector<unsigned> *found = indices_for(scene)) return *found;
    g_by_scene.push_back(SceneObjects{ scene, {} });
    return g_by_scene.back().indices;
}

Cell *slot_at(unsigned index) {
    if (index >= g_slots.size()) return nullptr;
    return &g_slots[index];
}

// The view, for an object whose `bounds` are empty. GetScreenWidth() is 0
// before a window exists, which is exactly the case tests/object_test.cpp runs
// in, so the design size from the .toml stands in. A test that cares about a
// specific rectangle sets `bounds` and does not depend on either.
Rectangle view_rect() {
    auto w = static_cast<float>(GetScreenWidth());
    auto h = static_cast<float>(GetScreenHeight());
    if (w <= 0 || h <= 0) {
        w = static_cast<float>(APP_WINDOW_WIDTH);
        h = static_cast<float>(APP_WINDOW_HEIGHT);
    }
    return Rectangle{ 0, 0, w, h };
}

bool empty_rect(const Rectangle &r) { return r.width <= 0 || r.height <= 0; }

} // namespace

// ---------------------------------------------------------------------------
// Shapes
// ---------------------------------------------------------------------------

Shape rect(Vector2 size) {
    Shape s;
    s.kind = ShapeKind::RECTANGLE;
    s.size = size;
    return s;
}

Shape circle(float radius) {
    Shape s;
    s.kind = ShapeKind::CIRCLE;
    s.radius = radius;
    return s;
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

namespace detail {

Object *resolve(unsigned index, unsigned generation) {
    if (generation == 0) return nullptr;
    Cell *slot = slot_at(index);
    if (slot == nullptr || !slot->occupied) return nullptr;
    if (slot->generation != generation) return nullptr;
    // A handle goes false the moment destroy() is called, not when the memory
    // is finally released at the end of the frame. Anything else would let
    // `if (target)` pass and then hand back an object that has already had its
    // _end() run and is not being updated or drawn.
    if (slot->object == nullptr || !slot->object->alive()) return nullptr;
    return slot->object.get();
}

} // namespace detail

// The one key into Object's private half, and the reason it is a struct and not
// a friend declaration naming the integrator: the public header would otherwise
// have to declare `namespace objects::detail { void update(Scene&, float); }`
// to have a name to befriend, which puts an internal function in front of every
// user who includes rmp/object.h.
struct Storage {
    static Vector2 take_force(Object &object) {
        const Vector2 force = object.pending_force_;
        object.pending_force_ = Vector2{ 0, 0 };
        return force;
    }
};

// ---------------------------------------------------------------------------
// Object
// ---------------------------------------------------------------------------

void Object::apply_force(Vector2 force) {
    pending_force_.x += force.x;
    pending_force_.y += force.y;
}

void Object::apply_impulse(Vector2 impulse) {
    const float m = mass > 0 ? mass : 1.0f;
    velocity.x += impulse.x / m;
    velocity.y += impulse.y / m;
}

Rectangle Object::world_bounds() const {
    // The sprite wins, the same way it wins when drawing.
    if (sprite.texture.valid()) {
        float w = sprite.size.x;
        float h = sprite.size.y;
        if (w <= 0 || h <= 0) {
            const Texture2D &tex = sprite.texture;
            w = sprite.source.width > 0 ? sprite.source.width
                                        : static_cast<float>(tex.width);
            h = sprite.source.height > 0 ? sprite.source.height
                                         : static_cast<float>(tex.height);
        }
        w *= scale.x;
        h *= scale.y;
        // `origin` is normalised, so this is where the picture actually lands.
        return Rectangle{ position.x - sprite.origin.x * w,
                          position.y - sprite.origin.y * h, w, h };
    }
    switch (shape.kind) {
        case ShapeKind::RECTANGLE: {
            const float w = shape.size.x * scale.x;
            const float h = shape.size.y * scale.y;
            return Rectangle{ position.x + shape.offset.x - w / 2,
                              position.y + shape.offset.y - h / 2, w, h };
        }
        case ShapeKind::CIRCLE: {
            const float w = shape.radius * 2 * scale.x;
            const float h = shape.radius * 2 * scale.y;
            return Rectangle{ position.x + shape.offset.x - w / 2,
                              position.y + shape.offset.y - h / 2, w, h };
        }
        case ShapeKind::NONE:
        default:
            // A logic object with neither is a point, and a point is a
            // perfectly good thing to clamp or wrap.
            return Rectangle{ position.x, position.y, 0, 0 };
    }
}

void Object::destroy() {
    if (!alive_) return; // twice is harmless, and happens
    alive_ = false;
    _end();
    objects::detail::mark_for_release(index_);
}

// ---------------------------------------------------------------------------
// Scene's half
// ---------------------------------------------------------------------------

void Scene::detail_spawn(Object *made, const ObjectOptions &options) {
    unsigned index = 0;
    if (!g_free.empty()) {
        index = g_free.back();
        g_free.pop_back();
    } else {
        g_slots.emplace_back();
        index = static_cast<unsigned>(g_slots.size() - 1);
    }

    Cell &slot = g_slots[index];
    slot.occupied = true;
    slot.generation += 1; // never 0 again once a slot has been used
    slot.object.reset(made);

    made->scene_ = this;
    made->index_ = index;
    made->generation_ = slot.generation;
    made->alive_ = true;

    made->position = options.position;
    made->velocity = options.velocity;
    made->scale = options.scale;
    made->rotation = options.rotation;
    made->layer = options.layer;
    made->visible = options.visible;
    made->edges = options.edges;
    made->bounds = options.bounds;
    // `size` is shorthand and `shape` is the full form, so the full form wins.
    // Writing both is not an error: `{ .size = {12,12}, .shape = circle(6) }`
    // is a circle, because that is the one that says what it means.
    if (options.shape.kind != ShapeKind::NONE) {
        made->shape = options.shape;
    } else if (options.size.x > 0 || options.size.y > 0) {
        made->shape = rmp::rect(options.size);
    }

    indices_for_or_add(this).push_back(index);
    made->_ready();
}

void Scene::destroy(Object &object) {
    // The scene is not needed to do the work -- object.destroy() is the whole
    // of it -- but it is needed to notice this, and clang-tidy asking why the
    // method was not static is what prompted writing it. Destroying another
    // scene's object through this one is a real mistake with a confusing
    // symptom: the object does go away, and the count that did not move is on
    // the scene the caller was looking at.
    if (object.scene() != this) {
        TraceLog(LOG_WARNING,
                 "SCENE: destroy() was given an object that belongs to "
                 "another scene. Destroying it anyway; call object.destroy().");
    }
    object.destroy();
}

int Scene::object_count() const {
    const std::vector<unsigned> *indices = indices_for(this);
    if (indices == nullptr) return 0;
    int n = 0;
    for (unsigned index : *indices) {
        Cell *slot = slot_at(index);
        if (slot != nullptr && slot->occupied && slot->object != nullptr &&
            slot->object->alive()) {
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

namespace objects::detail {

void mark_for_release(unsigned index) { g_pending_free.push_back(index); }

namespace {

// One axis of the edge rules, run twice. Written once because a bug fixed on x
// and not on y is the single most likely mistake in this whole file, and the
// tests check both axes for exactly that reason.
struct Axis {
    float lo; // the object's low edge
    float size; // its extent
    float area_lo; // the world's low edge
    float area_hi; // the world's high edge
};

void apply_edges(Object &object) {
    if (object.edges == Edge::NONE) return;

    const Rectangle area = empty_rect(object.bounds) ? view_rect() : object.bounds;
    const Rectangle box = object.world_bounds();

    const float left = area.x;
    const float right = area.x + area.width;
    const float top = area.y;
    const float bottom = area.y + area.height;

    switch (object.edges) {
        case Edge::CLAMP: {
            if (box.x < left) object.position.x += left - box.x;
            if (box.x + box.width > right)
                object.position.x -= (box.x + box.width) - right;
            if (box.y < top) object.position.y += top - box.y;
            if (box.y + box.height > bottom)
                object.position.y -= (box.y + box.height) - bottom;
            break;
        }
        case Edge::BOUNCE: {
            // The velocity is only flipped when it points OUT of the world. An
            // object that starts overlapping the wall would otherwise flip
            // every frame and vibrate against it instead of leaving.
            if (box.x < left) {
                object.position.x += left - box.x;
                if (object.velocity.x < 0) object.velocity.x = -object.velocity.x;
            }
            if (box.x + box.width > right) {
                object.position.x -= (box.x + box.width) - right;
                if (object.velocity.x > 0) object.velocity.x = -object.velocity.x;
            }
            if (box.y < top) {
                object.position.y += top - box.y;
                if (object.velocity.y < 0) object.velocity.y = -object.velocity.y;
            }
            if (box.y + box.height > bottom) {
                object.position.y -= (box.y + box.height) - bottom;
                if (object.velocity.y > 0) object.velocity.y = -object.velocity.y;
            }
            break;
        }
        case Edge::WRAP: {
            // Completely past a side, not merely touching it: an asteroid that
            // teleported the instant it grazed the edge would pop rather than
            // slide across.
            //
            // And it comes back JUST OUTSIDE the far edge, so it slides in the
            // way it slid out. Shifting by the width of the world instead put
            // it back INSIDE -- an object that left the left edge reappeared
            // some way in from the right, which reads as a jump. The test for
            // both axes is what said so.
            if (box.x + box.width < left)
                object.position.x += right - box.x;
            else if (box.x > right)
                object.position.x += left - (box.x + box.width);
            if (box.y + box.height < top)
                object.position.y += bottom - box.y;
            else if (box.y > bottom)
                object.position.y += top - (box.y + box.height);
            break;
        }
        case Edge::DESTROY: {
            // Completely outside, again for a concrete reason: a bullet fired
            // from a muzzle on the edge of the screen would die at birth if
            // touching the boundary were enough.
            const bool out = box.x + box.width < left || box.x > right ||
                box.y + box.height < top || box.y > bottom;
            if (out) object.destroy();
            break;
        }
        case Edge::NONE:
        default:
            break;
    }
}

} // namespace

void update(Scene &scene, float delta) {
    const std::vector<unsigned> *start = indices_for(&scene);
    if (start == nullptr) return;

    // A snapshot of the size, so an object spawned during this pass gets its
    // _ready() now and its first _update() next frame.
    const std::size_t count = start->size();
    for (std::size_t i = 0; i < count; i++) {
        // Looked up again every iteration, and this is not caution for its own
        // sake: _update() is the user's code, it may spawn, and a spawn can
        // grow BOTH vectors underneath -- the list of scenes and the list of
        // indices. A reference taken before the loop is a dangling one by the
        // time the loop uses it.
        const std::vector<unsigned> *indices = indices_for(&scene);
        if (indices == nullptr || i >= indices->size()) break;
        const unsigned index = (*indices)[i];

        Cell *slot = slot_at(index);
        if (slot == nullptr || !slot->occupied || slot->object == nullptr) continue;
        Object *object = slot->object.get();
        if (!object->alive()) continue;

        object->_update(delta);
        if (!object->alive()) continue; // it may have destroyed itself

        // Gravity and apply_force land in the same accumulator, so an object
        // with gravity_scale = 1 that you push upwards does what it would do in
        // the world. There is no separate path for gravity.
        const float m = object->mass > 0 ? object->mass : 1.0f;
        const Vector2 force = Storage::take_force(*object);
        Vector2 acceleration{ force.x / m, force.y / m };
        acceleration.x += scene.gravity.x * object->gravity_scale;
        acceleration.y += scene.gravity.y * object->gravity_scale;

        object->velocity.x += acceleration.x * delta;
        object->velocity.y += acceleration.y * delta;
        object->position.x += object->velocity.x * delta;
        object->position.y += object->velocity.y * delta;

        apply_edges(*object);
    }
}

const std::vector<Object *> &draw_order(Scene &scene) {
    g_draw_order.clear();
    const std::vector<unsigned> *indices = indices_for(&scene);
    if (indices == nullptr) return g_draw_order;

    for (unsigned index : *indices) {
        Cell *slot = slot_at(index);
        if (slot == nullptr || !slot->occupied || slot->object == nullptr) continue;
        Object *object = slot->object.get();
        if (object->alive() && object->visible) g_draw_order.push_back(object);
    }

    // Stable, so that objects on the same layer keep creation order. That is
    // the documented tie-break and it is the one that makes a scene look the
    // same twice.
    std::ranges::stable_sort(g_draw_order, [](const Object *a, const Object *b) {
        return a->layer < b->layer;
    });
    return g_draw_order;
}

void draw(Scene &scene) {
    // A copy, because _draw() is the user's code and may spawn or destroy,
    // and either one writes to the scratch vector underneath.
    const std::vector<Object *> order = draw_order(scene);
    for (Object *object : order) {
        if (!object->alive()) continue; // an earlier _draw() may have killed it
        draw_one(*object);
        object->_draw();
    }
}

void draw_one(Object &object) {
    if (object.sprite.texture.valid()) {
        const Texture2D &tex = object.sprite.texture;
        Rectangle source = object.sprite.source;
        if (source.width <= 0 || source.height <= 0) {
            source = Rectangle{ 0, 0, static_cast<float>(tex.width),
                                static_cast<float>(tex.height) };
        }
        // Flipping is a negative source rectangle, which is raylib's own
        // convention and costs nothing.
        if (object.flip_x) source.width = -source.width;
        if (object.flip_y) source.height = -source.height;

        float w = object.sprite.size.x;
        float h = object.sprite.size.y;
        if (w <= 0 || h <= 0) {
            w = source.width < 0 ? -source.width : source.width;
            h = source.height < 0 ? -source.height : source.height;
        }
        w *= object.scale.x;
        h *= object.scale.y;

        const Rectangle dest{ object.position.x, object.position.y, w, h };
        const Vector2 origin{ object.sprite.origin.x * w, object.sprite.origin.y * h };
        DrawTexturePro(tex, source, dest, origin, object.rotation, object.sprite.tint);
        return;
    }

    switch (object.shape.kind) {
        case ShapeKind::RECTANGLE: {
            const float w = object.shape.size.x * object.scale.x;
            const float h = object.shape.size.y * object.scale.y;
            const Rectangle dest{ object.position.x + object.shape.offset.x,
                                  object.position.y + object.shape.offset.y, w, h };
            // The origin is half the size because position is the centre.
            const Vector2 origin{ w / 2, h / 2 };
            if (object.shape.filled) {
                DrawRectanglePro(dest, origin, object.rotation, object.shape.color);
            } else {
                DrawRectangleLinesEx(
                    Rectangle{ dest.x - origin.x, dest.y - origin.y, w, h },
                    object.shape.thickness, object.shape.color);
            }
            break;
        }
        case ShapeKind::CIRCLE: {
            const Vector2 centre{ object.position.x + object.shape.offset.x,
                                  object.position.y + object.shape.offset.y };
            const float r = object.shape.radius * object.scale.x;
            if (object.shape.filled) {
                DrawCircleV(centre, r, object.shape.color);
            } else {
                DrawCircleLinesV(centre, r, object.shape.color);
            }
            break;
        }
        case ShapeKind::NONE:
        default:
            break;
    }
}

void collect() {
    if (g_pending_free.empty()) return;
    for (unsigned index : g_pending_free) {
        Cell *slot = slot_at(index);
        if (slot == nullptr || !slot->occupied) continue;
        // The generation goes up HERE and not on reuse, so that a handle taken
        // out before the object died stops matching the moment the slot is
        // free -- with or without anything ever taking the slot again.
        slot->generation += 1;
        slot->occupied = false;
        slot->object.reset();
        g_free.push_back(index);
    }
    g_pending_free.clear();

    // Take the freed indices out of their scene's list. Done once per frame
    // rather than per destruction, because a wave of bullets dying together is
    // the normal case and one pass is cheaper than fifty erases.
    for (SceneObjects &entry : g_by_scene) {
        const auto gone = std::ranges::remove_if(entry.indices, [](unsigned index) {
            const Cell *slot = slot_at(index);
            return slot == nullptr || !slot->occupied;
        });
        entry.indices.erase(gone.begin(), gone.end());
    }
}

void release_scene(Scene &scene) {
    const std::vector<unsigned> *found = indices_for(&scene);
    if (found == nullptr) return;
    // A copy, because _end() is entitled to touch other objects and anything
    // it does can reach back into this list.
    const std::vector<unsigned> indices = *found;
    for (unsigned index : indices) {
        Cell *slot = slot_at(index);
        if (slot == nullptr || !slot->occupied || slot->object == nullptr) continue;
        if (slot->object->alive()) slot->object->destroy();
    }
    collect();
    const auto gone =
        std::ranges::remove_if(g_by_scene, [&scene](const SceneObjects &entry) {
            return entry.scene == &scene;
        });
    g_by_scene.erase(gone.begin(), gone.end());
}

void reset_for_tests() {
    g_by_scene.clear();
    g_pending_free.clear();
    g_draw_order.clear();
    g_free.clear();
    g_slots.clear();
}

int live_count() {
    int n = 0;
    for (const Cell &slot : g_slots) {
        if (slot.occupied && slot.object != nullptr && slot.object->alive()) n++;
    }
    return n;
}

int slot_count() { return static_cast<int>(g_slots.size()); }

} // namespace objects::detail

} // namespace rmp
