// ===========================================================================
// The scene stack.
//
// Every interesting property of a scene stack is an ORDERING property, not an
// algorithmic one: which callback runs, on which scene, in what order, and how
// many times. So these tests barely look at return values. Each test scene
// appends its name and the event to one transcript, and the test compares the
// whole transcript with a string.
//
// That shape is deliberate. An assertion per event passes for the wrong reason
// as soon as an extra callback creeps in — a _suspend that fires twice, a
// _resume on a scene that was never suspended — because nothing was checking
// for events that should NOT be there. A whole-transcript comparison cannot
// miss one.
//
// Not one InitWindow in this file. The stack is arithmetic and callbacks.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/scene_internal.h"

#include <rmp/scene.h>

#include <string>

namespace {

std::string g_log;

void note(const char *name, const char *event) {
    if (!g_log.empty()) g_log += ' ';
    g_log += name;
    g_log += '.';
    g_log += event;
}

// A scene that writes down everything that happens to it. The name is a
// template parameter so that each one is a distinct type — Scene::change<T>()
// takes a type, so the test needs as many types as it has scenes.
template <char Name> class Probe : public rmp::Scene {
public:
    static constexpr char kName[2] = { Name, '\0' };

    void _ready() override { note(kName, "ready"); }
    void _end() override { note(kName, "end"); }
    void _suspend() override { note(kName, "suspend"); }
    void _resume() override { note(kName, "resume"); }
    void _update(float /*delta*/) override { note(kName, "update"); }
    void _draw() override { note(kName, "draw"); }
};

using A = Probe<'A'>;
using B = Probe<'B'>;
using C = Probe<'C'>;

// One turn of the loop, minus the drawing calls the app makes around it. The
// order is the one in src/rmp/app.cpp, and keeping the two in step is what
// makes this file a test of the real thing rather than of itself.
void run_frame() {
    rmp::scenes::detail::update(1.0f / 60.0f);
    rmp::scenes::detail::draw();
    rmp::scenes::detail::apply_pending();
}

// Every test starts from nothing. shutdown() on an already-empty stack is a
// no-op, so this is safe even if a test left early.
struct Fixture {
    Fixture() {
        rmp::scenes::detail::shutdown();
        g_log.clear();
    }
    ~Fixture() {
        rmp::scenes::detail::shutdown();
        g_log.clear();
    }
};

// Start with `first` on the stack and the transcript cleared, so that a test
// reads as "given a running stack, when X, then the transcript is Y".
template <class T> void start_clean() {
    rmp::scenes::detail::start(new T());
    g_log.clear();
}

} // namespace

TEST_SUITE("scenes") {
    TEST_CASE("the first scene is ready before the first frame") {
        Fixture fix;
        rmp::scenes::detail::start(new A());
        // Not deferred, and it cannot be: the first frame draws, and drawing a
        // scene whose _ready has not run would be drawing uninitialised state.
        CHECK(g_log == "A.ready");
        CHECK(rmp::Scene::depth() == 1);
        CHECK(rmp::scenes::detail::running());
    }

    TEST_CASE("a frame is update then draw, and nothing else") {
        Fixture fix;
        start_clean<A>();
        run_frame();
        CHECK(g_log == "A.update A.draw");
    }

    TEST_CASE("push suspends what is under it, pop resumes it") {
        Fixture fix;
        start_clean<A>();

        rmp::Scene::push<B>();
        // Still nothing: the transition is queued, not applied.
        CHECK(g_log.empty());
        CHECK(rmp::Scene::depth() == 1);

        rmp::scenes::detail::apply_pending();
        CHECK(g_log == "A.suspend B.ready");
        CHECK(rmp::Scene::depth() == 2);

        g_log.clear();
        rmp::Scene::pop();
        rmp::scenes::detail::apply_pending();
        CHECK(g_log == "B.end A.resume");
        CHECK(rmp::Scene::depth() == 1);
    }

    TEST_CASE("a transition asked for mid-frame happens after the frame") {
        Fixture fix;

        // The case the deferral exists for: a scene that changes away from itself
        // inside its own _draw, which is where `if (button("Play")) change<Game>()`
        // lives. It has to survive the rest of that frame.
        class Leaver : public rmp::Scene {
        public:
            void _update(float /*delta*/) override {
                note("L", "update");
                rmp::Scene::change<A>();
            }
            void _draw() override { note("L", "draw"); }
            void _end() override { note("L", "end"); }
        };

        rmp::scenes::detail::start(new Leaver());
        g_log.clear();
        run_frame();

        // Its _draw still ran, and its _end came after it — not in the middle of
        // the update that asked for it.
        CHECK(g_log == "L.update L.draw L.end A.ready");
    }

    TEST_CASE("change ends the whole stack, top down") {
        Fixture fix;
        start_clean<A>();
        rmp::Scene::push<B>();
        rmp::Scene::push<C>();
        rmp::scenes::detail::apply_pending();

        g_log.clear();
        rmp::Scene::change<A>();
        rmp::scenes::detail::apply_pending();

        // Top down, so no scene is ended while something it put on the stack is
        // still above it.
        CHECK(g_log == "C.end B.end A.end A.ready");
        CHECK(rmp::Scene::depth() == 1);
    }

    TEST_CASE("replace touches only the top, and does not resume what is under it") {
        Fixture fix;
        start_clean<A>();
        rmp::Scene::push<B>();
        rmp::scenes::detail::apply_pending();

        g_log.clear();
        rmp::Scene::replace<C>();
        rmp::scenes::detail::apply_pending();

        // A stays suspended. It was suspended when B went on and nothing has come
        // off since, so a _resume here would be a lie.
        CHECK(g_log == "B.end C.ready");
        CHECK(rmp::Scene::depth() == 2);
    }

    TEST_CASE("popping the last scene is refused") {
        Fixture fix;
        start_clean<A>();
        rmp::Scene::pop();
        rmp::scenes::detail::apply_pending();

        // An empty stack is a black window on desktop and, on iOS, a screen with
        // no way back. Refusing beats obeying.
        CHECK(g_log.empty());
        CHECK(rmp::Scene::depth() == 1);
    }

    TEST_CASE("several transitions in one frame apply in the order they were asked") {
        Fixture fix;
        start_clean<A>();
        rmp::Scene::push<B>();
        rmp::Scene::pop();
        rmp::scenes::detail::apply_pending();

        CHECK(g_log == "A.suspend B.ready B.end A.resume");
        CHECK(rmp::Scene::depth() == 1);
    }

    TEST_CASE("a transition queued from _ready belongs to the next frame") {
        Fixture fix;

        class Eager : public rmp::Scene {
        public:
            void _ready() override {
                note("E", "ready");
                rmp::Scene::push<A>();
            }
            void _suspend() override { note("E", "suspend"); }
            void _end() override { note("E", "end"); }
        };

        rmp::scenes::detail::start(new Eager());
        CHECK(g_log == "E.ready");

        // Applying inside apply_pending() would mean recursing into the vector it
        // is walking. It waits.
        g_log.clear();
        rmp::scenes::detail::apply_pending();
        CHECK(g_log == "E.suspend A.ready");
    }

} // TEST_SUITE

TEST_SUITE("scene policies") {
    // A probe that lets things through. Written as a type per policy because the
    // navigation functions take types, not instances.
    template <char Name, bool Updates, bool Draws> class Veil : public Probe<Name> {
    public:
        Veil() {
            this->updates_below = Updates;
            this->draws_below = Draws;
        }
    };

    TEST_CASE("the defaults are a pause menu, with no policy written") {
        Fixture fix;
        start_clean<A>();
        rmp::Scene::push<B>(); // B takes every default
        rmp::scenes::detail::apply_pending();

        g_log.clear();
        run_frame();

        // A does not update — the world is frozen — but it is still drawn, so the
        // player can see what they paused. B updates because B is the top scene;
        // the policy is about what is UNDERNEATH. That is a pause menu, and neither
        // scene said a word about it.
        CHECK(g_log == "B.update A.draw B.draw");
    }

    TEST_CASE("updates_below lets the world below keep running") {
        Fixture fix;
        using Hud = Veil<'H', true, true>;
        start_clean<A>();
        rmp::Scene::push<Hud>();
        rmp::scenes::detail::apply_pending();

        g_log.clear();
        run_frame();
        CHECK(g_log == "A.update H.update A.draw H.draw");
    }

    TEST_CASE("draws_below false is a full-screen scene, and it is two words") {
        Fixture fix;
        using Loading = Veil<'L', false, false>;
        start_clean<A>();
        rmp::Scene::push<Loading>();
        rmp::scenes::detail::apply_pending();

        g_log.clear();
        run_frame();
        CHECK(g_log == "L.update L.draw");
    }

    TEST_CASE("the policy is read from the whole stack, not just the top") {
        Fixture fix;
        using Pass = Veil<'P', true, true>;
        start_clean<A>();
        rmp::Scene::push<Pass>(); // lets A through
        rmp::Scene::push<B>(); // freezes everything under it
        rmp::scenes::detail::apply_pending();

        g_log.clear();
        run_frame();

        // B stops the updates at P, so A does not run even though P would have let
        // it. Walking down from the top and stopping at the first scene that says
        // no is what makes a pause menu freeze a whole stack rather than one layer.
        CHECK(g_log == "B.update A.draw P.draw B.draw");
    }

    TEST_CASE("the clear colour comes from the lowest scene that is visible") {
        Fixture fix;
        using Opaque = Veil<'O', false, false>;

        class Blue : public rmp::Scene {
        public:
            Blue() { background = Color{ 0, 0, 255, 255 }; }
        };

        rmp::scenes::detail::start(new Blue());
        CHECK(rmp::scenes::detail::clear_color().b == 255);

        // A scene that covers the screen becomes the one that decides.
        rmp::Scene::push<Opaque>();
        rmp::scenes::detail::apply_pending();
        // Opaque left background at BLANK, so the theme's colour wins — and the
        // theme's background is not blue.
        CHECK(rmp::scenes::detail::clear_color().b != 255);
    }

} // TEST_SUITE

TEST_SUITE("scene shutdown") {
    TEST_CASE("shutdown unwinds the whole stack, top down") {
        Fixture fix;
        start_clean<A>();
        rmp::Scene::push<B>();
        rmp::Scene::push<C>();
        rmp::scenes::detail::apply_pending();

        g_log.clear();
        rmp::scenes::detail::shutdown();
        CHECK(g_log == "C.end B.end A.end");
        CHECK(rmp::Scene::depth() == 0);
        CHECK(!rmp::scenes::detail::running());
    }

    TEST_CASE("a transition queued as the app closes is dropped") {
        Fixture fix;
        start_clean<A>();
        rmp::Scene::push<B>();

        g_log.clear();
        rmp::scenes::detail::shutdown();

        // B is never entered. Running its _ready would give it a chance to load
        // assets that are about to be released, for a scene nobody will ever see.
        CHECK(g_log == "A.end");
    }

    TEST_CASE("a frame after shutdown does nothing rather than crashing") {
        Fixture fix;
        start_clean<A>();
        rmp::scenes::detail::shutdown();

        g_log.clear();
        run_frame();
        CHECK(g_log.empty());
    }

} // TEST_SUITE
