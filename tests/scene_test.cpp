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
#include "../src/rmp/ui/internal.h"

#include <rmp/scene.h>
#include <rmp/ui.h>

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
    // The UI frame boundary is part of the order, so it is part of the replica.
    // With scenes that draw no UI it is all no-ops — the UI does not start until
    // something asks for it — which is itself worth having under test.
    rmp::ui::detail::begin_frame();
    rmp::scenes::detail::update(1.0f / 60.0f);
    rmp::scenes::detail::draw();
    rmp::ui::detail::end_frame();
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

// --- the headless UI seams, for the two-scene tests at the bottom -----------
Clay_Dimensions measure_stub(Clay_StringSlice text, Clay_TextElementConfig *config,
                             void * /*user*/) {
    // Every glyph half an em wide. The numbers do not matter; being the same on
    // every machine does.
    const float size = config != nullptr ? static_cast<float>(config->fontSize) : 16.0f;
    return Clay_Dimensions{ static_cast<float>(text.length) * size * 0.5f, size };
}

void pointer_stub(Clay_Vector2 *position, bool *down) {
    *position = Clay_Vector2{ -1.0f, -1.0f };
    *down = false;
}

// Headless UI for the duration of a test, and raylib's own providers back
// afterwards — a later test that touched the UI without a window would crash.
struct HeadlessUi {
    HeadlessUi() {
        rmp::ui::detail::set_measure_provider(measure_stub);
        rmp::ui::detail::set_pointer_provider(pointer_stub);
        rmp::ui::detail::set_test_viewport(1280, 720);
    }
    ~HeadlessUi() {
        rmp::ui::detail::set_measure_provider(rmp::ui::detail::measure_with_raylib);
        rmp::ui::detail::set_pointer_provider(rmp::ui::detail::pointer_from_raylib);
        rmp::ui::detail::set_test_viewport(0, 0);
    }
};

// Two of these on the stack is the case the whole frame boundary exists for:
// a pause menu over a HUD, both describing UI in the same frame.
template <char Name> class UiScene : public rmp::Scene {
public:
    void _draw() override {
        note(Probe<Name>::kName, "draw");
        rmp::ui::begin();
        rmp::ui::button("Back");
        rmp::ui::end();
    }
};

// The id a label gets in a given pass, worked out the same way element_id()
// does it: the pass owns a block of 4096 indices, and the occurrence counts
// within it. Hashing only — no interning, so this disturbs nothing.
Clay_ElementId id_in_pass(const char *label, unsigned pass, unsigned occurrence) {
    Clay_String s{ false, static_cast<int32_t>(std::string_view{ label }.size()), label };
    return Clay_GetElementIdWithIndex(s, pass * 4096 + occurrence);
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
        CHECK(&rmp::Scene::current() == &rmp::Scene::current());
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
        // current() is the TOP of the stack, not the one that has been there
        // longest. B is what the player is looking at.
        CHECK(dynamic_cast<B *>(&rmp::Scene::current()) != nullptr);

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

TEST_SUITE("scene ui passes") {
    TEST_CASE("two scenes drawing UI in one frame get an id space each") {
        Fixture fix;
        HeadlessUi headless;

        start_clean<UiScene<'A'>>();
        rmp::Scene::push<UiScene<'B'>>();
        rmp::scenes::detail::apply_pending();
        g_log.clear();

        run_frame(); // records the geometry
        run_frame(); // and reads it back

        CHECK(g_log == "A.draw B.draw A.draw B.draw");

        // The point of the pass offset. Both scenes drew a button labelled
        // "Back", and each has to be its own element — otherwise hovering one
        // lights up the other, and worse, a lower scene showing a widget
        // conditionally renumbers every scene above it and the hover jumps with
        // nothing in that scene having changed.
        //
        // Both also have to survive in OUR snapshot, because Clay cannot answer
        // this: Clay_BeginLayout resets its element map, so after two passes
        // only the second one's boxes exist.
        //
        // id_in_pass() spells the scheme out independently of the code, so a
        // change to the scheme fails here. Checked by breaking it on purpose:
        // with the pass offset removed, the second scene's box is not found.
        Clay_BoundingBox box{};
        CHECK(rmp::ui::detail::bounds_of_id(id_in_pass("Back", 0, 0), &box));
        CHECK(box.width > 0);
        CHECK(rmp::ui::detail::bounds_of_id(id_in_pass("Back", 1, 0), &box));
        CHECK(box.width > 0);
    }

    TEST_CASE("a pass the input cannot reach lays out and draws, but does not react") {
        Fixture fix;
        HeadlessUi headless;

        // B takes the default input_below = false, so A is drawn and inert.
        start_clean<UiScene<'A'>>();
        rmp::Scene::push<UiScene<'B'>>();
        rmp::scenes::detail::apply_pending();

        run_frame();
        run_frame();

        // A was drawn — it has geometry — which is what separates "not
        // interactive" from "not there".
        Clay_BoundingBox box{};
        CHECK(rmp::ui::detail::bounds_of_id(id_in_pass("Back", 0, 0), &box));
        CHECK(box.width > 0);
    }

} // TEST_SUITE
