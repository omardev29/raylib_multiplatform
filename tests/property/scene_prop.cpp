// ---------------------------------------------------------------------------
// The scene stack, held to what rmp/scene.h says of it over any frames of
// change(), push(), replace() and pop() -- several in one frame, the same one
// twice, a pop() of the last scene, and calls a scene's own hooks make while
// the stack is being changed -- against a model written from the header:
//
//   - the transcript of every hook, in order, is the model's: _ready() on
//     entering, _end() on leaving for good (top down under change()),
//     _suspend() under a push and _resume() when it comes off, nothing for the
//     scene under a replace(); _update() bottom up from the lowest scene the
//     ones above let run, each told whether input reaches it, and _draw()
//     from the lowest the ones above leave visible;
//   - what a hook asks for belongs to the next frame; the same scene asked
//     for twice in one frame is asked for once, and a pop() of the last scene
//     is refused -- each said once;
//   - current() and depth() are the stack; every scene made is destroyed
//     once, and shutdown() leaves nothing behind, whatever the hooks it runs
//     ask for on the way out -- the next run starts clean;
//   - a scene's object_count() is what it spawned and did not destroy, and
//     every object ends once, destroyed or ended with its scene;
//   - the three raycasts are one: raycast(from, to) is the query that filters
//     nothing, and raycast_all()'s nearest hit is raycast()'s.
//
// The scenes of the base class -- whose hooks are the empty ones -- are on the
// stack as well, among the ones that write the transcript.
//
// A counterexample one of these finds becomes a named case in
// tests/scene_test.cpp; the commit that added them says what each was seen
// red on.
//
// covers: rmp::Scene::_draw()
// covers: rmp::Scene::_end()
// covers: rmp::Scene::_ready()
// covers: rmp::Scene::_resume()
// covers: rmp::Scene::_suspend()
// covers: rmp::Scene::_update(float)
// covers: rmp::Scene::change [template]
// covers: rmp::Scene::current()
// covers: rmp::Scene::depth()
// covers: rmp::Scene::destroy(Object &)
// covers: rmp::Scene::object_count() const
// covers: rmp::Scene::pop()
// covers: rmp::Scene::push [template]
// covers: rmp::Scene::raycast(Vector2, Vector2) const
// covers: rmp::Scene::raycast(const RayQuery &) const
// covers: rmp::Scene::raycast_all(const RayQuery &, std::span<RayHit>) const
// covers: rmp::Scene::replace [template]
// covers: rmp::Scene::spawn [template]
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/internal.h"
#include "../../src/rmp/object_internal.h"
#include "../../src/rmp/scene_internal.h"
#include "../../src/rmp/ui/internal.h"

#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

enum class Op { CHANGE, PUSH, REPLACE, POP };
enum class Hook { NONE, READY, END, SUSPEND, RESUME };

// Kinds 0..3 write the transcript; 4 and 5 keep the base class's empty hooks.
constexpr int LOUD_KINDS = 4;
constexpr float DELTA = 1.0f / 60;

// What a scene lets through to the one under it.
struct Policy {
    bool updates_below = false;
    bool draws_below = true;
    bool input_below = false;
};

// A navigation call: the operation, and for all but pop() the scene's kind and
// what it lets through.
struct Call {
    Op op = Op::POP;
    int kind = 0;
    Policy policy;
};

// A call the frame makes, with what the scene it makes asks for in one of its
// own hooks -- once a hook of that kind runs, every time it runs.
struct Command {
    Call call;
    Hook when = Hook::NONE;
    Call then;
};

using Frame = std::vector<Command>;

// NOLINTBEGIN(readability-identifier-naming): RapidCheck's name, found by ADL
void showValue(const Call &c, std::ostream &os) {
    static constexpr std::array<std::string_view, 4> OPS{ "change", "push", "replace",
                                                          "pop" };
    os << OPS.at(static_cast<std::size_t>(c.op));
    if (c.op == Op::POP) return;
    os << "<" << c.kind << ">{" << (c.policy.updates_below ? "u" : "")
       << (c.policy.draws_below ? "d" : "") << (c.policy.input_below ? "i" : "") << "}";
}
void showValue(const Command &c, std::ostream &os) {
    static constexpr std::array<std::string_view, 5> HOOKS{ "", "ready", "end", "suspend",
                                                            "resume" };
    showValue(c.call, os);
    if (c.when == Hook::NONE) return;
    os << " on " << HOOKS.at(static_cast<std::size_t>(c.when)) << " ";
    showValue(c.then, os);
}
// NOLINTEND(readability-identifier-naming)

// The test's side of things: the transcript, the scenes alive, the next id.
// The dot says "file state".
struct {
    std::vector<std::string> log;
    int live = 0;
    int next_id = 0;
    std::vector<float> deltas;
} run;

void note(int id, std::string_view event) {
    run.log.push_back(std::to_string(id) + "." + std::string(event));
}

void issue(const Call &call, Hook when, const Call &then);

// What every scene of the test is: an id, and the scenes alive counted.
class Tagged : public rmp::Scene {
public:
    const int id;
    explicit Tagged(const Policy &policy) : id(run.next_id++) {
        updates_below = policy.updates_below;
        draws_below = policy.draws_below;
        input_below = policy.input_below;
        ++run.live;
    }
    ~Tagged() override { --run.live; }
    Tagged(const Tagged &) = delete;
    Tagged &operator=(const Tagged &) = delete;
    Tagged(Tagged &&) = delete;
    Tagged &operator=(Tagged &&) = delete;
};

// A scene that writes its hooks down, and makes its call when one of them runs.
template <int K> class Loud : public Tagged {
public:
    Loud(const Policy &policy, Hook when, const Call &then)
        : Tagged(policy), _when(when), _then(then) {}

    void _ready() override { hook(Hook::READY, "ready"); }
    void _end() override { hook(Hook::END, "end"); }
    void _suspend() override { hook(Hook::SUSPEND, "suspend"); }
    void _resume() override { hook(Hook::RESUME, "resume"); }
    void _update(float delta) override {
        run.deltas.push_back(delta);
        note(id, rmp::input::detail::layer_input() ? "update" : "update-deaf");
    }
    void _draw() override { note(id, "draw"); }

private:
    void hook(Hook which, std::string_view event) {
        note(id, event);
        if (_when == which) issue(_then, Hook::NONE, Call{});
    }
    Hook _when;
    Call _then;
};

// A scene of the base class's hooks: they run, and do nothing.
template <int K> class Quiet : public Tagged {
public:
    explicit Quiet(const Policy &policy) : Tagged(policy) {}
};

template <int K> void navigate(Op op, const Policy &policy, Hook when, const Call &then) {
    if constexpr (K < LOUD_KINDS) {
        switch (op) {
            case Op::CHANGE:
                rmp::Scene::change<Loud<K>>(policy, when, then);
                return;
            case Op::PUSH:
                rmp::Scene::push<Loud<K>>(policy, when, then);
                return;
            case Op::REPLACE:
                rmp::Scene::replace<Loud<K>>(policy, when, then);
                return;
            case Op::POP:
                rmp::Scene::pop();
                return;
        }
    } else {
        switch (op) {
            case Op::CHANGE:
                rmp::Scene::change<Quiet<K>>(policy);
                return;
            case Op::PUSH:
                rmp::Scene::push<Quiet<K>>(policy);
                return;
            case Op::REPLACE:
                rmp::Scene::replace<Quiet<K>>(policy);
                return;
            case Op::POP:
                rmp::Scene::pop();
                return;
        }
    }
}

void issue(const Call &call, Hook when, const Call &then) {
    switch (call.kind) {
        case 0:
            navigate<0>(call.op, call.policy, when, then);
            return;
        case 1:
            navigate<1>(call.op, call.policy, when, then);
            return;
        case 2:
            navigate<2>(call.op, call.policy, when, then);
            return;
        case 3:
            navigate<3>(call.op, call.policy, when, then);
            return;
        case 4:
            navigate<4>(call.op, call.policy, when, then);
            return;
        default:
            navigate<5>(call.op, call.policy, when, then);
            return;
    }
}

// --- the model -------------------------------------------------------------
// rmp/scene.h's sentences, one by one, with none of scene.cpp's code.

struct ModelScene {
    int id;
    int kind;
    Policy policy;
    Hook when;
    Call then;
};

struct Pending {
    Op op;
    ModelScene next{}; // what change, push and replace enter; nothing for pop()
};

struct StackModel {
    std::vector<ModelScene> stack;
    std::vector<Pending> pending;
    std::vector<std::string> log;
    int next_id = 0;
    bool said_twice = false;
    bool said_pop = false;

    void note(const ModelScene &s, std::string_view event) {
        if (s.kind < LOUD_KINDS)
            log.push_back(std::to_string(s.id) + "." + std::string(event));
    }

    // What `hook` does to `s`: written down, and its call made.
    void hook(const ModelScene &s, Hook which, std::string_view event) {
        note(s, event);
        if (s.kind < LOUD_KINDS && s.when == which) ask(s.then, Hook::NONE, Call{});
    }

    // A call is recorded, and the scene it names is made then -- its id is
    // taken -- even when it is the second of its kind and goes no further.
    void ask(const Call &call, Hook when, const Call &then) {
        if (call.op == Op::POP) {
            pending.push_back(Pending{ Op::POP });
            return;
        }
        const ModelScene next{ next_id++, call.kind, call.policy, when, then };
        for (const Pending &p : pending) {
            if (p.op == call.op && p.next.kind == call.kind) {
                said_twice = true;
                return;
            }
        }
        pending.push_back(Pending{ call.op, next });
    }

    void end_top() {
        const ModelScene top = stack.back();
        hook(top, Hook::END, "end");
        stack.pop_back();
    }

    void enter(const ModelScene &s) {
        stack.push_back(s);
        hook(s, Hook::READY, "ready");
    }

    // The end of a frame: what it asked for, in order. What a hook asks for
    // on the way belongs to the next one.
    void apply() {
        std::vector<Pending> batch;
        batch.swap(pending);
        for (const Pending &p : batch) {
            switch (p.op) {
                case Op::CHANGE:
                    while (!stack.empty()) end_top();
                    enter(p.next);
                    break;
                case Op::PUSH:
                    if (!stack.empty()) {
                        const ModelScene top = stack.back();
                        hook(top, Hook::SUSPEND, "suspend");
                    }
                    enter(p.next);
                    break;
                case Op::REPLACE:
                    if (!stack.empty()) end_top();
                    enter(p.next);
                    break;
                case Op::POP:
                    if (stack.size() <= 1) {
                        said_pop = true;
                        break;
                    }
                    end_top();
                    {
                        const ModelScene top = stack.back();
                        hook(top, Hook::RESUME, "resume");
                    }
                    break;
            }
        }
    }

    // The lowest scene a pass reaches: down from the top while each lets the
    // one under it through.
    template <class Through> std::size_t lowest(Through through) const {
        std::size_t i = stack.size() - 1;
        while (i > 0 && through(stack[i].policy)) --i;
        return i;
    }

    void update() {
        if (stack.empty()) return;
        for (std::size_t i = lowest([](const Policy &p) { return p.updates_below; });
             i < stack.size(); ++i) {
            bool reached = true;
            for (std::size_t above = i + 1; above < stack.size(); ++above) {
                reached = reached && stack[above].policy.input_below;
            }
            note(stack[i], reached ? "update" : "update-deaf");
        }
    }

    void draw() {
        if (stack.empty()) return;
        for (std::size_t i = lowest([](const Policy &p) { return p.draws_below; });
             i < stack.size(); ++i) {
            note(stack[i], "draw");
        }
    }

    // Nothing queued survives it, whatever the _end()s ask for.
    void shutdown() {
        pending.clear();
        while (!stack.empty()) end_top();
        pending.clear();
    }
};

// --- generators ------------------------------------------------------------

rc::Gen<Policy> policy() {
    return rc::gen::exec([] {
        // The defaults -- a pause menu -- more often than not.
        if (*rc::gen::element(0, 1) == 0) return Policy{};
        return Policy{ *rc::gen::arbitrary<bool>(), *rc::gen::arbitrary<bool>(),
                       *rc::gen::arbitrary<bool>() };
    });
}

rc::Gen<Call> call() {
    return rc::gen::exec([] {
        const Op op =
            *rc::gen::element(Op::CHANGE, Op::PUSH, Op::PUSH, Op::PUSH, Op::REPLACE,
                              Op::REPLACE, Op::POP, Op::POP, Op::POP);
        if (op == Op::POP) return Call{};
        return Call{ op, *rc::gen::element(0, 1, 2, 3, 4, 5), *policy() };
    });
}

rc::Gen<Command> command() {
    return rc::gen::exec([] {
        Command c{ *call(), Hook::NONE, Call{} };
        if (c.call.op != Op::POP && c.call.kind < LOUD_KINDS) {
            c.when = *rc::gen::element(Hook::NONE, Hook::NONE, Hook::NONE, Hook::READY,
                                       Hook::END, Hook::SUSPEND, Hook::RESUME);
            if (c.when != Hook::NONE) c.then = *call();
        }
        return c;
    });
}

// Up to 24 frames of up to four calls each: past that a stack only grows
// deeper, and a property's run longer.
rc::Gen<std::vector<Frame>> script() {
    return rc::gen::exec([] {
        const int frames = *rc::gen::inRange(0, 25);
        return *rc::gen::container<std::vector<Frame>>(
            static_cast<std::size_t>(frames), rc::gen::exec([] {
                const int calls = *rc::gen::inRange(0, 5);
                return *rc::gen::container<Frame>(static_cast<std::size_t>(calls),
                                                  command());
            }));
    });
}

// The log off, the stack empty and the reports forgotten for a property, and
// the same once it ends, however it ends.
struct Stage {
    Stage() {
        SetTraceLogLevel(LOG_NONE);
        clean();
    }
    ~Stage() {
        clean();
        SetTraceLogLevel(LOG_INFO);
    }
    Stage(const Stage &) = delete;
    Stage &operator=(const Stage &) = delete;
    Stage(Stage &&) = delete;
    Stage &operator=(Stage &&) = delete;

    static void clean() {
        rmp::scenes::detail::shutdown();
        rmp::objects::detail::reset_for_tests();
        rmp::detail::reset_reports_for_tests();
        run.log.clear();
        run.deltas.clear();
    }
};

// One turn of the loop, as src/rmp/app.cpp's frame() takes it, minus the
// drawing calls around it; the frame's own calls after the draw, where a
// scene's _update() or _draw() would have made them.
void play(const Frame &frame) {
    rmp::ui::detail::begin_frame();
    rmp::scenes::detail::update(DELTA);
    rmp::scenes::detail::draw();
    rmp::ui::detail::end_frame();
    for (const Command &c : frame) issue(c.call, c.when, c.then);
    rmp::scenes::detail::apply_pending();
}

int current_id() { return dynamic_cast<const Tagged &>(rmp::Scene::current()).id; }

// --- objects ---------------------------------------------------------------

struct {
    std::map<int, int> ends;
    int live = 0;
} things;

class Counted : public rmp::Object {
public:
    int id = -1;
    Counted() { ++things.live; }
    ~Counted() override { --things.live; }
    Counted(const Counted &) = delete;
    Counted &operator=(const Counted &) = delete;
    Counted(Counted &&) = delete;
    Counted &operator=(Counted &&) = delete;
    void _end() override { ++things.ends[id]; }
};

} // namespace

TEST_SUITE("property: scene") {
    TEST_CASE("the stack is what scene.h says it is") {
        rmp::test::check(
            "the scene stack follows its model through any frames of change, push, "
            "replace and pop",
            [] {
                const Stage stage;
                const auto frames = *script();
                const Command first = *command();
                StackModel model;
                run.next_id = 0;
                model.next_id = 0;

                // The first scene, as RMP_GAME starts it.
                {
                    const Call c = first.call.op == Op::POP
                        ? Call{ Op::CHANGE, 0, Policy{} }
                        : Call{ Op::CHANGE, first.call.kind, first.call.policy };
                    model.enter(ModelScene{ model.next_id++, c.kind, c.policy, first.when,
                                            first.then });
                    switch (c.kind) {
                        case 0:
                            rmp::scenes::detail::start(std::make_unique<Loud<0>>(
                                c.policy, first.when, first.then));
                            break;
                        case 1:
                            rmp::scenes::detail::start(std::make_unique<Loud<1>>(
                                c.policy, first.when, first.then));
                            break;
                        case 2:
                            rmp::scenes::detail::start(std::make_unique<Loud<2>>(
                                c.policy, first.when, first.then));
                            break;
                        case 3:
                            rmp::scenes::detail::start(std::make_unique<Loud<3>>(
                                c.policy, first.when, first.then));
                            break;
                        case 4:
                            rmp::scenes::detail::start(
                                std::make_unique<Quiet<4>>(c.policy));
                            break;
                        default:
                            rmp::scenes::detail::start(
                                std::make_unique<Quiet<5>>(c.policy));
                            break;
                    }
                }
                RC_ASSERT(run.log == model.log);
                RC_ASSERT(rmp::scenes::detail::running());

                for (const Frame &frame : frames) {
                    model.update();
                    model.draw();
                    for (const Command &c : frame) model.ask(c.call, c.when, c.then);
                    model.apply();
                    play(frame);
                    RC_ASSERT(run.log == model.log);
                    // Frame by frame: compared whole, a long script's two
                    // transcripts are compared again every frame.
                    run.log.clear();
                    model.log.clear();
                    RC_ASSERT(rmp::Scene::depth() ==
                              static_cast<int>(model.stack.size()));
                    RC_ASSERT(current_id() == model.stack.back().id);
                    // Alive: what is on the stack, and what is queued to go on it.
                    const auto queued = std::ranges::count_if(
                        model.pending, [](const Pending &p) { return p.op != Op::POP; });
                    RC_ASSERT(run.live ==
                              static_cast<int>(model.stack.size()) +
                                  static_cast<int>(queued));
                }
                for (const float d : run.deltas) RC_ASSERT(d == DELTA);
                RC_ASSERT(rmp::detail::report_count() ==
                          (model.said_twice ? 1 : 0) + (model.said_pop ? 1 : 0));

                // Down, whatever the _end()s ask for on the way out.
                model.shutdown();
                rmp::scenes::detail::shutdown();
                RC_ASSERT(run.log == model.log);
                RC_ASSERT(rmp::Scene::depth() == 0);
                RC_ASSERT(!rmp::scenes::detail::running());
                RC_ASSERT(run.live == 0);

                // And the next run starts clean: nothing the last one queued.
                run.log.clear();
                const int fresh = run.next_id;
                rmp::scenes::detail::start(
                    std::make_unique<Loud<0>>(Policy{}, Hook::NONE, Call{}));
                rmp::scenes::detail::apply_pending();
                RC_ASSERT(run.log ==
                          std::vector<std::string>{ std::to_string(fresh) + ".ready" });
                RC_ASSERT(rmp::Scene::depth() == 1);
                rmp::scenes::detail::shutdown();
                RC_ASSERT(run.live == 0);
            });

        rmp::test::check(
            "current() of an empty stack is a stand-in that does nothing, said once", [] {
                const Stage stage;
                const int asked = *rc::gen::inRange(1, 4);
                for (int i = 0; i < asked; ++i) {
                    rmp::Scene &stand_in = rmp::Scene::current();
                    stand_in._ready();
                    stand_in._update(DELTA);
                    stand_in._draw();
                    stand_in._suspend();
                    stand_in._resume();
                    stand_in._end();
                    RC_ASSERT(rmp::Scene::depth() == 0);
                }
                RC_ASSERT(rmp::detail::report_count() == 1);
            });
    }

    TEST_CASE("objects belong to their scene") {
        rmp::test::check(
            "a scene's object_count is what it spawned and did not destroy, and every "
            "object ends once",
            [] {
                const Stage stage;
                things.ends.clear();
                things.live = 0;
                run.next_id = 0;
                rmp::scenes::detail::start(
                    std::make_unique<Loud<0>>(Policy{}, Hook::NONE, Call{}));

                // scene id -> the ids of its objects alive; object id -> its handle.
                std::map<int, std::set<int>> alive;
                std::map<int, rmp::Handle<Counted>> handles;
                std::map<int, int> owner;
                int next = 0;
                bool foreign = false;
                const int frames = *rc::gen::inRange(1, 8);
                for (int f = 0; f < frames; ++f) {
                    const int top = current_id();
                    const int spawns = *rc::gen::inRange(0, 5);
                    for (int s = 0; s < spawns; ++s) {
                        auto &made = rmp::Scene::current().spawn<Counted>();
                        made.id = next;
                        handles[next] = made.handle<Counted>();
                        owner[next] = top;
                        alive[top].insert(next);
                        ++next;
                    }
                    // Destroy a few, some twice, some another scene's.
                    const int destroys = *rc::gen::inRange(0, 4);
                    for (int d = 0; d < destroys && next > 0; ++d) {
                        const int victim = *rc::gen::inRange(0, next);
                        if (auto object = handles[victim].get()) {
                            if (owner[victim] != top) foreign = true;
                            rmp::Scene::current().destroy(*object);
                            alive[owner[victim]].erase(victim);
                        }
                    }
                    RC_ASSERT(rmp::Scene::current().object_count() ==
                              static_cast<int>(alive[top].size()));
                    // Somewhere else to be next frame.
                    switch (*rc::gen::element(0, 0, 1, 2)) {
                        case 1:
                            rmp::Scene::push<Loud<1>>(Policy{}, Hook::NONE, Call{});
                            break;
                        case 2:
                            if (rmp::Scene::depth() > 1) {
                                rmp::Scene::pop();
                                alive.erase(top); // its objects end with it
                            }
                            break;
                        default:
                            break;
                    }
                    play(Frame{});
                    RC_ASSERT(rmp::Scene::current().object_count() ==
                              static_cast<int>(alive[current_id()].size()));
                }
                RC_ASSERT(rmp::detail::report_count() == (foreign ? 1 : 0));
                rmp::scenes::detail::shutdown();
                RC_ASSERT(things.live == 0);
                for (int id = 0; id < next; ++id) RC_ASSERT(things.ends[id] == 1);
            });
    }

    TEST_CASE("the raycasts are one") {
        rmp::test::check(
            "raycast(from, to) is the query that filters nothing, and raycast_all's "
            "nearest hit is raycast's",
            [] {
                const Stage stage;
                rmp::scenes::detail::start(
                    std::make_unique<Loud<0>>(Policy{}, Hook::NONE, Call{}));
                rmp::Scene &scene = rmp::Scene::current();
                const int boxes = *rc::gen::inRange(0, 12);
                for (int b = 0; b < boxes; ++b) {
                    const auto at = [] {
                        return static_cast<float>(*rc::gen::inRange(-200, 1000));
                    };
                    const auto side = [] {
                        return static_cast<float>(*rc::gen::inRange(1, 120));
                    };
                    scene.spawn(
                        { .position = { at(), at() }, .size = { side(), side() } });
                }
                const auto at = [] {
                    return static_cast<float>(*rc::gen::inRange(-300, 1100));
                };
                const Vector2 from{ at(), at() };
                const Vector2 to{ at(), at() };
                const rmp::RayHit plain = scene.raycast(from, to);
                const rmp::RayHit query = scene.raycast({ .from = from, .to = to });
                RC_ASSERT(static_cast<bool>(plain) == static_cast<bool>(query));
                RC_ASSERT(plain.object == query.object);
                RC_ASSERT(plain.distance == query.distance);
                std::array<rmp::RayHit, 16> all{};
                const std::size_t room =
                    *rc::gen::inRange<std::size_t>(0, all.size() + 1);
                const int n = scene.raycast_all({ .from = from, .to = to },
                                                std::span(all).first(room));
                RC_ASSERT(n >= 0);
                RC_ASSERT(static_cast<std::size_t>(n) <= room);
                if (room > 0) RC_ASSERT((n > 0) == static_cast<bool>(plain));
                if (n > 0) RC_ASSERT(all[0].distance == plain.distance);
                for (int i = 1; i < n; ++i) {
                    RC_ASSERT(all[static_cast<std::size_t>(i - 1)].distance <=
                              all[static_cast<std::size_t>(i)].distance);
                }
            });
    }
}
