// ---------------------------------------------------------------------------
// The catalogue's second and third parts, held to the campaign's rules:
//
//   - rmp::find_path() against a plain Dijkstra over the same cells, on any
//     grid of walls between any two points: a way exactly when Dijkstra finds
//     one, as short as Dijkstra's, every step to a neighbour, none through a
//     wall or across a wall's corner, ending on the goal;
//   - an Area against a model of what it promises -- enter, stay, inside,
//     outside, exit -- over any path of another object across it;
//   - a StateMachine against a model of its transitions, over any sequence of
//     answers its conditions give;
//   - every one of them, with any fields and any delta, in a world of a few
//     objects on a map: no undefined behaviour, no hang, and what they write
//     into an object finite whenever the object's was;
//   - B::from() of every entity of every LDtk file the tests carry reads each
//     documented field as the entity holds it, and the default where it does
//     not.
//
// A counterexample becomes a named case in tests/behaviors_more_test.cpp.
// ---------------------------------------------------------------------------

// covers: rmp::find_path(const Tilemap &, Vector2, Vector2, const PathOptions &)
// covers: rmp::behavior::Area::_update(Object &, float)
// covers: rmp::behavior::Area::_collision(Object &, Object &)
// covers: rmp::behavior::Area::contains(const Object &) const
// covers: rmp::behavior::Area::overlapping() const
// covers: rmp::behavior::StateMachine::_update(Object &, float)
// covers: rmp::behavior::StateMachine::go(std::string_view)
// covers: rmp::behavior::StateMachine::state() const
// covers: rmp::behavior::StateMachine::seconds_in_state() const
// covers: rmp::behavior::kinds [template]
// covers: rmp::Object::enable [template]
// covers: rmp::Object::enabled [template]
// covers: rmp::Condition::Condition [template]
// covers: rmp::Condition::operator bool [template]
// covers: rmp::Condition::operator() [template]
// covers: rmp::behavior::Hitbox::_update(Object &, float)
// covers: rmp::behavior::Hitbox::_collision(Object &, Object &)
// covers: rmp::behavior::Collectible::_update(Object &, float)
// covers: rmp::behavior::Collectible::_collision(Object &, Object &)
// covers: rmp::behavior::Collectible::collected() const
// covers: rmp::behavior::Shooter::_update(Object &, float)
// covers: rmp::behavior::Shooter::fire(Object &)
// covers: rmp::behavior::Shooter::shots() const
// covers: rmp::behavior::Orbit::_late_update(Object &, float)
// covers: rmp::behavior::Animator::_late_update(Object &, float)
// covers: rmp::behavior::Animator::playing() const
// covers: rmp::behavior::Draggable::_ready(Object &)
// covers: rmp::behavior::Draggable::_update(Object &, float)
// covers: rmp::behavior::Draggable::drop(Object &)
// covers: rmp::behavior::Draggable::held() const
// covers: rmp::behavior::Patrol::_update(Object &, float)
// covers: rmp::behavior::Patrol::heading_for() const
// covers: rmp::behavior::Pathfind::_update(Object &, float)
// covers: rmp::behavior::Pathfind::path() const
// covers: rmp::behavior::Pathfind::arrived() const
// covers: rmp::behavior::Chase::_ready(Object &)
// covers: rmp::behavior::Chase::_update(Object &, float)
// covers: rmp::behavior::Chase::chasing() const
// covers: rmp::behavior::Chase::sees() const
// covers: rmp::behavior::Patrol::from(const MapObject &)
// covers: rmp::behavior::Chase::from(const MapObject &)
// covers: rmp::behavior::Orbit::from(const MapObject &)
// covers: rmp::behavior::Shooter::from(const MapObject &)
// covers: rmp::behavior::Hitbox::from(const MapObject &)
// covers: rmp::behavior::Collectible::from(const MapObject &)
// covers: rmp::Tilemap::draw(Rectangle) const

#include "check.h"
#include "gen.h"

#include "../../src/rmp/internal.h"
#include "../../src/rmp/object_internal.h"
#include "../../src/rmp/tilemap_internal.h"

#include <rmp/assets.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/tilemap.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using rmp::test::gen::NOMINAL_SIZE;
namespace b = rmp::behavior;

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

struct {
    rmp::input::detail::DeviceState devices;
} fake;
void fake_sample(rmp::input::detail::DeviceState &out) { out = fake.devices; }

struct Session {
    Session() { reset(); }
    ~Session() {
        reset();
        rmp::detail::release_all();
        SetTraceLogLevel(LOG_INFO);
    }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Session(Session &&) = delete;
    Session &operator=(Session &&) = delete;
    static void reset() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
        rmp::objects::detail::reset_pointer_for_tests();
        rmp::input::detail::reset();
        fake.devices = rmp::input::detail::DeviceState{};
        rmp::input::detail::set_sample_provider(fake_sample);
        rmp::detail::reset_reports_for_tests();
        SetTraceLogLevel(LOG_NONE);
    }
};

void frame(rmp::Scene &scene, float delta) {
    rmp::input::detail::begin_frame();
    rmp::objects::detail::update(scene, delta);
    rmp::objects::detail::collide(scene);
    rmp::objects::detail::collect();
}

float between(float lo, float hi) {
    const int k = *rc::gen::resize(NOMINAL_SIZE, rc::gen::inRange(0, 1001));
    return lo + (hi - lo) * static_cast<float>(k) / 1000.0f;
}

float field(bool hostile, float lo, float hi) {
    if (hostile && *rc::gen::inRange(0, 3) == 0) return *rmp::test::gen::hostile_float();
    return between(lo, hi);
}

bool finite(Vector2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

// A map of 16-unit cells, `rows` of '#' and '.'.
std::string tiled(const std::vector<std::string> &rows) {
    std::string data;
    for (const std::string &row : rows) {
        for (const char c : row) {
            if (!data.empty()) data += ",";
            data += c == '#' ? "1" : "0";
        }
    }
    const std::string hs = std::to_string(rows.size());
    const std::string ws = std::to_string(rows[0].size());
    return R"({"compressionlevel":-1,"height":)" + hs + R"(,"width":)" + ws +
        R"(,"infinite":false,"layers":[{"class":"solid","data":[)" + data +
        R"(],"height":)" + hs + R"(,"width":)" + ws +
        R"(,"id":1,"name":"walls","opacity":1,"type":"tilelayer","visible":true,"x":0,"y":0}],)"
        R"("nextlayerid":2,"nextobjectid":1,"orientation":"orthogonal","renderorder":"right-down",)"
        R"("tiledversion":"1.5.0","tileheight":16,"tilewidth":16,"type":"map","version":"1.5",)"
        R"("tilesets":[{"columns":1,"firstgid":1,"image":"none.png","imageheight":16,)"
        R"("imagewidth":16,"margin":0,"name":"t","spacing":0,"tilecount":1,"tileheight":16,)"
        R"("tilewidth":16}]})";
}

void give_map(rmp::Tilemap &map, const std::vector<std::string> &rows) {
    const std::string text = tiled(rows);
    rmp::tilemap::detail::Access::adopt(
        map,
        rmp::tilemap::detail::parse_map(text.data(), static_cast<int>(text.size()),
                                        "prop.json", ""));
}

std::vector<std::string> any_grid() {
    const int w = *rc::gen::inRange(1, 13);
    const int h = *rc::gen::inRange(1, 10);
    const int walls = *rc::gen::inRange(0, 50); // percent
    std::vector<std::string> rows(static_cast<std::size_t>(h),
                                  std::string(static_cast<std::size_t>(w), '.'));
    for (std::string &row : rows) {
        for (char &c : row) c = *rc::gen::inRange(0, 100) < walls ? '#' : '.';
    }
    return rows;
}

Vector2 centre(int c, int r) {
    return Vector2{ static_cast<float>(c) * 16 + 8, static_cast<float>(r) * 16 + 8 };
}

// Dijkstra over the same cells and the same moves: what find_path() must
// agree with. Infinity when there is no way.
float shortest(const std::vector<std::string> &rows, int sc, int sr, int gc, int gr,
               bool diagonal) {
    const int h = static_cast<int>(rows.size());
    const int w = static_cast<int>(rows[0].size());
    const auto wall = [&](int c, int r) {
        return c < 0 || r < 0 || c >= w || r >= h ||
            rows[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)] == '#';
    };
    std::vector<float> cost(static_cast<std::size_t>(w * h),
                            std::numeric_limits<float>::infinity());
    using Entry = std::pair<float, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    cost[(static_cast<std::size_t>(sr) * static_cast<std::size_t>(w)) +
         static_cast<std::size_t>(sc)] = 0;
    open.emplace(0.0f, sr * w + sc);
    const float slant = std::sqrt(16.0f * 16 + 16.0f * 16);
    while (!open.empty()) {
        const auto [d, at] = open.top();
        open.pop();
        if (d > cost[static_cast<std::size_t>(at)]) continue;
        const int c = at % w;
        const int r = at / w;
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if ((dc == 0 && dr == 0) || (!diagonal && dc != 0 && dr != 0)) continue;
                if (wall(c + dc, r + dr)) continue;
                if (dc != 0 && dr != 0 && (wall(c + dc, r) || wall(c, r + dr))) continue;
                const float next = d + (dc != 0 && dr != 0 ? slant : 16.0f);
                const auto to =
                    (static_cast<std::size_t>(r + dr) * static_cast<std::size_t>(w)) +
                    static_cast<std::size_t>(c + dc);
                if (next < cost[to]) {
                    cost[to] = next;
                    open.emplace(next, static_cast<int>(to));
                }
            }
        }
    }
    return cost[(static_cast<std::size_t>(gr) * static_cast<std::size_t>(w)) +
                static_cast<std::size_t>(gc)];
}

// The world's objects, after any frames: finite wherever they were.
void all_finite(const World &world) {
    for (const auto &o : rmp::objects::detail::live_objects(world)) {
        RC_ASSERT(finite(o->position));
        RC_ASSERT(finite(o->velocity));
        RC_ASSERT(finite(o->scale));
        RC_ASSERT(std::isfinite(o->rotation));
    }
}

std::string read(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

} // namespace

TEST_SUITE("property: behaviors, second part") {
    TEST_CASE("find_path agrees with Dijkstra on any grid") {
        rmp::test::check(
            "a way exactly when there is one, as short, walkable, ending on `to`", [] {
                const Session session;
                const auto rows = any_grid();
                const int w = static_cast<int>(rows[0].size());
                const int h = static_cast<int>(rows.size());
                const int sc = *rc::gen::inRange(0, w);
                const int sr = *rc::gen::inRange(0, h);
                const int gc = *rc::gen::inRange(0, w);
                const int gr = *rc::gen::inRange(0, h);
                const bool diagonal = *rc::gen::arbitrary<bool>();
                rmp::Tilemap map;
                give_map(map, rows);
                RC_ASSERT(map.valid());
                const Vector2 to{ centre(gc, gr).x + between(-7, 7),
                                  centre(gc, gr).y + between(-7, 7) };
                const auto path =
                    rmp::find_path(map, centre(sc, sr), to, { .diagonal = diagonal });
                const bool ends_open =
                    rows[static_cast<std::size_t>(sr)][static_cast<std::size_t>(sc)] !=
                        '#' &&
                    rows[static_cast<std::size_t>(gr)][static_cast<std::size_t>(gc)] !=
                        '#';
                const float best = ends_open ? shortest(rows, sc, sr, gc, gr, diagonal)
                                             : std::numeric_limits<float>::infinity();
                RC_ASSERT(path.empty() == !std::isfinite(best));
                if (path.empty()) return;
                RC_ASSERT(path.back().x == to.x);
                RC_ASSERT(path.back().y == to.y);
                if (sc == gc && sr == gr) {
                    RC_ASSERT(path.size() == 1U); // already there
                    return;
                }
                Vector2 was = centre(sc, sr);
                float length = 0;
                for (std::size_t i = 0; i < path.size(); ++i) {
                    // The last point is `to`, inside the goal's cell.
                    const Vector2 p = i + 1 == path.size() ? centre(gc, gr) : path[i];
                    const float dx = std::fabs(p.x - was.x);
                    const float dy = std::fabs(p.y - was.y);
                    RC_ASSERT(dx <= 16.01f);
                    RC_ASSERT(dy <= 16.01f);
                    RC_ASSERT(dx + dy > 0);
                    if (!diagonal) RC_ASSERT(dx < 0.01f || dy < 0.01f);
                    RC_ASSERT(!map.solid_at(p));
                    if (dx > 0.01f && dy > 0.01f) {
                        RC_ASSERT(!map.solid_at(Vector2{ p.x, was.y }));
                        RC_ASSERT(!map.solid_at(Vector2{ was.x, p.y }));
                    }
                    length += std::sqrt(dx * dx + dy * dy);
                    was = p;
                }
                RC_ASSERT(std::fabs(length - best) < 0.05f);
                // The map draws through its culled path too, here
                // with no GPU: whatever area, it only looks.
                map.draw(Rectangle{ between(-50, 200), between(-50, 200), between(0, 300),
                                    between(0, 300) });
            });
    }

    TEST_CASE("an Area tells what a model of it says, over any path across it") {
        rmp::test::check(
            "enter, stay, inside, outside and exit, in order, once each", [] {
                const Session session;
                World world;
                rmp::Object &zone =
                    world.spawn({ .position = { 0, 0 }, .size = { 100, 100 } });
                std::vector<std::string> said;
                zone.add<b::Area>({
                    .on_enter = [&](rmp::Object &,
                                    rmp::Object &) { said.emplace_back("enter"); },
                    .on_stay = [&](rmp::Object &,
                                   rmp::Object &) { said.emplace_back("stay"); },
                    .on_exit = [&](rmp::Object &,
                                   rmp::Object &) { said.emplace_back("exit"); },
                    .on_inside = [&](rmp::Object &,
                                     rmp::Object &) { said.emplace_back("inside"); },
                    .on_outside = [&](rmp::Object &,
                                      rmp::Object &) { said.emplace_back("outside"); },
                });
                rmp::Object &mover =
                    world.spawn({ .position = { 500, 0 }, .size = { 10, 10 } });
                const int steps = *rc::gen::inRange(1, 30);
                // The model: whether it touched and was wholly inside, frame by frame.
                bool touched = false;
                bool was_inside = false;
                bool settled_touch = false; // what the area knows at its update
                bool settled_inside = false;
                std::vector<std::string> expected;
                for (int i = 0; i < steps; ++i) {
                    mover.position = { between(-80, 80), between(-80, 80) };
                    // Its update settles last frame's pass first.
                    if (settled_touch && !touched) {
                        if (settled_inside) expected.emplace_back("outside");
                        expected.emplace_back("exit");
                    }
                    settled_touch = touched;
                    settled_inside = was_inside;
                    const float x = mover.position.x;
                    const float y = mover.position.y;
                    const bool touch = std::fabs(x) <= 55 && std::fabs(y) <= 55;
                    const bool inside = std::fabs(x) <= 45 && std::fabs(y) <= 45;
                    if (touch && !settled_touch) {
                        expected.emplace_back("enter");
                        if (inside) expected.emplace_back("inside");
                    } else if (touch) {
                        expected.emplace_back("stay");
                        if (inside && !settled_inside) expected.emplace_back("inside");
                        if (!inside && settled_inside) expected.emplace_back("outside");
                    }
                    touched = touch;
                    was_inside = touch && inside;
                    frame(world, 1.0f / 60);
                    RC_ASSERT(said == expected);
                    const auto area = zone.get<b::Area>();
                    RC_ASSERT(area->overlapping().size() == (settled_touch ? 1U : 0U));
                    RC_ASSERT(area->contains(mover) == settled_touch);
                }
            });
    }

    TEST_CASE("a StateMachine follows its transitions as a model of it does") {
        rmp::test::check(
            "the first matching transition is taken; go() wins at the next update", [] {
                const Session session;
                World world;
                rmp::Object &o = world.spawn({ .size = { 4, 4 } });
                struct Kept {
                    int updates = 0;
                    void _update(rmp::Object &self, float delta) {
                        (void)self;
                        (void)delta;
                        ++updates;
                    }
                };
                o.add<Kept>();
                std::vector<int> answers(4, 0);
                o.add<b::StateMachine>();
                auto m = o.get<b::StateMachine>();
                for (const std::string_view name : { "a", "b", "c" }) {
                    m->states.push_back(b::State{
                        .name = std::string(name),
                        .enables = name == "b" ? b::kinds<Kept>() : std::vector<int>{} });
                }
                const std::vector<std::pair<std::string, std::string>> moves = {
                    { "a", "b" }, { "b", "c" }, { "", "a" }, { "c", "b" }
                };
                for (std::size_t i = 0; i < moves.size(); ++i) {
                    m->transitions.push_back(
                        b::Transition{ .from = moves[i].first,
                                       .to = moves[i].second,
                                       .when = [&answers, i](rmp::Object &) {
                                           return answers[i] != 0;
                                       } });
                }
                std::string model;
                const int steps = *rc::gen::inRange(1, 40);
                for (int s = 0; s < steps; ++s) {
                    for (int &a : answers) a = *rc::gen::inRange(0, 2);
                    std::string wanted;
                    if (*rc::gen::inRange(0, 5) == 0) {
                        wanted = *rc::gen::element<std::string>("a", "b", "c", "z");
                        m->go(wanted);
                    }
                    if (model.empty()) model = "a";
                    if (!wanted.empty() && wanted != "z") model = wanted;
                    for (std::size_t i = 0; i < moves.size(); ++i) {
                        if (!moves[i].first.empty() && moves[i].first != model) continue;
                        if (moves[i].second == model) continue;
                        if (answers[i] == 0) continue;
                        model = moves[i].second;
                        break;
                    }
                    frame(world, 1.0f / 60);
                    RC_ASSERT(std::string(m->state()) == model);
                    RC_ASSERT(o.enabled<Kept>() == (model == "b"));
                    RC_ASSERT(m->seconds_in_state() >= 0);
                }
            });
    }

    TEST_CASE("every new behavior with any fields keeps the world finite") {
        rmp::test::check(
            "any fields, any frames: no UB, no hang, nothing written but numbers", [] {
                const Session session;
                const bool hostile = *rc::gen::arbitrary<bool>();
                World world;
                if (*rc::gen::arbitrary<bool>()) give_map(world.map, any_grid());
                std::vector<rmp::Handle<>> made;
                const int count = *rc::gen::inRange(1, 7);
                for (int i = 0; i < count; ++i) {
                    rmp::Object &o =
                        world.spawn({ .position = { between(0, 150), between(0, 120) },
                                      .size = { between(1, 30), between(1, 30) } });
                    o.collision_layer = 1U << *rc::gen::inRange(0, 3);
                    o.collision_mask = static_cast<unsigned>(*rc::gen::inRange(0, 8));
                    o.gravity_scale = *rc::gen::inRange(0, 3) == 0 ? 1.0f : 0.0f;
                    made.push_back(o.handle<>());
                }
                const auto other = [&] {
                    return made[static_cast<std::size_t>(
                        *rc::gen::inRange(0, static_cast<int>(made.size())))];
                };
                for (const rmp::Handle<> &handle : made) {
                    const rmp::Ref<rmp::Object> o = handle.get();
                    switch (*rc::gen::inRange(0, 11)) {
                        case 0:
                            o->add<b::Area>();
                            break;
                        case 1:
                            o->add<b::Health>({ .hp = 5 });
                            o->add<b::Hitbox>(
                                { .damage = 1,
                                  .knockback = field(hostile, 0, 300),
                                  .once_per_target = *rc::gen::arbitrary<bool>() });
                            break;
                        case 2:
                            o->add<b::Collectible>(
                                { .magnet = field(hostile, 0, 100),
                                  .magnet_speed = field(hostile, 0, 400),
                                  .vanish_seconds = field(hostile, 0, 1) });
                            break;
                        case 3:
                            o->add<b::Shooter>(
                                { .automatic = true,
                                  .every = field(hostile, 0, 1),
                                  .target = *rc::gen::arbitrary<bool>() ? other()
                                                                        : rmp::Handle<>{},
                                  .range = field(hostile, 0, 200),
                                  .direction = { field(hostile, -1, 1),
                                                 field(hostile, -1, 1) },
                                  .speed = field(hostile, 0, 400),
                                  .count = *rc::gen::inRange(-2, 12),
                                  .spread = field(hostile, 0, 400),
                                  .rotate_per_shot = field(hostile, -90, 90),
                                  .bullet_radius = field(hostile, 0, 8),
                                  .bullet_life = field(hostile, 0, 1) });
                            (void)o->get<b::Shooter>()->fire(*o);
                            (void)o->get<b::Shooter>()->shots();
                            break;
                        case 4:
                            o->add<b::Orbit>(
                                { .centre = *rc::gen::arbitrary<bool>() ? other()
                                                                        : rmp::Handle<>{},
                                  .point = { field(hostile, -100, 100),
                                             field(hostile, -100, 100) },
                                  .radius = field(hostile, 0, 80),
                                  .degrees_per_second = field(hostile, -720, 720),
                                  .angle = field(hostile, -360, 360),
                                  .face = *rc::gen::element(
                                      b::Face::NONE, b::Face::CENTRE, b::Face::OUTWARD),
                                  .index = *rc::gen::inRange(-2, 5),
                                  .of = *rc::gen::inRange(-2, 5) });
                            break;
                        case 5:
                            o->add<b::Animator>({ .moving_at = field(hostile, 0, 50) });
                            break;
                        case 6:
                            o->add<b::Draggable>(
                                { .snap = { field(hostile, 0, 32),
                                            field(hostile, 0, 32) },
                                  .throwable = *rc::gen::arbitrary<bool>(),
                                  .friction = field(hostile, 0, 10),
                                  .return_if_rejected = *rc::gen::arbitrary<bool>() });
                            break;
                        case 7:
                            o->add<b::Patrol>(
                                { .speed = field(hostile, 0, 200),
                                  .wait = field(hostile, 0, 1),
                                  .ping_pong = *rc::gen::arbitrary<bool>() });
                            if (*rc::gen::arbitrary<bool>()) {
                                o->get<b::Patrol>()->waypoints = {
                                    { between(0, 150), between(0, 120) },
                                    { field(hostile, 0, 150), field(hostile, 0, 120) }
                                };
                            }
                            break;
                        case 8:
                            o->add<b::Pathfind>(
                                { .target = *rc::gen::arbitrary<bool>() ? other()
                                                                        : rmp::Handle<>{},
                                  .goal = Vector2{ field(hostile, 0, 200),
                                                   field(hostile, 0, 200) },
                                  .speed = field(hostile, 0, 200),
                                  .diagonal = *rc::gen::arbitrary<bool>(),
                                  .clearance = field(hostile, 0, 20),
                                  .repath_every = field(hostile, 0, 1) });
                            break;
                        case 9:
                            if (*rc::gen::arbitrary<bool>()) o->add<b::Patrol>();
                            if (*rc::gen::arbitrary<bool>()) o->add<b::Pathfind>();
                            o->add<b::Chase>(
                                { .target = other(),
                                  .speed = field(hostile, 0, 200),
                                  .sight = field(hostile, 0, 200),
                                  .fov = field(hostile, 0, 400),
                                  .line_of_sight = *rc::gen::arbitrary<bool>(),
                                  .lose_after = field(hostile, 0, 1),
                                  .stop_distance = field(hostile, 0, 20) });
                            break;
                        default:
                            o->add<b::StateMachine>({ .initial = "x" });
                            o->get<b::StateMachine>()->states.push_back(
                                b::State{ .name = "x" });
                            break;
                    }
                }
                const int frames = *rc::gen::inRange(1, 60);
                for (int f = 0; f < frames; ++f) {
                    fake.devices.mouse[0] = *rc::gen::arbitrary<bool>();
                    for (const auto &o : rmp::objects::detail::live_objects(world)) {
                        if (o->has<b::Draggable>() && *rc::gen::inRange(0, 3) == 0) {
                            rmp::Storage::notify_drag(
                                *o, { between(-20, 20), between(-20, 20) });
                        }
                    }
                    // A frame as the clock hands one (clamp_delta): the
                    // behaviors' own guard against any delta at all is
                    // tests/property/behavior_prop.cpp's.
                    frame(world, between(0, 0.1f));
                    all_finite(world);
                }
                // The hooks, called by hand as a game may, between
                // any two objects: finite still.
                const auto alive = rmp::objects::detail::live_objects(world);
                for (const auto &o : alive) {
                    rmp::Object &with = *alive[static_cast<std::size_t>(
                        *rc::gen::inRange(0, static_cast<int>(alive.size())))];
                    if (!o->alive() || !with.alive()) continue;
                    if (auto a = o->get<b::Area>()) a->_collision(*o, with);
                    if (auto h = o->get<b::Hitbox>()) h->_collision(*o, with);
                    if (auto c = o->get<b::Collectible>()) c->_collision(*o, with);
                    if (auto r = o->get<b::Orbit>()) r->_late_update(*o, 1.0f / 60);
                    if (auto a = o->get<b::Animator>()) a->_late_update(*o, 1.0f / 60);
                    if (auto d = o->get<b::Draggable>()) d->_ready(*o);
                    if (auto c = o->get<b::Chase>()) c->_ready(*o);
                    const rmp::Condition<rmp::Object &> moving = [](rmp::Object &it) {
                        return it.velocity.x != 0;
                    };
                    o->enable<b::Patrol>(!moving(*o));
                }
                all_finite(world);
                // Every question answers, whatever happened.
                for (const auto &o : rmp::objects::detail::live_objects(world)) {
                    if (auto c = o->get<b::Collectible>()) (void)c->collected();
                    if (auto a = o->get<b::Animator>()) (void)a->playing();
                    if (auto d = o->get<b::Draggable>()) {
                        (void)d->held();
                        d->drop(*o);
                    }
                    if (auto p = o->get<b::Patrol>()) RC_ASSERT(p->heading_for() >= 0);
                    if (auto p = o->get<b::Pathfind>()) {
                        (void)p->arrived();
                        for (const Vector2 v : p->path()) RC_ASSERT(finite(v));
                    }
                    if (auto c = o->get<b::Chase>()) {
                        (void)c->chasing();
                        (void)c->sees();
                    }
                }
            });
    }

    TEST_CASE("B::from() reads every documented field as the entity holds it") {
        rmp::test::check(
            "each field as property_*() reads it, the default where there is none", [] {
                const Session session;
                std::vector<std::filesystem::path> files;
                for (const auto &entry : std::filesystem::directory_iterator(
                         std::string(RMP_TEST_FIXTURES) + "ldtk")) {
                    if (entry.path().extension() == ".ldtk")
                        files.push_back(entry.path());
                }
                std::ranges::sort(files);
                RC_ASSERT(!files.empty());
                const auto &file = files[static_cast<std::size_t>(
                    *rc::gen::inRange(0, static_cast<int>(files.size())))];
                const std::string text = read(file);
                World world;
                rmp::tilemap::detail::Access::adopt(
                    world.map,
                    rmp::tilemap::detail::parse_map(
                        text.data(), static_cast<int>(text.size()),
                        file.filename().string().c_str(), ""));
                if (!world.map.valid()) return;
                RC_ASSERT(rmp::tilemap::detail::Access::data(world.map) != nullptr);
                for (const rmp::MapObject &o :
                     rmp::tilemap::detail::Access::data(world.map)->objects) {
                    const b::Patrol p = b::Patrol::from(o);
                    RC_ASSERT(p.speed == o.property_float("speed", b::Patrol{}.speed) ||
                              std::isnan(p.speed));
                    RC_ASSERT(p.ping_pong == o.property_bool("ping_pong", false));
                    const int points = o.property_count("path");
                    RC_ASSERT(p.waypoints.size() ==
                              (points > 0 ? static_cast<std::size_t>(points) + 1 : 0U));
                    const b::Chase c = b::Chase::from(o);
                    RC_ASSERT(c.sight == o.property_float("sight", b::Chase{}.sight));
                    RC_ASSERT(c.fov == o.property_float("fov", b::Chase{}.fov));
                    const b::Orbit r = b::Orbit::from(o);
                    RC_ASSERT(r.radius == o.property_float("radius", b::Orbit{}.radius));
                    RC_ASSERT(r.angle == o.property_float("angle", 0));
                    const b::Shooter s = b::Shooter::from(o);
                    RC_ASSERT(s.count == o.property_int("count", 1));
                    RC_ASSERT(s.every == o.property_float("every", b::Shooter{}.every));
                    const b::Hitbox h = b::Hitbox::from(o);
                    RC_ASSERT(h.damage == o.property_int("damage", 1));
                    RC_ASSERT(h.knockback == o.property_float("knockback", 0));
                    RC_ASSERT(b::Collectible::from(o).magnet ==
                              o.property_float("magnet", 0));
                }
            });
    }
}
