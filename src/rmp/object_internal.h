#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. The object storage as the scene drives it.
//
// The public surface is include/rmp/object.h. Everything here is the half the
// user never calls: run the objects of one scene, draw them, release what the
// frame destroyed, tear a scene's objects down.
//
// tests/object_test.cpp includes this directly, which is what lets the whole
// life cycle be tested without a window -- update(), collect() and the handle
// arithmetic touch nothing but memory. draw() is the only one that would touch
// the GPU, and the test drives everything except it.
// ---------------------------------------------------------------------------

#include <vector>

namespace rmp {
class Object;
class Scene;
} // namespace rmp

namespace rmp::objects::detail {

// One scene's objects: each _update(delta), then the integration of velocity
// (gravity and any accumulated force), then the edge rules. In that order,
// because the edges have to see where the object actually ended up.
void update(Scene &scene, float delta);

// One scene's objects, by layer and then by creation order. Calls the sprite or
// shape first and the object's own _draw() after, so an override adds to what
// is already there instead of replacing it.
void draw(Scene &scene);

// The draw pass minus the drawing: which objects take part, in which order.
// Split out so the ordering can be tested without a GL context -- raylib's
// DrawRectanglePro() reaches into a render batch that InitWindow() creates, and
// calling it before there is one is a segfault, not a failed assertion. This is
// the same seam rmp::ui has for the same reason.
const std::vector<Object *> &draw_order(Scene &scene);

// What one object looks like. Split out of draw() so a scene that wants to
// place a single object by hand can, and so the draw pass reads as a loop.
void draw_one(Object &object);

// Release everything destroy() marked during the frame. Runs after
// EndDrawing(), with the scene transitions, so nothing is freed while it is on
// the stack of the call that asked for it.
void collect();

// Called by Object::destroy(). Records the slot; the memory goes in collect().
void mark_for_release(unsigned index);

// Destroy every object of a scene that is leaving, running each _end(). Called
// from the scene stack's own teardown, while the window is still open, because
// an object can hold an rmp::Texture and its destructor has to find its slot in
// the resource table still there.
void release_scene(Scene &scene);

// --- the test seam ---------------------------------------------------------
// Global storage means one test leaks into the next unless something resets it,
// and a test suite whose result depends on its own order is worse than no test
// suite. tests/object_test.cpp calls this between cases.
void reset_for_tests();

// Live objects across every scene, and how many slots the storage has ever
// needed. The second one is how the reuse tests prove a slot was reused rather
// than assuming it: if the count did not grow, the new object took the old
// one's place.
int live_count();
int slot_count();

} // namespace rmp::objects::detail
