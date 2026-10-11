// ---------------------------------------------------------------------------
// rmp::Object and what it hands a game -- Handle, Ref, Callback, behaviors,
// the hooks, forces, the integrator, the edges and the box -- held to
// rmp/object.h over any sequence of spawns, destroys and frames, and any
// float a game can put in a field by mistake.
//
// The handles are checked against a model of the storage: a list of what was
// spawned and whether it is alive. A handle resolves exactly while its object
// is alive, through any reuse of its slot, and two handles are equal exactly
// when they name the same spawn -- the promise rmp/object.h makes in so many
// words ("it CANNOT come back to life").
//
// Each counterexample is a named case in tests/object_test.cpp.
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"
#include "object_gen.h"

#include "../../src/rmp/internal.h"
#include "../../src/rmp/object_internal.h"

#include <rmp/object.h>
#include <rmp/scene.h>

#include <doctest.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace world = rmp::test::world;

// What the framework did to an object, counted.
class Hooked : public world::Tagged {
public:
    int readies = 0;
    int updates = 0;
    int ends = 0;
    int collisions = 0;
    float last_delta = -1;

    void _ready() override { ++readies; }
    void _update(float delta) override {
        ++updates;
        last_delta = delta;
    }
    void _end() override { ++ends; }
    void _collision(rmp::Object &other) override {
        ++collisions;
        (void)other;
    }
};

// Behaviors of three kinds, each knowing which add() made it.
template <int N> struct Kind {
    int made = 0;
    void _update(rmp::Object &self, float delta) {
        (void)self;
        (void)delta;
    }
};

// Who was born and who died, for the Callback's captured state.
struct {
    std::vector<int> deaths;
    std::vector<int> calls;
} witness;

// Move-only, as a lambda that captures a std::unique_ptr is: what
// std::function cannot hold and Callback promises to.
class Token {
public:
    explicit Token(int id) : _id(std::make_unique<int>(id)) {}
    Token(Token &&) noexcept = default;
    Token &operator=(Token &&) noexcept = default;
    Token(const Token &) = delete;
    Token &operator=(const Token &) = delete;
    ~Token() {
        if (_id) witness.deaths.push_back(*_id);
    }
    [[nodiscard]] int id() const { return _id ? *_id : 0; }

private:
    std::unique_ptr<int> _id;
};

rmp::Callback<int> holding(int id) {
    return { [token = Token(id)](int arg) {
        witness.calls.push_back((token.id() * 1000) + arg);
    } };
}

// The same float, to the bit: NaN included, and -0 not 0.
bool same_bits(float a, float b) {
    return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
}
bool same_bits(Vector2 a, Vector2 b) {
    return same_bits(a.x, b.x) && same_bits(a.y, b.y);
}

rc::Gen<float> any_delta() {
    return rc::gen::element(0.0F, 1.0F / 240, 1.0F / 60, 1.0F / 30, 0.25F, 1.0F);
}

bool finite_fields(const rmp::Object &o) {
    return world::finite(o.position) && world::finite(o.velocity);
}

} // namespace

TEST_SUITE("property: object") {
    // covers: rmp::Handle::Handle [template]
    // covers: rmp::Handle::get [template]
    // covers: rmp::Handle::operator bool [template]
    // covers: rmp::Handle::operator* [template]
    // covers: rmp::Handle::operator-> [template]
    // covers: rmp::Object::handle [template]
    // covers: rmp::Object::destroy()
    // covers: rmp::Object::alive() const
    // covers: rmp::Object::~Object()
    // covers: rmp::operator== [template]
    TEST_CASE("a handle against a model of the storage") {
        const world::Hushed hushed;
        rmp::test::check(
            "object: a handle resolves exactly while its object is alive, through any "
            "spawn, destroy, collect and reuse",
            [] {
                const world::Fresh fresh;
                world::World scene;
                struct Spawned {
                    rmp::Handle<world::Tagged> handle;
                    bool alive;
                };
                std::vector<Spawned> model;
                const int steps = *rc::gen::inRange(1, 80);
                for (int step = 0; step < steps; ++step) {
                    const int op = *rc::gen::inRange(0, 7);
                    if (op <= 1 || model.empty()) {
                        auto &made = scene.spawn<world::Tagged>();
                        made.id = static_cast<int>(model.size());
                        model.push_back({ made.handle<world::Tagged>(), true });
                    } else if (op <= 3) {
                        Spawned &picked =
                            model[*rc::gen::inRange<std::size_t>(0, model.size())];
                        if (picked.alive) {
                            // Through the object, through the scene, or twice.
                            const int how = *rc::gen::inRange(0, 3);
                            if (how == 1) {
                                scene.destroy(*picked.handle);
                            } else {
                                picked.handle->destroy();
                                if (how == 2) picked.handle->destroy();
                            }
                            picked.alive = false;
                        }
                    } else if (op == 4) {
                        rmp::objects::detail::collect();
                    } else if (op == 5) {
                        rmp::objects::detail::update(scene, 1.0F / 60);
                        rmp::objects::detail::collect();
                    } else {
                        // Every free slot's generation, poked to the edge of
                        // the 32 bits: the next spawns there wrap past zero.
                        // Even, as a free slot's is until the counter has
                        // wrapped once (a spawn adds one and the collect
                        // another): an odd one would be a slot 2^32 reuses
                        // old, which a 32-bit generation does not promise.
                        rmp::objects::detail::collect();
                        const unsigned edge =
                            *rc::gen::element(UINT_MAX - 1, UINT_MAX - 3);
                        bool any_alive = false;
                        for (const Spawned &s : model) any_alive = any_alive || s.alive;
                        if (!any_alive) {
                            for (int i = 0; i < rmp::objects::detail::slot_count(); ++i)
                                rmp::objects::detail::set_generation_for_tests(
                                    static_cast<unsigned>(i), edge);
                        }
                    }

                    int alive = 0;
                    for (std::size_t i = 0; i < model.size(); ++i) {
                        const Spawned &s = model[i];
                        RC_ASSERT(static_cast<bool>(s.handle) == s.alive);
                        RC_ASSERT(static_cast<bool>(s.handle.get()) == s.alive);
                        if (s.alive) {
                            ++alive;
                            RC_ASSERT(s.handle->id == static_cast<int>(i));
                            RC_ASSERT((*s.handle).alive());
                            RC_ASSERT(&*s.handle.get() == &*s.handle);
                        }
                        // The same spawn, and only the same spawn, is equal.
                        for (std::size_t j = 0; j < model.size(); ++j)
                            RC_ASSERT((s.handle == model[j].handle) == (i == j));
                    }
                    RC_ASSERT(scene.object_count() == alive);
                    RC_ASSERT(rmp::objects::detail::live_count() == alive);
                }
            });
        rmp::test::check("object: a handle made from nothing names nothing", [] {
            const world::Fresh fresh;
            world::World scene;
            // Live objects in the first slots, which a handle that forgot to be
            // empty would find.
            const int live = *rc::gen::inRange(0, 6);
            for (int i = 0; i < live; ++i) (void)scene.spawn();
            const rmp::Handle<rmp::Object> empty;
            RC_ASSERT(!empty);
            RC_ASSERT(!empty.get());
            RC_ASSERT(empty == rmp::Handle<rmp::Object>());
            // Generation 0, at any index: "points at nothing".
            const unsigned index = *rc::gen::arbitrary<unsigned>();
            RC_ASSERT(!rmp::Handle<rmp::Object>(index, 0));
        });
    }

    // covers: rmp::Ref::Ref [template]
    // covers: rmp::Ref::operator bool [template]
    // covers: rmp::Ref::operator* [template]
    // covers: rmp::Ref::operator-> [template]
    TEST_CASE("a Ref is something or nothing") {
        const world::Hushed hushed;
        rmp::test::check(
            "object: a Ref refers to what it was made from, and an empty one reaches a "
            "fresh stand-in, said once",
            [] {
                world::Tagged one;
                world::Tagged two;
                one.id = *rc::gen::arbitrary<int>();
                two.id = *rc::gen::arbitrary<int>();
                const rmp::Ref<world::Tagged> to_one(one);
                RC_ASSERT(static_cast<bool>(to_one));
                RC_ASSERT(&*to_one == &one);
                RC_ASSERT(to_one->id == one.id);
                RC_ASSERT(to_one == rmp::Ref<world::Tagged>(one));
                RC_ASSERT(!(to_one == rmp::Ref<world::Tagged>(two)));
                RC_ASSERT(!(to_one == nullptr));

                const rmp::Ref<world::Tagged> empty;
                RC_ASSERT(!empty);
                RC_ASSERT(empty == nullptr);
                RC_ASSERT(empty == rmp::Ref<world::Tagged>());
                rmp::detail::reset_reports_for_tests();
                empty->id = one.id + 1; // lands nowhere
                RC_ASSERT((*empty).id == -1);
                RC_ASSERT(&*empty != &one);
                RC_ASSERT(rmp::detail::report_count() == 1);
            });
    }

    // covers: rmp::Callback::Callback [template]
    // covers: rmp::Callback::clear [template]
    // covers: rmp::Callback::operator bool [template]
    // covers: rmp::Callback::operator() [template]
    // covers: rmp::Callback::operator= [template]
    // covers: rmp::Callback::set [template]
    // covers: rmp::Callback::~Callback [template]
    TEST_CASE("a Callback against a model") {
        const world::Hushed hushed;
        rmp::test::check(
            "object: a Callback holds one function, calls it, and lets what it "
            "captured go exactly once",
            [] {
                witness.deaths.clear();
                witness.calls.clear();
                std::vector<int> deaths;
                std::vector<int> calls;
                {
                    // Two of them, and which id each holds (0: none).
                    std::array<rmp::Callback<int>, 2> held;
                    std::array<int, 2> model{ 0, 0 };
                    int next = 1;
                    const int steps = *rc::gen::inRange(1, 40);
                    for (int step = 0; step < steps; ++step) {
                        const auto i = *rc::gen::inRange<std::size_t>(0, 2);
                        const std::size_t other = 1 - i;
                        switch (*rc::gen::inRange(0, 7)) {
                            case 0: // set
                                if (model[i] != 0) deaths.push_back(model[i]);
                                held[i].set([token = Token(next)](int arg) {
                                    witness.calls.push_back((token.id() * 1000) + arg);
                                });
                                model[i] = next++;
                                break;
                            case 1: // assigned a new one
                                if (model[i] != 0) deaths.push_back(model[i]);
                                held[i] = holding(next);
                                model[i] = next++;
                                break;
                            case 2:
                                if (model[i] != 0) deaths.push_back(model[i]);
                                held[i].clear();
                                model[i] = 0;
                                break;
                            case 3: // moved from the other
                                if (model[i] != 0) deaths.push_back(model[i]);
                                held[i] = std::move(held[other]);
                                model[i] = model[other];
                                model[other] = 0;
                                break;
                            case 4: { // moved into a new one, and that one gone
                                const rmp::Callback<int> taken(std::move(held[i]));
                                if (model[i] != 0) deaths.push_back(model[i]);
                                model[i] = 0;
                                break;
                            }
                            case 5: { // moved onto itself
                                rmp::Callback<int> &self = held[i];
                                held[i] = std::move(self);
                                break;
                            }
                            default: {
                                const int arg = *rc::gen::inRange(0, 1000);
                                held[i](arg);
                                if (model[i] != 0)
                                    calls.push_back((model[i] * 1000) + arg);
                                break;
                            }
                        }
                        RC_ASSERT(static_cast<bool>(held[0]) == (model[0] != 0));
                        RC_ASSERT(static_cast<bool>(held[1]) == (model[1] != 0));
                        RC_ASSERT(witness.deaths == deaths);
                        RC_ASSERT(witness.calls == calls);
                    }
                    for (const int id : model)
                        if (id != 0) deaths.push_back(id);
                }
                // Both went out of scope: what they still held went with them,
                // in the order the array destroys them (last first).
                RC_ASSERT(witness.deaths.size() == deaths.size());
                std::vector<int> sorted_seen = witness.deaths;
                std::vector<int> sorted_expected = deaths;
                std::ranges::sort(sorted_seen);
                std::ranges::sort(sorted_expected);
                RC_ASSERT(sorted_seen == sorted_expected);
            });
    }

    // covers: rmp::Object::on_click [template]
    // covers: rmp::Object::on_drag [template]
    // covers: rmp::Object::on_collision [template]
    TEST_CASE("the on_ callbacks") {
        const world::Hushed hushed;
        rmp::test::check(
            "object: on_click, on_drag and on_collision each keep the last function "
            "handed to them, and call it with the object",
            [] {
                const world::Fresh fresh;
                world::World scene;
                auto &self = scene.spawn<world::Tagged>();
                auto &other = scene.spawn<world::Tagged>();
                std::vector<std::string> heard;
                const int sets = *rc::gen::inRange(1, 4);
                for (int n = 0; n < sets; ++n) {
                    self.on_click([&heard, n, &self](rmp::Object &me) {
                        heard.push_back("click" + std::to_string(n) +
                                        (&me == &self ? "" : "?"));
                    });
                    self.on_drag([&heard, n](rmp::Object &me, Vector2 moved) {
                        (void)me;
                        heard.push_back("drag" + std::to_string(n) + "@" +
                                        std::to_string(moved.x));
                    });
                    self.on_collision(
                        [&heard, n, &other](rmp::Object &me, rmp::Object &it) {
                            (void)me;
                            heard.push_back("hit" + std::to_string(n) +
                                            (&it == &other ? "" : "?"));
                        });
                }
                const float moved = *rmp::test::gen::hostile_float();
                rmp::Storage::notify_click(self);
                rmp::Storage::notify_drag(self, Vector2{ moved, 0 });
                rmp::Storage::notify_collision(self, other);
                const std::string last = std::to_string(sets - 1);
                const std::vector<std::string> expected{ "click" + last,
                                                         "drag" + last + "@" +
                                                             std::to_string(moved),
                                                         "hit" + last };
                RC_ASSERT(heard == expected);
                RC_ASSERT(rmp::Storage::has_pointer_callback(self));
                RC_ASSERT(!rmp::Storage::has_pointer_callback(other));
            });
    }

    // covers: rmp::Object::add [template]
    // covers: rmp::Object::get [template]
    // covers: rmp::Object::has [template]
    // covers: rmp::Object::remove [template]
    TEST_CASE("behaviors against a set") {
        const world::Hushed hushed;
        rmp::test::check(
            "object: behaviors added, got and removed in any order are a set, the last "
            "add of a kind winning",
            [] {
                const world::Fresh fresh;
                world::World scene;
                auto &object = scene.spawn<world::Tagged>();
                std::array<int, 3> model{ 0, 0, 0 }; // which add() made it; 0: none
                int next = 1;
                const int steps = *rc::gen::inRange(1, 30);
                for (int step = 0; step < steps; ++step) {
                    const int which = *rc::gen::inRange(0, 3);
                    const bool adding = *rc::gen::arbitrary<bool>();
                    const int made = next++;
                    if (which == 0) {
                        if (adding) {
                            RC_ASSERT(object.add<Kind<0>>({ .made = made }).made == made);
                        } else {
                            object.remove<Kind<0>>();
                        }
                    } else if (which == 1) {
                        if (adding) {
                            RC_ASSERT(object.add<Kind<1>>({ .made = made }).made == made);
                        } else {
                            object.remove<Kind<1>>();
                        }
                    } else {
                        if (adding) {
                            RC_ASSERT(object.add<Kind<2>>({ .made = made }).made == made);
                        } else {
                            object.remove<Kind<2>>();
                        }
                    }
                    model[static_cast<std::size_t>(which)] = adding ? made : 0;
                    RC_ASSERT(object.has<Kind<0>>() == (model[0] != 0));
                    RC_ASSERT(object.has<Kind<1>>() == (model[1] != 0));
                    RC_ASSERT(object.has<Kind<2>>() == (model[2] != 0));
                    if (auto k = object.get<Kind<0>>()) RC_ASSERT(k->made == model[0]);
                    if (auto k = object.get<Kind<1>>()) RC_ASSERT(k->made == model[1]);
                    if (auto k = object.get<Kind<2>>()) RC_ASSERT(k->made == model[2]);
                    int count = 0;
                    for (const int m : model) count += m != 0 ? 1 : 0;
                    RC_ASSERT(rmp::objects::detail::behavior_count(object) == count);
                }
            });
    }

    // covers: rmp::Object::_ready()
    // covers: rmp::Object::_update(float)
    // covers: rmp::Object::_end()
    // covers: rmp::Object::_collision(Object &)
    // covers: rmp::Object::_draw()
    TEST_CASE("the hooks") {
        const world::Hushed hushed;
        rmp::test::check(
            "object: _ready once at spawn, _update every frame alive, _end once at "
            "destroy, _collision once a pair a frame on each",
            [] {
                const world::Fresh fresh;
                world::World scene;
                struct Seen {
                    rmp::Handle<Hooked> handle;
                    int frames = 0;
                    bool destroyed = false;
                };
                std::vector<Seen> seen;
                const int steps = *rc::gen::inRange(1, 30);
                for (int step = 0; step < steps; ++step) {
                    const int op = *rc::gen::inRange(0, 4);
                    if (op == 0 || seen.empty()) {
                        auto &made = scene.spawn<Hooked>(
                            { .position = { static_cast<float>(
                                                *rc::gen::inRange(-20, 21)),
                                            0 },
                              .shape = rmp::rect({ 10, 10 }) });
                        made.id = static_cast<int>(seen.size());
                        RC_ASSERT(made.readies == 1);
                        RC_ASSERT(made.updates == 0);
                        seen.push_back({ made.handle<Hooked>() });
                    } else if (op == 1) {
                        Seen &picked =
                            seen[*rc::gen::inRange<std::size_t>(0, seen.size())];
                        if (!picked.destroyed) {
                            Hooked &object = *picked.handle;
                            object.destroy();
                            RC_ASSERT(object.ends == 1);
                            object.destroy();
                            RC_ASSERT(object.ends == 1);
                            picked.destroyed = true;
                        }
                    } else {
                        const float delta = *any_delta();
                        rmp::objects::detail::update(scene, delta);
                        // The pairs the pass is about to find, from where the
                        // update left everything: each one, once, on each side.
                        std::vector<int> touching(seen.size(), 0);
                        for (const auto &[a, b] : world::pairs_of(scene, true)) {
                            ++touching[static_cast<std::size_t>(a)];
                            ++touching[static_cast<std::size_t>(b)];
                        }
                        std::vector<int> before(seen.size(), 0);
                        for (std::size_t i = 0; i < seen.size(); ++i)
                            if (!seen[i].destroyed)
                                before[i] = seen[i].handle->collisions;
                        rmp::objects::detail::collide(scene);
                        for (std::size_t i = 0; i < seen.size(); ++i) {
                            Seen &s = seen[i];
                            if (s.destroyed) continue;
                            ++s.frames;
                            RC_ASSERT(s.handle->updates == s.frames);
                            RC_ASSERT(s.handle->last_delta == delta);
                            RC_ASSERT(s.handle->collisions - before[i] == touching[i]);
                        }
                        rmp::objects::detail::collect();
                    }
                }
                for (const Seen &s : seen) {
                    if (!s.destroyed) RC_ASSERT(s.handle->ends == 0);
                }
            });
        rmp::test::check(
            "object: the hooks of a plain Object do nothing, whatever they are handed",
            [] {
                const world::Fresh fresh;
                world::World scene;
                auto &plain =
                    scene.spawn({ .position = { *rmp::test::gen::hostile_float(),
                                                *rmp::test::gen::hostile_float() } });
                auto &other = scene.spawn();
                const Vector2 at = plain.position;
                plain._ready();
                plain._update(*rmp::test::gen::hostile_float());
                plain._draw();
                plain._collision(other);
                plain._end();
                RC_ASSERT(same_bits(plain.position, at));
                RC_ASSERT(plain.alive());
                RC_ASSERT(plain.velocity.x == 0);
                RC_ASSERT(plain.velocity.y == 0);
            });
    }

    // covers: rmp::Object::apply_force(Vector2)
    // covers: rmp::Object::apply_impulse(Vector2)
    TEST_CASE("forces and the integrator") {
        const world::Hushed hushed;
        rmp::test::check(
            "object: a force or an impulse that is not a number is refused, said once, "
            "and the velocity stays one",
            [] {
                const world::Fresh fresh;
                world::World scene;
                auto &object = scene.spawn(
                    { .velocity = { *world::speed(false), *world::speed(false) } });
                object.mass = *rc::gen::element(1.0F, 2.0F, 0.0F, -1.0F, 1e-30F);
                const Vector2 push{ *rmp::test::gen::hostile_float(),
                                    *rmp::test::gen::hostile_float() };
                RC_PRE(!world::finite(push));
                rmp::detail::reset_reports_for_tests();
                const Vector2 before = object.velocity;
                object.apply_impulse(push);
                object.apply_force(push);
                RC_ASSERT(object.velocity.x == before.x);
                RC_ASSERT(object.velocity.y == before.y);
                const int said = rmp::detail::report_count();
                RC_ASSERT(said >= 1);
                object.apply_impulse(push);
                object.apply_force(push);
                rmp::objects::detail::update(scene, *any_delta());
                RC_ASSERT(world::finite(object.velocity));
                RC_ASSERT(world::finite(object.position));
                RC_ASSERT(rmp::detail::report_count() == said);
            });
        rmp::test::check(
            "object: a frame never makes a finite position or velocity infinite, "
            "whatever the speed, the mass, the force and the gravity",
            [] {
                const world::Fresh fresh;
                world::World scene;
                const auto finite_float = [] {
                    return rc::gen::suchThat(
                        world::sometimes_hostile(world::speed(false), true),
                        [](float f) { return std::isfinite(f); });
                };
                scene.gravity = { *finite_float(), *finite_float() };
                auto &object =
                    scene.spawn({ .position = { *finite_float(), *finite_float() },
                                  .velocity = { *finite_float(), *finite_float() } });
                object.mass = *finite_float();
                object.gravity_scale = *finite_float();
                object.apply_force({ *finite_float(), *finite_float() });
                object.apply_impulse({ *finite_float(), *finite_float() });
                RC_ASSERT(finite_fields(object));
                for (int i = 0; i < 3; ++i) {
                    rmp::objects::detail::update(scene, *any_delta());
                    RC_ASSERT(finite_fields(object));
                }
            });
    }

    // covers: rmp::Object::world_bounds() const
    // covers: rmp::rect(Vector2)
    // covers: rmp::circle(float)
    TEST_CASE("the box an object occupies") {
        const world::Hushed hushed;
        rmp::test::check(
            "object: rect() and circle() are the shape they are handed, to the bit", [] {
                const Vector2 size{ *rmp::test::gen::hostile_float(),
                                    *rmp::test::gen::hostile_float() };
                const float radius = *rmp::test::gen::hostile_float();
                const rmp::Shape box = rmp::rect(size);
                const rmp::Shape disc = rmp::circle(radius);
                RC_ASSERT(box.kind == rmp::ShapeKind::RECTANGLE);
                RC_ASSERT(disc.kind == rmp::ShapeKind::CIRCLE);
                RC_ASSERT(same_bits(box.size, size));
                RC_ASSERT(same_bits(disc.radius, radius));
                RC_ASSERT(box.radius == 0);
                RC_ASSERT(disc.size.x == 0);
            });
        rmp::test::check(
            "object: world_bounds is the shape's size times the scale, whatever their "
            "signs, around the position",
            [] {
                const world::Fresh fresh;
                world::World scene;
                const world::Thing t = *world::thing(false);
                auto &object = world::spawn(scene, t, 0);
                const Rectangle box = object.world_bounds();
                const float cx = t.position.x + t.offset.x;
                const float cy = t.position.y + t.offset.y;
                float w = 0;
                float h = 0;
                if (t.kind == world::Kind::BOX || t.kind == world::Kind::DISC_IN_BOX) {
                    w = std::fabs(t.size.x * t.scale.x);
                    h = std::fabs(t.size.y * t.scale.y);
                } else if (t.kind == world::Kind::DISC) {
                    w = h = 2 * std::fabs(t.radius) *
                        std::max(std::fabs(t.scale.x), std::fabs(t.scale.y));
                }
                RC_ASSERT(box.width >= 0);
                RC_ASSERT(box.height >= 0);
                RC_ASSERT(std::fabs(box.width - w) <= 1e-3F);
                RC_ASSERT(std::fabs(box.height - h) <= 1e-3F);
                if (t.kind == world::Kind::POINT) {
                    RC_ASSERT(box.x == t.position.x);
                    RC_ASSERT(box.y == t.position.y);
                } else {
                    RC_ASSERT(std::fabs(box.x + (box.width / 2) - cx) <= 1e-3F);
                    RC_ASSERT(std::fabs(box.y + (box.height / 2) - cy) <= 1e-3F);
                }
            });
    }

    TEST_CASE("the edges") {
        const world::Hushed hushed;
        rmp::test::check(
            "object: an edge rule never makes a finite position or velocity infinite, "
            "whatever its bounds",
            [] {
                const world::Fresh fresh;
                world::World scene;
                const world::Thing t = *world::thing(*rc::gen::arbitrary<bool>());
                auto &object = world::spawn(scene, t, 0);
                object.edges = *rc::gen::element(rmp::Edge::CLAMP, rmp::Edge::BOUNCE,
                                                 rmp::Edge::WRAP, rmp::Edge::DESTROY);
                object.bounds = *rc::gen::weightedOneOf<Rectangle>({
                    { 2,
                      rc::gen::build<Rectangle>(
                          rc::gen::set(&Rectangle::x, world::coordinate(false)),
                          rc::gen::set(&Rectangle::y, world::coordinate(false)),
                          rc::gen::set(&Rectangle::width, world::extent(false)),
                          rc::gen::set(&Rectangle::height, world::extent(false))) },
                    { 1, rmp::test::gen::hostile_rect() },
                    { 1,
                      rc::gen::build<Rectangle>(
                          rc::gen::set(&Rectangle::x, world::near_the_top()),
                          rc::gen::set(&Rectangle::y, world::near_the_top()),
                          rc::gen::set(&Rectangle::width, world::near_the_top()),
                          rc::gen::set(&Rectangle::height, world::near_the_top())) },
                });
                const bool was = finite_fields(object);
                for (int i = 0; i < 2 && object.alive(); ++i) {
                    rmp::objects::detail::update(scene, *any_delta());
                    if (was && object.alive()) RC_ASSERT(finite_fields(object));
                }
            });
        rmp::test::check(
            "object: CLAMP and BOUNCE keep a box that fits inside its bounds", [] {
                const world::Fresh fresh;
                world::World scene;
                const world::Thing t = *world::thing(false);
                RC_PRE(t.kind != world::Kind::POINT);
                auto &object = world::spawn(scene, t, 0);
                object.edges = *rc::gen::element(rmp::Edge::CLAMP, rmp::Edge::BOUNCE);
                const Rectangle area{ *world::coordinate(false),
                                      *world::coordinate(false),
                                      static_cast<float>(*rc::gen::inRange(1, 3000)),
                                      static_cast<float>(*rc::gen::inRange(1, 3000)) };
                object.bounds = area;
                const Rectangle size = object.world_bounds();
                RC_PRE(size.width <= area.width && size.height <= area.height);
                rmp::objects::detail::update(scene, *any_delta());
                const Rectangle box = object.world_bounds();
                const float slack = 1e-3F *
                    (1 + std::fabs(area.x) + std::fabs(area.y) + area.width +
                     area.height);
                RC_ASSERT(box.x >= area.x - slack);
                RC_ASSERT(box.y >= area.y - slack);
                RC_ASSERT(box.x + box.width <= area.x + area.width + slack);
                RC_ASSERT(box.y + box.height <= area.y + area.height + slack);
            });
        rmp::test::check(
            "object: an edge rule acts on a box past an edge and on nothing that only "
            "touches one, as rmp/object.h says each one does",
            [] {
                const world::Fresh fresh;
                world::World scene;
                // Whole numbers, so that touching is exact.
                const auto whole = [](int lo, int hi) {
                    return static_cast<float>(*rc::gen::inRange(lo, hi));
                };
                const Rectangle area{ whole(-200, 200), whole(-200, 200), whole(60, 400),
                                      whole(60, 400) };
                const float w = 2 * whole(1, 20);
                const float h = 2 * whole(1, 20);
                // Where the box starts on each axis: well out, touching from
                // outside, across the edge, touching from inside, inside -- at
                // either end.
                const auto start = [](float low, float size, float box) {
                    switch (*rc::gen::inRange(0, 9)) {
                        case 0:
                            return low - box - 10;
                        case 1:
                            return low - box;
                        case 2:
                            return low - (box / 2);
                        case 3:
                            return low;
                        case 4:
                            return low + 1;
                        case 5:
                            return low + size - box;
                        case 6:
                            return low + size - (box / 2);
                        case 7:
                            return low + size;
                        default:
                            return low + size + 10;
                    }
                };
                const float bx = start(area.x, area.width, w);
                const float by = start(area.y, area.height, h);
                const Vector2 moving{ static_cast<float>(*rc::gen::element(-30, 0, 30)),
                                      static_cast<float>(*rc::gen::element(-30, 0, 30)) };
                const rmp::Edge edge =
                    *rc::gen::element(rmp::Edge::CLAMP, rmp::Edge::BOUNCE,
                                      rmp::Edge::WRAP, rmp::Edge::DESTROY);
                auto &object = scene.spawn({ .position = { bx + (w / 2), by + (h / 2) },
                                             .shape = rmp::rect({ w, h }),
                                             .velocity = moving,
                                             .edges = edge,
                                             .bounds = area });
                const bool entered = *rc::gen::arbitrary<bool>();
                rmp::Storage::set_entered_bounds(object, entered);
                rmp::objects::detail::update(scene, 0); // the edges, and nothing moving

                const float right = area.x + area.width;
                const float bottom = area.y + area.height;
                Vector2 at{ bx + (w / 2), by + (h / 2) };
                Vector2 v = moving;
                bool alive = true;
                if (edge == rmp::Edge::CLAMP || edge == rmp::Edge::BOUNCE) {
                    // As soon as the box crosses an edge, and not when it touches.
                    if (bx < area.x) {
                        at.x += area.x - bx;
                        if (edge == rmp::Edge::BOUNCE && v.x < 0) v.x = -v.x;
                    }
                    if (bx + w > right) {
                        at.x -= (bx + w) - right;
                        if (edge == rmp::Edge::BOUNCE && v.x > 0) v.x = -v.x;
                    }
                    if (by < area.y) {
                        at.y += area.y - by;
                        if (edge == rmp::Edge::BOUNCE && v.y < 0) v.y = -v.y;
                    }
                    if (by + h > bottom) {
                        at.y -= (by + h) - bottom;
                        if (edge == rmp::Edge::BOUNCE && v.y > 0) v.y = -v.y;
                    }
                } else if (edge == rmp::Edge::WRAP) {
                    // Entirely past a side: back just outside the other one.
                    if (bx + w < area.x) {
                        at.x += right - bx;
                    } else if (bx > right) {
                        at.x += area.x - (bx + w);
                    }
                    if (by + h < area.y) {
                        at.y += bottom - by;
                    } else if (by > bottom) {
                        at.y += area.y - (by + h);
                    }
                } else {
                    // Entirely outside, and inside at some point first.
                    const bool out =
                        bx + w < area.x || bx > right || by + h < area.y || by > bottom;
                    alive = !(out && entered);
                }
                RC_ASSERT(object.alive() == alive);
                RC_ASSERT(object.position.x == at.x);
                RC_ASSERT(object.position.y == at.y);
                RC_ASSERT(object.velocity.x == v.x);
                RC_ASSERT(object.velocity.y == v.y);
            });
    }

    // covers: rmp::Object::scene() const
    TEST_CASE("an object's scene") {
        const world::Hushed hushed;
        rmp::test::check(
            "object: scene() is the scene that spawned it; for any other, current(), "
            "said once",
            [] {
                const world::Fresh fresh;
                world::World one;
                world::World two;
                auto &a = one.spawn();
                auto &b = two.spawn();
                RC_ASSERT(&a.scene() == &one);
                RC_ASSERT(&b.scene() == &two);
                rmp::detail::reset_reports_for_tests();
                const rmp::Object plain;
                RC_ASSERT(&plain.scene() == &rmp::Scene::current());
                const int said = rmp::detail::report_count();
                RC_ASSERT(said >= 1);
                const int asks = *rc::gen::inRange(1, 4);
                for (int i = 0; i < asks; ++i)
                    RC_ASSERT(&plain.scene() == &rmp::Scene::current());
                RC_ASSERT(rmp::detail::report_count() == said);
            });
    }

    // covers: rmp::Sprite::play(std::string_view, bool)
    // covers: rmp::Sprite::stop()
    // covers: rmp::Sprite::finished() const
    // covers: rmp::Sprite::playing() const
    // covers: rmp::Sprite::frame_index() const
    // covers: rmp::Sprite::set_frame(int)
    TEST_CASE("a sprite with no sheet") {
        const world::Hushed hushed;
        rmp::test::check(
            "object: a sprite with no sheet plays nothing, says so once a tag, and keeps "
            "any frame it is set to",
            [] {
                rmp::Sprite sprite;
                const std::string tag = *rmp::test::gen::hostile_string();
                rmp::detail::reset_reports_for_tests();
                sprite.play(tag, *rc::gen::arbitrary<bool>());
                sprite.play(tag);
                RC_ASSERT(rmp::detail::report_count() == 1);
                RC_ASSERT(sprite.playing().empty());
                RC_ASSERT(!sprite.finished());
                const int frame = *rc::gen::arbitrary<int>();
                sprite.set_frame(frame);
                RC_ASSERT(sprite.frame_index() == frame);
                RC_ASSERT(!sprite.finished());
                sprite.stop();
                RC_ASSERT(sprite.finished());
                RC_ASSERT(sprite.frame_index() == frame);
                sprite.set_frame(frame);
                RC_ASSERT(!sprite.finished());
            });
    }
}
