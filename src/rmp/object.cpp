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

#include "animation_internal.h"
#include "internal.h"
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

// Every point at which the framework knows the world may have changed shape or
// moved. See world_version() in object_internal.h for what reads it.
unsigned g_world_version = 1;

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

// Storage is declared in object_internal.h, so that src/rmp/collision.cpp can
// reach the same private half without Object having to befriend every internal
// function by name.
Vector2 Storage::take_force(Object &object) {
    const Vector2 force = object.pending_force_;
    object.pending_force_ = Vector2{ 0, 0 };
    return force;
}

Vector2 Storage::previous_position(const Object &object) {
    return object.previous_position_;
}

void Storage::remember_position(Object &object) {
    object.previous_position_ = object.position;
}

void Storage::notify_collision(Object &self, Object &other) {
    self.collision_(self, other);
}

void Storage::notify_click(Object &self) { self.click_(self); }

void Storage::notify_drag(Object &self, Vector2 moved) { self.drag_(self, moved); }

bool Storage::has_pointer_callback(const Object &object) {
    return static_cast<bool>(object.click_) || static_cast<bool>(object.drag_);
}

bool Storage::entered_bounds(const Object &object) { return object.entered_bounds_; }
void Storage::set_entered_bounds(Object &object, bool entered) {
    object.entered_bounds_ = entered;
}

int Storage::behavior_slot(const Object &object) { return object.behavior_slot_; }

void Storage::set_behavior_slot(Object &object, int slot) {
    object.behavior_slot_ = slot;
}

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
    // A sheet wins over a loose texture, the same way it does when drawing.
    if (sprite.sheet.valid()) {
        const SheetData &data = sprite.sheet.raw();
        const float w =
            (sprite.size.x > 0 ? sprite.size.x : static_cast<float>(data.width)) *
            scale.x;
        const float h =
            (sprite.size.y > 0 ? sprite.size.y : static_cast<float>(data.height)) *
            scale.y;
        return Rectangle{ position.x - sprite.origin.x * w,
                          position.y - sprite.origin.y * h, w, h };
    }
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
            // ONE axis for both, because a circle scaled unevenly would be an
            // ellipse and this layer has no ellipses. The box used to be the
            // ellipse's -- width from x and height from y -- so a coin squashed
            // for a squash-and-stretch effect drew one size, collided at a
            // second and reported a third, and Edge::CLAMP (which reads this)
            // stopped it short of the floor with nothing to explain why.
            const float d = shape.radius * 2 * circle_scale(scale);
            return Rectangle{ position.x + shape.offset.x - d / 2,
                              position.y + shape.offset.y - d / 2, d, d };
        }
        case ShapeKind::NONE:
        default:
            // A logic object with neither is a point, and a point is a
            // perfectly good thing to clamp or wrap.
            return Rectangle{ position.x, position.y, 0, 0 };
    }
}

Object::~Object() {
    // The object may never have been through destroy() -- a stack variable, a
    // member of a scene, a test's object. Its behaviors are heap-allocated and
    // the engine holds a record for it, and this is the last moment either can
    // be given back. Idempotent: after destroy() there is nothing left to do.
    objects::detail::release_behaviors(*this);
}

void Object::destroy() {
    if (!alive_) return; // twice is harmless, and happens
    alive_ = false;
    _end();
    // After the object's own _end, for the same reason a scene's objects go
    // after the scene's: the thing being torn down gets to speak first, while
    // everything it owns is still there.
    objects::detail::release_behaviors(*this);
    // Generation 0 is "points at nothing", so an object carrying it was never
    // spawned and owns no slot. Releasing index_ anyway frees SLOT ZERO --
    // whoever the scene happens to have put there -- and the game goes on
    // holding a reference to memory that has been handed back. It is reachable
    // from anything holding a plain rmp::Object: a member, a local, a test.
    if (generation_ == 0) return;
    objects::detail::mark_for_release(index_);
}

// ---------------------------------------------------------------------------
// Scene's half
// ---------------------------------------------------------------------------

void Scene::detail_spawn(std::unique_ptr<Object> owned, const ObjectOptions &options) {
    Object *made = owned.get(); // non-owning; the slot below takes `owned`
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
    // Never 0 again once a slot has been used, and the `if` is what makes that
    // true rather than nearly true: the counter is 32 bits and a slot reused a
    // thousand times a second reaches the end in about seven weeks. Landing on
    // 0 while the slot is OCCUPIED makes every handle to a live object resolve
    // to nullptr, with nothing logged and nothing to see.
    slot.generation += 1;
    if (slot.generation == 0) slot.generation = 1;
    slot.object = std::move(owned);

    made->scene_ = this;
    made->index_ = index;
    made->generation_ = slot.generation;
    made->alive_ = true;

    made->position = options.position;
    // Not {0,0}. The swept test reads the difference between this and the
    // current position, so a brand new object at (900, 400) would look like it
    // had crossed the whole world this frame -- and in a scene full of them,
    // every swept box would span from the origin and they would all "collide"
    // near it. Found by the differential test, which is the one place a wrong
    // answer of that shape cannot hide.
    made->previous_position_ = options.position;
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
    objects::detail::bump_world_version();
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
        RMP_REPORT_ONCE("SCENE: destroy() was given an object that belongs to "
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

// The anonymous-namespace one, reachable from the camera. Qualified, so it
// finds the file-scope function and not this wrapper.
Rectangle view_rect() { return rmp::view_rect(); }

void mark_for_release(unsigned index) {
    g_pending_free.push_back(index);
    bump_world_version();
}

unsigned world_version() { return g_world_version; }

void bump_world_version() { g_world_version++; }

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

// Returns whether the object was TELEPORTED -- moved somewhere it did not
// travel to. Only WRAP does that; CLAMP and BOUNCE shift it by the depth it had
// gone past the wall, which is a correction and not a jump. The caller uses it
// to re-mark the position, because the swept test reads the difference between
// where the object was and where it is, and a wrap is the width of the world.
bool apply_edges(Object &object) {
    if (object.edges == Edge::NONE) return false;

    // Empty bounds mean the MAP when the scene has one and the camera's VIEW
    // when it does not. That is the difference between a paddle in a
    // one-screen game, which wants the window, and a player in a Tiled level,
    // which wants the level -- and neither of them should have to say so. The
    // view and not the window, because a camera that has moved to x = 5000
    // makes "the window" a rectangle nothing on screen is inside.
    Rectangle area = object.bounds;
    if (empty_rect(area) && object.scene() != nullptr && object.scene()->map.valid()) {
        area = object.scene()->map.bounds();
    }
    if (empty_rect(area)) {
        area = object.scene() != nullptr ? object.scene()->camera.view() : view_rect();
    }
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
            bool wrapped = false;
            // Completely past a side, not merely touching it: an asteroid that
            // teleported the instant it grazed the edge would pop rather than
            // slide across.
            //
            // And it comes back JUST OUTSIDE the far edge, so it slides in the
            // way it slid out. Shifting by the width of the world instead put
            // it back INSIDE -- an object that left the left edge reappeared
            // some way in from the right, which reads as a jump. The test for
            // both axes is what said so.
            if (box.x + box.width < left) {
                object.position.x += right - box.x;
                wrapped = true;
            } else if (box.x > right) {
                object.position.x += left - (box.x + box.width);
                wrapped = true;
            }
            if (box.y + box.height < top) {
                object.position.y += bottom - box.y;
                wrapped = true;
            } else if (box.y > bottom) {
                object.position.y += top - (box.y + box.height);
                wrapped = true;
            }
            return wrapped;
        }
        case Edge::DESTROY: {
            // Completely outside, again for a concrete reason: a bullet fired
            // from a muzzle on the edge of the screen would die at birth if
            // touching the boundary were enough. And only after it has been
            // INSIDE once: a runner spawns its obstacles ahead of the camera,
            // outside the view, and every one of them used to die on its first
            // frame -- DESTROY means "it left", and nothing can leave a place
            // it has not been.
            const bool out = box.x + box.width < left || box.x > right ||
                box.y + box.height < top || box.y > bottom;
            // Where it was before this frame's integration counts as well: a
            // bullet spawned inside and fast enough to be outside by the time
            // the edges are checked has still been inside.
            const Vector2 before = Storage::previous_position(object);
            const float dx = before.x - object.position.x;
            const float dy = before.y - object.position.y;
            const bool was_out = box.x + dx + box.width < left || box.x + dx > right ||
                box.y + dy + box.height < top || box.y + dy > bottom;
            if (!out || !was_out) Storage::set_entered_bounds(object, true);
            if (out && Storage::entered_bounds(object)) object.destroy();
            break;
        }
        case Edge::NONE:
        default:
            break;
    }
    return false;
}

} // namespace

void update(Scene &scene, float delta) {
    const std::vector<unsigned> *start = indices_for(&scene);
    if (start == nullptr) return;

    // Everything in this pass is about to move, so whatever the collision grid
    // was built from is out of date before the first object runs.
    bump_world_version();

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

        // Behaviors first, then the object's own _update: the framework's
        // code runs and then yours, so yours has the last word on what the
        // frame leaves behind.
        update_behaviors(*object, delta);
        if (!object->alive()) continue;

        object->_update(delta);
        if (!object->alive()) continue; // it may have destroyed itself

        // AFTER _update and before the integration, which is the only place it
        // can go. The user's code is entitled to teleport an object by writing
        // position, and a teleport is not a sweep -- a game that moves
        // something across the map must not collide with everything on the line
        // between. Marking before _update swept the teleport too, and the test
        // named "a teleport is not a sweep" is what said so.
        Storage::remember_position(*object);

        // A frame with no time in it is not integrated AT ALL, and the reason
        // is the accumulator rather than the arithmetic: multiplying by zero
        // changes nothing, but TAKING the force empties it, so a push applied
        // before the first frame is silently spent on a frame that could not
        // use it. The frame time raylib reports is 0 on frame one, which is
        // exactly where an apply_force from _ready lands -- and that is why
        // Pong's and Breakout's ball each sat still. A negative delta is the same
        // refusal for the same money: running the integrator backwards is never
        // what a caller meant.
        if (delta > 0) {
            // Gravity and apply_force land in the same accumulator, so an
            // object with gravity_scale = 1 that you push upwards does what it
            // would do in the world. There is no separate path for gravity.
            const float m = object->mass > 0 ? object->mass : 1.0f;
            const Vector2 force = Storage::take_force(*object);
            Vector2 acceleration{ force.x / m, force.y / m };
            acceleration.x += scene.gravity.x * object->gravity_scale;
            acceleration.y += scene.gravity.y * object->gravity_scale;

            object->velocity.x += acceleration.x * delta;
            object->velocity.y += acceleration.y * delta;
            object->position.x += object->velocity.x * delta;
            object->position.y += object->velocity.y * delta;
        }

        // A wrap moves the object by the width of the world, and the swept test
        // reads the distance travelled: without re-marking, an asteroid coming
        // back on the far side is tested against the SEGMENT across the whole
        // level and collides with everything on it. Same argument as the
        // teleport above, one step later in the frame.
        if (apply_edges(*object)) Storage::remember_position(*object);
        if (!object->alive()) continue; // Edge::DESTROY

        // The animation clock, once, after the movement. Here and not in the
        // draw pass, because a scene that is updated but not drawn -- one
        // underneath a pause menu with updates_below on -- still has time
        // passing in it, and an animation that froze there would jump when the
        // menu closed.
        rmp::animation::detail::advance(object->sprite, delta);

        // After everything that moves it: this is where a behavior that
        // corrects the final position gets to run. See _late_update in
        // include/rmp/object.h for why it has to be here and not above.
        late_update_behaviors(*object, delta);
    }
}

std::vector<Object *> live_objects(const Scene &scene) {
    // By value and not through the shared scratch, because a raycast can be
    // issued from inside a _collision that is itself walking one of these, and
    // the inner call would otherwise pull the list out from under the outer.
    std::vector<Object *> out;
    const std::vector<unsigned> *indices = indices_for(&scene);
    if (indices == nullptr) return out;
    out.reserve(indices->size());
    for (unsigned index : *indices) {
        Cell *slot = slot_at(index);
        if (slot == nullptr || !slot->occupied || slot->object == nullptr) continue;
        if (slot->object->alive()) out.push_back(slot->object.get());
    }
    return out;
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
        draw_behaviors(*object);
        object->_draw();
    }
}

// The rectangle the sprite is showing right now: the sheet's current frame when
// there is a sheet, the explicit `source` when there is one, and the whole
// texture otherwise. One place, because the draw pass and world_bounds have to
// agree or the picture and the collider drift apart.
Rectangle sprite_source(const Sprite &sprite) {
    if (sprite.sheet.valid()) {
        const SheetData &data = sprite.sheet.raw();
        if (data.frame_count() > 0) {
            const int index = sprite.ours.frame < 0
                ? 0
                : (sprite.ours.frame >= data.frame_count() ? data.frame_count() - 1
                                                           : sprite.ours.frame);
            return data.frame(index).source;
        }
    }
    if (sprite.source.width > 0 && sprite.source.height > 0) return sprite.source;
    if (sprite.texture.valid()) {
        const Texture2D &tex = sprite.texture;
        return Rectangle{ 0, 0, static_cast<float>(tex.width),
                          static_cast<float>(tex.height) };
    }
    return Rectangle{};
}

void draw_one(Object &object) {
    if (object.sprite.sheet.valid()) {
        const SheetData &data = object.sprite.sheet.raw();
        Rectangle source = sprite_source(object.sprite);
        if (object.flip_x) source.width = -source.width;
        if (object.flip_y) source.height = -source.height;

        float w = object.sprite.size.x > 0 ? object.sprite.size.x
                                           : static_cast<float>(data.width);
        float h = object.sprite.size.y > 0 ? object.sprite.size.y
                                           : static_cast<float>(data.height);
        w *= object.scale.x;
        h *= object.scale.y;
        const Rectangle dest{ object.position.x, object.position.y, w, h };
        const Vector2 origin{ object.sprite.origin.x * w, object.sprite.origin.y * h };
        DrawTexturePro(data.texture, source, dest, origin, object.rotation,
                       object.sprite.tint);
        return;
    }
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
            // The same one axis world_bounds() and the collider use.
            const float r = object.shape.radius * circle_scale(object.scale);
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
        // free -- with or without anything ever taking the slot again. Zero is
        // skipped for the same reason it is on spawn, and here it is worse: 0
        // plus the next spawn's 1 is the generation the slot's FIRST object
        // had, so a handle nobody has touched since then comes back to life
        // pointing at somebody else.
        slot->generation += 1;
        if (slot->generation == 0) slot->generation = 1;
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
    bump_world_version();
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
    bump_world_version();
}

void reset_for_tests() {
    g_by_scene.clear();
    g_pending_free.clear();
    g_draw_order.clear();
    g_free.clear();
    g_slots.clear();
    bump_world_version();
}

int live_count() {
    int n = 0;
    for (const Cell &slot : g_slots) {
        if (slot.occupied && slot.object != nullptr && slot.object->alive()) n++;
    }
    return n;
}

int slot_count() { return static_cast<int>(g_slots.size()); }

void set_generation_for_tests(unsigned index, unsigned generation) {
    Cell *slot = slot_at(index);
    if (slot == nullptr) return;
    slot->generation = generation;
}

} // namespace objects::detail

} // namespace rmp
