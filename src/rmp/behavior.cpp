// ---------------------------------------------------------------------------
// The behavior engine: storage, ordering, and the four optional hooks.
//
// The model is in the comment above Object::add in include/rmp/object.h, and
// the whole of it is: a behavior is any struct with
// `void _update(rmp::Object &, float)`. No base class, no virtuals, no
// registration. What lives here is the bookkeeping that makes that possible.
//
// WHERE THE DATA LIVES, and why it is not inside the object.
//
// next_architecture/05-behaviors.md says a behavior that is trivially copyable
// and fits in 64 bytes should live inside the object with no allocation. It is
// not built that way, and the reason is the shape of the objects rather than a
// shortcut: most objects in a 2D game have NO behaviors at all -- bullets,
// coins, particles, pickups, tiles, the walls of a level -- so a fixed arena in
// rmp::Object would make the common object bigger to save a malloc for the
// uncommon one. Behaviors are added in _ready and not per frame, so the
// allocation happens once per object that has one.
//
// What the architecture was really protecting is the CONTRACT, and that is
// kept exactly: add<B>() returns B&, get<B>() returns B*, and both stay valid
// for as long as the behavior is attached -- the data is heap-allocated and
// never moves, so growing the slot list cannot invalidate a reference the way
// an inline arena in a vector would. If a real game ever measures a reason, the
// inline path goes in behind that unchanged API and nothing above this file
// notices, which is the same argument object.cpp makes about per-type pools.
// ---------------------------------------------------------------------------

#include <rmp/object.h>
#include <rmp/scene.h>

#include "object_internal.h"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace rmp {

namespace {

struct Attached {
    const void *type = nullptr;
    void *data = nullptr;
    const detail::BehaviorOps *ops = nullptr;
};

struct Owner {
    const Object *object = nullptr;
    std::vector<Attached> list;
};

// Keyed by object pointer and kept in a vector rather than a hash map, for the
// same two reasons the scene list is: iterating a hash of pointers is
// nondeterministic, and the number of objects WITH behaviors is small next to
// the number of objects. A linear scan over the ones that have any beats
// hashing every lookup.
std::vector<Owner> g_owners;

Owner *owner_of(const Object *object) {
    for (Owner &o : g_owners) {
        if (o.object == object) return &o;
    }
    return nullptr;
}

Owner &owner_for(const Object *object) {
    if (Owner *found = owner_of(object)) return *found;
    g_owners.push_back(Owner{ object, {} });
    return g_owners.back();
}

} // namespace

namespace detail {

void *attach(Object &self, const void *type, const BehaviorOps &ops, void *data) {
    // Adding the same behavior twice replaces it rather than stacking two,
    // because two of the same is never what anybody means and the second
    // get<B>() could only return one of them anyway.
    detach(self, type);

    Owner &owner = owner_for(&self);
    owner.list.push_back(Attached{ type, data, &ops });
    if (ops.ready != nullptr) ops.ready(data, self);
    return data;
}

void *find_behavior(const Object &self, const void *type) {
    Owner *owner = owner_of(&self);
    if (owner == nullptr) return nullptr;
    for (const Attached &a : owner->list) {
        if (a.type == type) return a.data;
    }
    return nullptr;
}

void detach(Object &self, const void *type) {
    Owner *owner = owner_of(&self);
    if (owner == nullptr) return;
    for (std::size_t i = 0; i < owner->list.size(); i++) {
        if (owner->list[i].type != type) continue;
        const Attached a = owner->list[i];
        // Out of the list BEFORE _end runs: the user's code is entitled to
        // remove the same behavior again from in there, and finding it gone is
        // better than freeing it twice.
        owner->list.erase(owner->list.begin() + static_cast<std::ptrdiff_t>(i));
        if (a.ops->end != nullptr) a.ops->end(a.data, self);
        a.ops->destroy(a.data);
        return;
    }
}

} // namespace detail

namespace objects::detail {

void update_behaviors(Object &object, float delta) {
    Owner *owner = owner_of(&object);
    if (owner == nullptr) return;

    // A snapshot of the count, so a behavior added from inside an _update gets
    // its _ready now and its first _update next frame. Adding to the list being
    // walked is the classic way to invalidate an iterator, and deferring it is
    // cheaper than explaining when what happens.
    const std::size_t count = owner->list.size();
    for (std::size_t i = 0; i < count; i++) {
        // Looked up again every iteration: _update is the user's code, it can
        // add or remove behaviors and it can spawn objects, and any of those
        // moves the vectors underneath.
        Owner *live = owner_of(&object);
        if (live == nullptr || i >= live->list.size()) return;
        const Attached a = live->list[i];
        if (a.ops->update != nullptr) a.ops->update(a.data, object, delta);
        if (!object.alive()) return;
    }
}

void late_update_behaviors(Object &object, float delta) {
    Owner *owner = owner_of(&object);
    if (owner == nullptr) return;
    const std::size_t count = owner->list.size();
    for (std::size_t i = 0; i < count; i++) {
        Owner *live = owner_of(&object);
        if (live == nullptr || i >= live->list.size()) return;
        const Attached a = live->list[i];
        if (a.ops->late_update != nullptr) a.ops->late_update(a.data, object, delta);
        if (!object.alive()) return;
    }
}

void draw_behaviors(Object &object) {
    Owner *owner = owner_of(&object);
    if (owner == nullptr) return;
    const std::size_t count = owner->list.size();
    for (std::size_t i = 0; i < count; i++) {
        Owner *live = owner_of(&object);
        if (live == nullptr || i >= live->list.size()) return;
        const Attached a = live->list[i];
        if (a.ops->draw != nullptr) a.ops->draw(a.data, object);
    }
}

void collide_behaviors(Object &object, Object &other) {
    Owner *owner = owner_of(&object);
    if (owner == nullptr) return;
    const std::size_t count = owner->list.size();
    for (std::size_t i = 0; i < count; i++) {
        Owner *live = owner_of(&object);
        if (live == nullptr || i >= live->list.size()) return;
        const Attached a = live->list[i];
        if (a.ops->collision != nullptr) a.ops->collision(a.data, object, other);
        if (!object.alive() || !other.alive()) return;
    }
}

void release_behaviors(Object &object) {
    Owner *owner = owner_of(&object);
    if (owner == nullptr) return;

    // A copy, and the list emptied first: _end is the user's code and it is
    // entitled to touch this object, including removing another behavior.
    const std::vector<Attached> taken = owner->list;
    owner->list.clear();
    for (const Attached &a : taken) {
        if (a.ops->end != nullptr) a.ops->end(a.data, object);
        a.ops->destroy(a.data);
    }

    const auto gone = std::ranges::remove_if(
        g_owners, [&object](const Owner &o) { return o.object == &object; });
    g_owners.erase(gone.begin(), gone.end());
}

int behavior_count(const Object &object) {
    const Owner *owner = owner_of(&object);
    return owner == nullptr ? 0 : static_cast<int>(owner->list.size());
}

void reset_behaviors_for_tests() {
    // Whole objects are torn down by reset_for_tests(), which calls
    // release_behaviors for each. This is the belt for anything that outlived
    // its object -- a test that built an Object on the stack, for instance,
    // which is exactly how a behavior is meant to be tested.
    const std::vector<Owner> taken = g_owners;
    g_owners.clear();
    for (const Owner &owner : taken) {
        for (const Attached &a : owner.list) a.ops->destroy(a.data);
    }
}

} // namespace objects::detail

} // namespace rmp
