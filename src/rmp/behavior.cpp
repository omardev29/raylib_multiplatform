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
#include <deque>
#include <vector>

namespace rmp {

namespace {

struct Attached {
    const void *type = nullptr;
    void *data = nullptr;
    const detail::BehaviorOps *ops = nullptr;
    // Removed while the list was being walked. The entry stays where it is
    // until the walk is over -- see detach() for why erasing there cost the
    // next behavior its frame -- and is taken out by compact() afterwards.
    bool dead = false;
};

struct Owner {
    Object *object = nullptr; // null when the record is free
    std::vector<Attached> list;
    int walking = 0; // how many passes are inside this list right now
};

// One record per object that has ever had a behavior, found by the index the
// object carries. It used to be a vector keyed by the object's ADDRESS, scanned
// linearly, and both halves of that were wrong:
//
//   the SCAN made every lookup O(owners), and there is one per behavior per
//   pass -- so a frame cost O(objects * behaviors * owners) and 2 000 objects
//   with one behavior each took 17 ms, a whole frame, in the engine alone;
//
//   the ADDRESS made a record outlive its object. Nothing released one from
//   ~Object, so the next object the allocator put at that address inherited a
//   dead object's behaviors, state and all.
//
// A deque and not a vector because the records must not move: a pass holds a
// pointer to one across the user's _update, which is entitled to add a behavior
// to another object. Free records are recycled through g_free_owners.
// Behind a function rather than at file scope because a std::deque allocates in
// its constructor: an exception during static initialisation is one nobody can
// catch, and clang-tidy says so. Built on first use, which is when the first
// behavior is added.
std::deque<Owner> &owners() {
    static std::deque<Owner> g_owners;
    return g_owners;
}
std::vector<int> g_free_owners;

Owner *owner_of(const Object *object) {
    if (object == nullptr) return nullptr;
    const int slot = Storage::behavior_slot(*object);
    if (slot < 0) return nullptr;
    return &owners()[static_cast<std::size_t>(slot)];
}

Owner &owner_for(Object *object) {
    if (Owner *found = owner_of(object)) return *found;

    int slot = 0;
    if (!g_free_owners.empty()) {
        slot = g_free_owners.back();
        g_free_owners.pop_back();
    } else {
        slot = static_cast<int>(owners().size());
        owners().emplace_back();
    }

    Owner &owner = owners()[static_cast<std::size_t>(slot)];
    owner.object = object;
    owner.list.clear();
    owner.walking = 0;
    Storage::set_behavior_slot(*object, slot);
    return owner;
}

// Take out what detach() marked. Only ever called with no pass inside the list.
void compact(Owner &owner) {
    const auto gone =
        std::ranges::remove_if(owner.list, [](const Attached &a) { return a.dead; });
    owner.list.erase(gone.begin(), gone.end());
}

// The end of one pass over an object's behaviors. The owner is looked up again
// rather than remembered, because the pass may have destroyed the object -- and
// then the record is free, possibly already somebody else's, and the one thing
// that must not happen is decrementing a counter in it.
void end_walk(const Object &object) {
    Owner *owner = owner_of(&object);
    if (owner == nullptr) return;
    if (owner->walking > 0) owner->walking--;
    if (owner->walking == 0) compact(*owner);
}

} // namespace

namespace detail {

void *attach(Object &self, const void *type, const BehaviorOps &ops, void *data) {
    // Adding the same behavior twice replaces it rather than stacking two,
    // because two of the same is never what anybody means and the second
    // get<B>() could only return one of them anyway.
    detach(self, type);

    Owner &owner = owner_for(&self);
    owner.list.push_back(Attached{ type, data, &ops, false });
    if (ops.ready != nullptr) ops.ready(data, self);
    return data;
}

void *find_behavior(const Object &self, const void *type) {
    Owner *owner = owner_of(&self);
    if (owner == nullptr) return nullptr;
    for (const Attached &a : owner->list) {
        if (a.type == type && !a.dead) return a.data;
    }
    return nullptr;
}

void detach(Object &self, const void *type) {
    Owner *owner = owner_of(&self);
    if (owner == nullptr) return;
    for (std::size_t i = 0; i < owner->list.size(); i++) {
        if (owner->list[i].type != type || owner->list[i].dead) continue;
        const Attached a = owner->list[i];
        // Out of the list BEFORE _end runs: the user's code is entitled to
        // remove the same behavior again from in there, and finding it gone is
        // better than freeing it twice.
        //
        // MARKED and not erased while a pass is inside the list. Erasing from
        // the middle shifts every later entry down by one and the walk's i++
        // then steps over one of them -- so a behavior that removes itself from
        // its own _update, which is what a one-shot intro or a state-machine
        // step IS, silently cost its neighbour a frame.
        if (owner->walking > 0) {
            owner->list[i].dead = true;
            owner->list[i].type = nullptr;
            owner->list[i].data = nullptr;
        } else {
            owner->list.erase(owner->list.begin() + static_cast<std::ptrdiff_t>(i));
        }
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
    owner->walking++;
    for (std::size_t i = 0; i < count; i++) {
        // Looked up again every iteration: _update is the user's code, it can
        // add or remove behaviors and it can spawn objects, and any of those
        // moves the vectors underneath.
        Owner *live = owner_of(&object);
        if (live == nullptr || i >= live->list.size()) break;
        const Attached a = live->list[i];
        if (a.dead) continue;
        if (a.ops->update != nullptr) a.ops->update(a.data, object, delta);
        if (!object.alive()) break;
    }
    end_walk(object);
}

void late_update_behaviors(Object &object, float delta) {
    Owner *owner = owner_of(&object);
    if (owner == nullptr) return;
    const std::size_t count = owner->list.size();
    owner->walking++;
    for (std::size_t i = 0; i < count; i++) {
        Owner *live = owner_of(&object);
        if (live == nullptr || i >= live->list.size()) break;
        const Attached a = live->list[i];
        if (a.dead) continue;
        if (a.ops->late_update != nullptr) a.ops->late_update(a.data, object, delta);
        if (!object.alive()) break;
    }
    end_walk(object);
}

void draw_behaviors(Object &object) {
    Owner *owner = owner_of(&object);
    if (owner == nullptr) return;
    const std::size_t count = owner->list.size();
    owner->walking++;
    for (std::size_t i = 0; i < count; i++) {
        Owner *live = owner_of(&object);
        if (live == nullptr || i >= live->list.size()) break;
        const Attached a = live->list[i];
        if (a.dead) continue;
        if (a.ops->draw != nullptr) a.ops->draw(a.data, object);
    }
    end_walk(object);
}

void collide_behaviors(Object &object, Object &other) {
    Owner *owner = owner_of(&object);
    if (owner == nullptr) return;
    const std::size_t count = owner->list.size();
    owner->walking++;
    for (std::size_t i = 0; i < count; i++) {
        Owner *live = owner_of(&object);
        if (live == nullptr || i >= live->list.size()) break;
        const Attached a = live->list[i];
        if (a.dead) continue;
        if (a.ops->collision != nullptr) a.ops->collision(a.data, object, other);
        if (!object.alive() || !other.alive()) break;
    }
    end_walk(object);
}

void release_behaviors(Object &object) {
    const int slot = Storage::behavior_slot(object);
    if (slot < 0) return;
    Owner &owner = owners()[static_cast<std::size_t>(slot)];

    // The object lets go of the record FIRST, so anything the _end callbacks do
    // -- including asking this object for a behavior, or destroying it again --
    // finds nothing attached rather than a list being emptied underneath.
    Storage::set_behavior_slot(object, -1);
    const std::vector<Attached> taken = owner.list;
    owner.list.clear();
    owner.object = nullptr;
    owner.walking = 0;
    g_free_owners.push_back(slot);

    for (const Attached &a : taken) {
        if (a.dead) continue; // detach() already ran its _end and freed it
        if (a.ops->end != nullptr) a.ops->end(a.data, object);
        a.ops->destroy(a.data);
    }
}

int behavior_count(const Object &object) {
    const Owner *owner = owner_of(&object);
    if (owner == nullptr) return 0;
    int n = 0;
    for (const Attached &a : owner->list) {
        if (!a.dead) n++;
    }
    return n;
}

void reset_behaviors_for_tests() {
    // Whole objects are torn down by reset_for_tests(), which calls
    // release_behaviors for each, and ~Object does it for everything else. This
    // is the belt: it frees whatever is still attached and, crucially, tells
    // each object its record is gone -- a live object left pointing at a record
    // that has been recycled would reach somebody else's behaviors.
    for (Owner &owner : owners()) {
        if (owner.object != nullptr) Storage::set_behavior_slot(*owner.object, -1);
        for (const Attached &a : owner.list) {
            if (!a.dead) a.ops->destroy(a.data);
        }
        owner.list.clear();
        owner.object = nullptr;
        owner.walking = 0;
    }
    owners().clear();
    g_free_owners.clear();
}

} // namespace objects::detail

} // namespace rmp
