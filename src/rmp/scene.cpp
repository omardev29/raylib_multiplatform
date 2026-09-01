// ===========================================================================
// rmp::Scene — see include/rmp/scene.h for what a scene is, and
// src/rmp/scene_internal.h for the half the app calls.
//
// The stack is a vector of owning pointers and the transitions are a queue.
// That is the whole data structure, and it is deliberately that small: every
// interesting property of this file is an ORDERING property, not an
// algorithmic one, which is why tests/scene_test.cpp checks a transcript of
// events rather than a return value.
// ===========================================================================

#include <rmp/scene.h>

#include "object_internal.h"
#include "scene_internal.h"

#include <rmp/input.h>
#include <rmp/ui.h>

#include <memory>
#include <utility>
#include <vector>

namespace rmp {

namespace {

// The stack. Index 0 is the bottom, back() is what the player is looking at.
std::vector<std::unique_ptr<Scene>> g_stack;

enum class Op { CHANGE, PUSH, REPLACE, POP };

struct Pending {
    Op op;
    std::unique_ptr<Scene> next; // null for POP
};

// Everything change/push/replace/pop recorded this frame, in the order it was
// asked for. More than one in a frame is unusual but well defined: they are
// applied in that order, so `push(A); pop();` leaves the stack as it was.
std::vector<Pending> g_pending;

bool g_running = false;

// Ownership arrives raw from the template in rmp/scene.h — that header cannot
// afford <memory>, and this file can — and is wrapped here, on the first line,
// so that everything downstream of this point is owning.
void record(Op op, Scene *next) {
    g_pending.push_back(Pending{ op, std::unique_ptr<Scene>(next) });
}

} // namespace

// ---------------------------------------------------------------------------
// The public navigation surface. Every one of these RECORDS and returns; see
// the comment in rmp/scene.h for why doing the work here would be a bug.
// ---------------------------------------------------------------------------

void Scene::detail_change(Scene *next) { record(Op::CHANGE, next); }
void Scene::detail_push(Scene *next) { record(Op::PUSH, next); }
void Scene::detail_replace(Scene *next) { record(Op::REPLACE, next); }
void Scene::pop() { record(Op::POP, nullptr); }

int Scene::depth() { return static_cast<int>(g_stack.size()); }

// current() is a reference because there is always a scene while the app runs,
// and returning a pointer would invite a null check that can never fire. The
// one case where the stack really is empty is a programming error — calling it
// before RMP_GAME has started or after shutdown — and it gets a scene that does
// nothing rather than a crash in someone's release build.
Scene &Scene::current() {
    static Scene fallback;
    if (g_stack.empty()) {
        TraceLog(LOG_WARNING,
                 "SCENE: current() with an empty stack — is this before "
                 "RMP_GAME started, or after the app closed?");
        return fallback;
    }
    return *g_stack.back();
}

} // namespace rmp

namespace rmp::scenes::detail {

namespace {

// The lowest scene that still takes part in a pass, found by walking down from
// the top for as long as each scene lets the one below it through. Returns an
// index into g_stack; the stack is never empty when this is called.
template <class Policy> int lowest_participant(Policy lets_through) {
    int i = static_cast<int>(g_stack.size()) - 1;
    while (i > 0 && lets_through(*g_stack[static_cast<size_t>(i)])) i--;
    return i;
}

// Can input reach the scene at `index`? Only if every scene above it lets it
// through. Walking upwards rather than remembering a flag means a stack three
// deep answers correctly without anything having to be kept in step.
bool reachable_by_input(int index) {
    const int top = static_cast<int>(g_stack.size()) - 1;
    for (int above = index + 1; above <= top; above++) {
        if (!g_stack[static_cast<size_t>(above)]->input_below) return false;
    }
    return true;
}

void end_top() {
    g_stack.back()->_end();
    // The scene's objects go with it, each getting its _end(), and they go
    // AFTER the scene's own _end() so that a scene tidying up can still walk
    // what it spawned. This is the same ordering argument as rmp/app.h's
    // teardown list, and for the same reason: an object may hold an
    // rmp::Texture, and its destructor has to find its slot still there.
    rmp::objects::detail::release_scene(*g_stack.back());
    g_stack.pop_back();
}

void enter(std::unique_ptr<Scene> next) {
    g_stack.push_back(std::move(next));
    g_stack.back()->_ready();
}

} // namespace

void start(Scene *first) {
    g_running = true;
    if (first == nullptr) {
        TraceLog(LOG_ERROR, "SCENE: RMP_GAME was given a null scene");
        return;
    }
    enter(std::unique_ptr<Scene>(first));
}

bool running() { return g_running; }

void update(float delta) {
    if (g_stack.empty()) return;
    const int lowest = lowest_participant([](const Scene &s) { return s.updates_below; });
    const int top = static_cast<int>(g_stack.size()) - 1;
    // Bottom upwards: the world moves before whatever is layered on top of it
    // reacts to where the world ended up.
    for (int i = lowest; i <= top; i++) {
        // The other half of Scene::input_below, and the half that matters for
        // gameplay: a scene that is still running under a HUD that took the
        // input reads every action as false, so `if (just_pressed("fire"))`
        // simply does not fire. Nothing in that scene says so.
        rmp::input::detail::set_layer_input(reachable_by_input(i));
        Scene &scene = *g_stack[static_cast<size_t>(i)];
        scene._update(delta);
        // The scene first, then its objects: the scene sets up the frame and
        // the objects move inside it. Documented in
        // next_architecture/03-app-and-scenes.md and asserted in
        // tests/scene_test.cpp, because every interesting property of this is
        // an ordering property.
        rmp::objects::detail::update(scene, delta);
    }
    rmp::input::detail::set_layer_input(true);
}

Color clear_color() {
    if (g_stack.empty()) return rmp::ui::current_theme().background;
    const int lowest = lowest_participant([](const Scene &s) { return s.draws_below; });
    const Color c = g_stack[static_cast<size_t>(lowest)]->background;
    // BLANK is the "I did not choose" value rather than a real colour, because
    // a fully transparent clear is never what a scene means and every other
    // colour is.
    const bool unset = c.r == 0 && c.g == 0 && c.b == 0 && c.a == 0;
    return unset ? rmp::ui::current_theme().background : c;
}

void draw() {
    if (g_stack.empty()) return;
    const int lowest = lowest_participant([](const Scene &s) { return s.draws_below; });
    const int top = static_cast<int>(g_stack.size()) - 1;

    for (int i = lowest; i <= top; i++) {
        // Input belongs to the top scene unless it says otherwise, and this is
        // where that becomes true rather than a comment: a UI pass with the
        // pointer suppressed lays out and draws exactly as it would, and no
        // widget in it can be hovered or clicked. A HUD under an open pause
        // menu is drawn, is not interactive, and neither scene wrote a line
        // about it.
        rmp::ui::detail::set_pass_input(reachable_by_input(i));
        Scene &scene = *g_stack[static_cast<size_t>(i)];
        // Objects first, in world space, and the scene's own _draw() after, in
        // screen space: that is what puts the HUD over the game rather than
        // under it, without either one saying so.
        rmp::objects::detail::draw(scene);
        scene._draw();
    }
    rmp::ui::detail::set_pass_input(true);
}

void apply_pending() {
    // Objects first, and unconditionally: a frame with no scene transition
    // still has bullets to bury, and an early return on g_pending would have
    // leaked every one of them. It is the sort of bug that only shows up as a
    // number going up.
    rmp::objects::detail::collect();

    if (g_pending.empty()) return;

    // Take the queue rather than iterating it: _ready() and _end() are the
    // user's code and are entitled to queue transitions of their own, and those
    // belong to the NEXT frame. Without the swap, one of them pushing here
    // would reallocate the vector being walked.
    std::vector<Pending> batch;
    batch.swap(g_pending);

    for (Pending &p : batch) {
        switch (p.op) {
            case Op::CHANGE:
                // Top down, so a scene is never ended while something it put on the
                // stack is still above it.
                while (!g_stack.empty()) end_top();
                enter(std::move(p.next));
                break;

            case Op::PUSH:
                if (!g_stack.empty()) g_stack.back()->_suspend();
                enter(std::move(p.next));
                break;

            case Op::REPLACE:
                // Only the top changes. What is underneath was suspended when the
                // top went on and stays suspended, so it gets no _resume().
                if (!g_stack.empty()) end_top();
                enter(std::move(p.next));
                break;

            case Op::POP:
                if (g_stack.size() <= 1) {
                    // Refusing beats obeying. An empty stack is a black window on
                    // desktop and, on iOS, a screen with no way back — and quit()
                    // is right there and says what it means.
                    TraceLog(LOG_WARNING,
                             "SCENE: pop() with %d scene(s) on the stack would "
                             "leave nothing to draw; ignoring. Did you mean "
                             "rmp::app::quit()?",
                             static_cast<int>(g_stack.size()));
                    break;
                }
                end_top();
                g_stack.back()->_resume();
                break;
        }
    }
}

void shutdown() {
    // Anything still queued is dropped: the app is going away, and running a
    // _ready() for a scene nobody will ever see would only give it a chance to
    // load assets that are about to be released.
    g_pending.clear();
    while (!g_stack.empty()) end_top();
    g_running = false;
}

} // namespace rmp::scenes::detail
