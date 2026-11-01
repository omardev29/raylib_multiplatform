// ===========================================================================
// The behaviors of the catalogue's second and third parts -- Area, Hitbox,
// Collectible, Shooter, Orbit, Animator, Draggable, Patrol, Pathfind, Chase
// and StateMachine -- rmp::find_path(), Object::enable<B>() and
// rmp::Condition, and every B::from(MapObject) against an LDtk file whose
// entities carry the fields the header documents.
//
// Like tests/behaviors_test.cpp: no window, input through the fake devices,
// and time a number the test passes. A map is parsed from text written here
// ('#' a wall), so the walls are where the test says.
// ===========================================================================

#include <doctest.h>

#include "../src/rmp/animation_internal.h"
#include "../src/rmp/behavior_internal.h"
#include "../src/rmp/internal.h"
#include "../src/rmp/object_internal.h"
#include "../src/rmp/tilemap_internal.h"

#include <rmp/assets.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>
#include <rmp/tilemap.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace {

using rmp::behavior::AnimationRule;
using rmp::behavior::Animator;
using rmp::behavior::Area;
using rmp::behavior::Chase;
using rmp::behavior::Collectible;
using rmp::behavior::Draggable;
using rmp::behavior::Face;
using rmp::behavior::Health;
using rmp::behavior::Hitbox;
using rmp::behavior::kinds;
using rmp::behavior::Lifespan;
using rmp::behavior::Orbit;
using rmp::behavior::Pathfind;
using rmp::behavior::Patrol;
using rmp::behavior::Projectile;
using rmp::behavior::Shooter;
using rmp::behavior::State;
using rmp::behavior::StateMachine;
using rmp::behavior::Transition;

class World : public rmp::Scene {
public:
    ~World() override { rmp::objects::detail::release_scene(*this); }
};

struct {
    rmp::input::detail::DeviceState devices;
} fake;

void fake_sample(rmp::input::detail::DeviceState &out) { out = fake.devices; }

struct Fixture {
    Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
        rmp::objects::detail::reset_pointer_for_tests();
        rmp::input::detail::reset();
        fake.devices = rmp::input::detail::DeviceState{};
        rmp::input::detail::set_sample_provider(fake_sample);
        rmp::detail::reset_reports_for_tests();
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    Fixture(Fixture &&) = delete;
    Fixture &operator=(Fixture &&) = delete;
    ~Fixture() {
        rmp::objects::detail::reset_for_tests();
        rmp::objects::detail::reset_behaviors_for_tests();
        rmp::input::detail::reset();
        fake.devices = rmp::input::detail::DeviceState{};
        rmp::detail::release_all();
    }
};

constexpr float STEP = 1.0f / 60;

// A whole frame of a scene, the way the scene stack drives it.
void frame(rmp::Scene &scene, float delta = STEP) {
    rmp::input::detail::begin_frame();
    rmp::objects::detail::update(scene, delta);
    rmp::objects::detail::collide(scene);
    rmp::objects::detail::collect();
}

void frames(rmp::Scene &scene, int n, float delta = STEP) {
    for (int i = 0; i < n; ++i) frame(scene, delta);
}

rmp::Object &box(World &world, Vector2 at, Vector2 size) {
    return world.spawn({ .position = at, .shape = rmp::rect(size) });
}

// A Tiled map of 16-unit cells, one row a string: '#' is a solid cell.
std::string tiled(const std::vector<std::string> &rows) {
    const std::size_t h = rows.size();
    const std::size_t w = rows[0].size();
    std::string data;
    for (const std::string &row : rows) {
        for (const char c : row) {
            if (!data.empty()) data += ",";
            data += c == '#' ? "1" : "0";
        }
    }
    const std::string hs = std::to_string(h);
    const std::string ws = std::to_string(w);
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
                                        "test.json", ""));
    REQUIRE(map.valid());
}

// The centre of a cell of those maps.
Vector2 cell(int column, int row) {
    return Vector2{ static_cast<float>(column) * 16 + 8,
                    static_cast<float>(row) * 16 + 8 };
}

float distance(Vector2 a, Vector2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

rmp::SpriteSheet sheet_with(const std::vector<std::string> &names) {
    rmp::SheetData data{};
    data.width = 16;
    data.height = 16;
    data.frames.resize(1);
    data.frames[0].seconds = 0.1f;
    data.tags.resize(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) data.tags[i].name = names[i];
    rmp::SpriteSheet sheet{ rmp::detail::adopt(rmp::detail::ResourceKind::SHEET,
                                               std::move(data)) };
    REQUIRE(sheet.valid());
    return sheet;
}

// A behavior that counts its hooks.
struct Counter {
    int updates = 0;
    int lates = 0;
    int draws = 0;
    int hits = 0;
    int ends = 0;
    void _update(rmp::Object &self, float delta) {
        (void)self;
        (void)delta;
        ++updates;
    }
    void _late_update(rmp::Object &self, float delta) {
        (void)self;
        (void)delta;
        ++lates;
    }
    void _draw(rmp::Object &self) {
        (void)self;
        ++draws;
    }
    void _collision(rmp::Object &self, rmp::Object &other) {
        (void)self;
        (void)other;
        ++hits;
    }
    void _end(rmp::Object &self) {
        (void)self;
        ++ends;
    }
};

struct Other {
    int updates = 0;
    void _update(rmp::Object &self, float delta) {
        (void)self;
        (void)delta;
        ++updates;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// enable<B>() and Condition
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "enable<B>(false) stops every hook but _end, and keeps the state") {
    World world;
    rmp::Object &a = box(world, { 0, 0 }, { 10, 10 });
    box(world, { 2, 0 }, { 10, 10 });
    a.add<Counter>();
    CHECK(a.enabled<Counter>());
    frame(world);
    rmp::objects::detail::draw_behaviors(a);
    const Counter seen = *a.get<Counter>();
    CHECK(seen.updates == 1);
    CHECK(seen.lates == 1);
    CHECK(seen.draws == 1);
    CHECK(seen.hits == 1);

    a.enable<Counter>(false);
    CHECK_FALSE(a.enabled<Counter>());
    CHECK(a.has<Counter>());
    frames(world, 3);
    rmp::objects::detail::draw_behaviors(a);
    CHECK(a.get<Counter>()->updates == 1);
    CHECK(a.get<Counter>()->lates == 1);
    CHECK(a.get<Counter>()->draws == 1);
    CHECK(a.get<Counter>()->hits == 1);

    a.enable<Counter>();
    frame(world);
    CHECK(a.get<Counter>()->updates == 2);

    a.enable<Counter>(false);
    a.remove<Counter>(); // _end still runs: it is the end of its life
    CHECK_FALSE(a.has<Counter>());
}

TEST_CASE_FIXTURE(Fixture,
                  "enable<B>() of a behavior the object has not got does nothing") {
    World world;
    rmp::Object &a = box(world, { 0, 0 }, { 10, 10 });
    a.enable<Counter>(false);
    CHECK_FALSE(a.enabled<Counter>());
    a.add<Other>();
    a.enable<Counter>(true);
    CHECK_FALSE(a.enabled<Counter>());
    CHECK(a.enabled<Other>());
    // Paused, then added again: the new one runs.
    a.enable<Other>(false);
    a.add<Other>();
    CHECK(a.enabled<Other>());
    rmp::Object loose;
    loose.enable<Other>(false);
    CHECK_FALSE(loose.enabled<Other>());
}

TEST_CASE("a Condition answers its function, and an empty one false") {
    const rmp::Condition<int> empty;
    CHECK_FALSE(static_cast<bool>(empty));
    CHECK_FALSE(empty(3));
    rmp::Condition<int> odd = [](int n) { return n % 2 == 1; };
    CHECK(static_cast<bool>(odd));
    CHECK(odd(3));
    CHECK_FALSE(odd(4));
    rmp::Condition<int> moved = std::move(odd);
    CHECK(moved(5));
    // What a move leaves: nothing.
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): what a move leaves
    CHECK_FALSE(static_cast<bool>(odd));
    rmp::Condition<int> again;
    again = std::move(moved);
    CHECK(again(7));
    // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move): what a move leaves
    CHECK_FALSE(moved(7));
}

// ---------------------------------------------------------------------------
// Area
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "Area: enter, stay, inside, outside and exit, in order") {
    World world;
    rmp::Object &zone = box(world, { 0, 0 }, { 100, 100 });
    std::vector<std::string> said;
    zone.add<Area>({
        .on_enter = [&](rmp::Object &, rmp::Object &) { said.emplace_back("enter"); },
        .on_stay = [&](rmp::Object &, rmp::Object &) { said.emplace_back("stay"); },
        .on_exit = [&](rmp::Object &, rmp::Object &) { said.emplace_back("exit"); },
        .on_inside = [&](rmp::Object &, rmp::Object &) { said.emplace_back("inside"); },
        .on_outside = [&](rmp::Object &, rmp::Object &) { said.emplace_back("outside"); },
    });
    rmp::Object &mover = box(world, { 200, 0 }, { 10, 10 });

    frame(world);
    CHECK(said.empty());
    mover.position = { 52, 0 }; // half in
    frame(world);
    CHECK(said == std::vector<std::string>{ "enter" });
    mover.position = { 0, 0 };
    frame(world);
    CHECK(said == std::vector<std::string>{ "enter", "stay", "inside" });
    frame(world);
    CHECK(zone.get<Area>()->contains(mover));
    CHECK(zone.get<Area>()->overlapping().size() == 1);
    said.clear();
    mover.position = { 52, 0 };
    frame(world);
    CHECK(said == std::vector<std::string>{ "stay", "outside" });
    said.clear();
    mover.position = { 300, 0 };
    frame(world);
    CHECK(said.empty());
    frame(world);
    CHECK(said == std::vector<std::string>{ "exit" });
    CHECK(zone.get<Area>()->overlapping().empty());
}

TEST_CASE_FIXTURE(Fixture,
                  "Area: a jump from wholly inside to gone is outside, then exit") {
    World world;
    rmp::Object &zone = box(world, { 0, 0 }, { 100, 100 });
    std::vector<std::string> said;
    zone.add<Area>({
        .on_exit = [&](rmp::Object &, rmp::Object &) { said.emplace_back("exit"); },
        .on_inside = [&](rmp::Object &, rmp::Object &) { said.emplace_back("inside"); },
        .on_outside = [&](rmp::Object &, rmp::Object &) { said.emplace_back("outside"); },
    });
    rmp::Object &mover = box(world, { 0, 0 }, { 10, 10 });
    frame(world);
    CHECK(said == std::vector<std::string>{ "inside" });
    mover.position = { 500, 500 };
    frames(world, 2);
    CHECK(said == std::vector<std::string>{ "inside", "outside", "exit" });
}

TEST_CASE_FIXTURE(Fixture, "Area: an object destroyed inside leaves with nothing said") {
    World world;
    rmp::Object &zone = box(world, { 0, 0 }, { 100, 100 });
    int exits = 0;
    zone.add<Area>({ .on_exit = [&](rmp::Object &, rmp::Object &) { ++exits; } });
    rmp::Object &mover = box(world, { 0, 0 }, { 10, 10 });
    frames(world, 2);
    REQUIRE(zone.get<Area>()->overlapping().size() == 1);
    mover.destroy();
    frames(world, 3);
    CHECK(exits == 0);
    CHECK(zone.get<Area>()->overlapping().empty());
}

TEST_CASE_FIXTURE(Fixture, "Area: it sees only what its object collides with") {
    World world;
    rmp::Object &zone = box(world, { 0, 0 }, { 100, 100 });
    zone.collision_layer = 0;
    zone.collision_mask = 2;
    int enters = 0;
    zone.add<Area>({ .on_enter = [&](rmp::Object &, rmp::Object &) { ++enters; } });
    rmp::Object &a = box(world, { 0, 0 }, { 10, 10 });
    a.collision_layer = 1;
    rmp::Object &b = box(world, { 10, 0 }, { 10, 10 });
    b.collision_layer = 2;
    frame(world);
    CHECK(enters == 1);
}

// ---------------------------------------------------------------------------
// Hitbox, and Health not counting it twice
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(
    Fixture, "Hitbox: one touch costs damage once, though the Health is hurt_by it") {
    World world;
    rmp::Object &spike = box(world, { 0, 0 }, { 10, 10 });
    spike.collision_layer = 4;
    int hit = 0;
    spike.add<Hitbox>(
        { .damage = 2, .on_hit = [&](rmp::Object &, rmp::Object &) { ++hit; } });
    rmp::Object &player = box(world, { 4, 0 }, { 10, 10 });
    player.add<Health>(
        { .hp = 10, .invulnerable_for = 1, .hurt_by = 4, .damage_on_hit = 5 });
    frame(world);
    CHECK(player.get<Health>()->hp == 8);
    CHECK(hit == 1);
    frame(world); // invulnerable: nothing lands, nothing is said
    CHECK(player.get<Health>()->hp == 8);
    CHECK(hit == 1);
}

TEST_CASE_FIXTURE(Fixture,
                  "Hitbox: knockback pushes away from it; hits picks the layers") {
    World world;
    rmp::Object &sword = box(world, { 0, 0 }, { 10, 10 });
    sword.add<Hitbox>({ .damage = 1, .hits = 2, .knockback = 100 });
    rmp::Object &enemy = box(world, { 6, 0 }, { 10, 10 });
    enemy.collision_layer = 2;
    enemy.add<Health>({ .hp = 3, .invulnerable_for = 0 });
    rmp::Object &ally = box(world, { -6, 0 }, { 10, 10 });
    ally.collision_layer = 1;
    ally.add<Health>({ .hp = 3, .invulnerable_for = 0 });
    frame(world);
    CHECK(enemy.get<Health>()->hp == 2);
    CHECK(enemy.velocity.x > 0);
    CHECK(ally.get<Health>()->hp == 3);
}

TEST_CASE_FIXTURE(Fixture, "Hitbox: once_per_target and destroy_on_hit") {
    World world;
    rmp::Object &swing = box(world, { 0, 0 }, { 10, 10 });
    swing.add<Hitbox>({ .once_per_target = true });
    rmp::Object &enemy = box(world, { 4, 0 }, { 10, 10 });
    enemy.add<Health>({ .hp = 5, .invulnerable_for = 0 });
    frames(world, 4);
    CHECK(enemy.get<Health>()->hp == 4);
    CHECK(swing.get<Hitbox>()->ours.struck.size() == 1);
    enemy.destroy();
    frames(world, 2);
    CHECK(swing.get<Hitbox>()->ours.struck.empty());

    rmp::Object &bullet = box(world, { 100, 0 }, { 4, 4 });
    bullet.add<Hitbox>({ .destroy_on_hit = true });
    rmp::Object &target = box(world, { 102, 0 }, { 10, 10 });
    target.add<Health>({ .hp = 5 });
    const rmp::Handle<> bullet_left = bullet.handle<>();
    frame(world);
    CHECK_FALSE(bullet_left); // gone, and freed: asked through its handle
    // Nothing with a Health: a Hitbox does nothing at all.
    rmp::Object &blade = box(world, { 200, 0 }, { 4, 4 });
    blade.add<Hitbox>({ .destroy_on_hit = true });
    box(world, { 201, 0 }, { 10, 10 });
    frame(world);
    CHECK(blade.alive());
}

// ---------------------------------------------------------------------------
// Collectible
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "Collectible: picked up once, stops colliding, vanishes, goes") {
    World world;
    rmp::Object &coin = box(world, { 0, 0 }, { 8, 8 });
    int collected = 0;
    coin.add<Collectible>(
        { .collected_by = 2,
          .vanish_seconds = 0.5f,
          .on_collect = [&](rmp::Object &, rmp::Object &) { ++collected; } });
    rmp::Object &other = box(world, { 2, 0 }, { 8, 8 });
    other.collision_layer = 1;
    frame(world);
    CHECK(collected == 0);
    rmp::Object &player = box(world, { -2, 0 }, { 8, 8 });
    player.collision_layer = 2;
    player.collision_mask = 3;
    frame(world);
    CHECK(collected == 1);
    CHECK(coin.get<Collectible>()->collected());
    CHECK(coin.collision_layer == 0);
    CHECK(coin.collision_mask == 0);
    frames(world, 10);
    CHECK(collected == 1);
    CHECK(coin.alive());
    CHECK(coin.scale.x > 1);
    CHECK(coin.shape.color.a < 255);
    const rmp::Handle<> coin_left = coin.handle<>();
    frames(world, 30);
    CHECK_FALSE(coin_left);
}

TEST_CASE_FIXTURE(Fixture,
                  "Collectible: a magnet pulls it to the nearest collector in reach") {
    World world;
    rmp::Object &coin = box(world, { 0, 0 }, { 8, 8 });
    coin.add<Collectible>(
        { .collected_by = 2, .magnet = 50, .magnet_speed = 100, .vanish_seconds = 0 });
    rmp::Object &far = box(world, { 200, 0 }, { 8, 8 });
    far.collision_layer = 2;
    frame(world);
    CHECK(coin.velocity.x == 0);
    rmp::Object &near = box(world, { 0, 40 }, { 8, 8 });
    near.collision_layer = 2;
    frame(world);
    CHECK(coin.velocity.y == doctest::Approx(100));
    near.position = { 0, 400 };
    frame(world);
    CHECK(coin.velocity.y == 0);
    near.position = { 0, 30 };
    const rmp::Handle<> coin_left = coin.handle<>();
    frames(world, 30);
    CHECK_FALSE(coin_left); // flew to it, and was picked up at once
}

// ---------------------------------------------------------------------------
// Shooter
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "Shooter: a volley spreads its bullets, and waits `every`") {
    World world;
    rmp::Object &gun = box(world, { 0, 0 }, { 10, 10 });
    gun.collision_layer = 1;
    gun.collision_mask = 1 | 2;
    std::vector<Vector2> shot;
    gun.add<Shooter>(
        { .every = 1,
          .direction = { 1, 0 },
          .speed = 100,
          .count = 3,
          .spread = 90,
          .on_shoot = [&](rmp::Scene &, Vector2, Vector2 v) { shot.push_back(v); } });
    const rmp::Ref<Shooter> s = gun.get<Shooter>();
    REQUIRE(s->fire(gun));
    REQUIRE(shot.size() == 3);
    CHECK(shot[0].x == doctest::Approx(100 * std::cos(std::numbers::pi_v<float> / 4)));
    CHECK(shot[0].y == doctest::Approx(-100 * std::sin(std::numbers::pi_v<float> / 4)));
    CHECK(shot[1].x == doctest::Approx(100));
    CHECK(shot[2].y == doctest::Approx(100 * std::sin(std::numbers::pi_v<float> / 4)));
    CHECK_FALSE(s->fire(gun)); // cooling down
    frames(world, 61);
    CHECK(s->fire(gun));
    CHECK(s->shots() == 2);
}

TEST_CASE_FIXTURE(Fixture,
                  "Shooter: all around, turned after each volley, and automatic") {
    World world;
    rmp::Object &boss = box(world, { 0, 0 }, { 10, 10 });
    std::vector<Vector2> shot;
    boss.add<Shooter>(
        { .automatic = true,
          .every = 0.5f,
          .direction = { 1, 0 },
          .speed = 10,
          .count = 4,
          .spread = 360,
          .rotate_per_shot = 45,
          .on_shoot = [&](rmp::Scene &, Vector2, Vector2 v) { shot.push_back(v); } });
    frame(world);
    REQUIRE(shot.size() == 4);
    CHECK(shot[1].y == doctest::Approx(10).epsilon(0.001));
    frames(world, 30);
    REQUIRE(shot.size() == 8);
    CHECK(shot[4].x == doctest::Approx(10 * std::cos(std::numbers::pi_v<float> / 4)));
    CHECK(boss.get<Shooter>()->shots() == 2);
}

TEST_CASE_FIXTURE(Fixture, "Shooter: the default bullet, aimed at a target, in range") {
    World world;
    rmp::Object &turret = box(world, { 0, 0 }, { 10, 10 });
    turret.collision_layer = 2;
    turret.collision_mask = 1 | 2;
    rmp::Object &player = box(world, { 0, 300 }, { 10, 10 });
    player.collision_layer = 1;
    player.add<Health>({ .hp = 3 });
    turret.add<Shooter>(
        { .automatic = true, .every = 10, .target = player.handle<>(), .range = 100 });
    frame(world);
    CHECK(turret.get<Shooter>()->shots() == 0); // out of range
    player.position = { 0, 80 };
    frame(world);
    REQUIRE(turret.get<Shooter>()->shots() == 1);
    const auto everyone = rmp::objects::detail::live_objects(world);
    const auto found = std::ranges::find_if(
        everyone, [](const auto &o) { return o->template has<Projectile>(); });
    REQUIRE(found != everyone.end());
    const rmp::Object &bullet = **found;
    CHECK(bullet.velocity.y > 0);
    CHECK(bullet.has<Lifespan>());
    CHECK(bullet.has<Hitbox>());
    CHECK(bullet.collision_layer == 0);
    CHECK(bullet.collision_mask == 1); // its mask, less its own layer
    frames(world, 30);
    CHECK(player.get<Health>()->hp == 2);
    player.destroy();
    frames(world, 2);
    turret.get<Shooter>()->ours.cooldown = 0;
    CHECK_FALSE(turret.get<Shooter>()->fire(turret)); // its target is gone
}

TEST_CASE_FIXTURE(Fixture, "Shooter: on its action, and the way it faces") {
    World world;
    rmp::input::action("fire", KEY_X);
    rmp::Object &hero = box(world, { 0, 0 }, { 10, 10 });
    hero.flip_x = true;
    std::vector<Vector2> shot;
    hero.add<Shooter>(
        { .action = "fire",
          .on_shoot = [&](rmp::Scene &, Vector2, Vector2 v) { shot.push_back(v); } });
    frame(world);
    CHECK(shot.empty());
    fake.devices.keys[KEY_X] = true;
    frame(world);
    REQUIRE(shot.size() == 1);
    CHECK(shot[0].x < 0);
}

// ---------------------------------------------------------------------------
// Orbit
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "Orbit: circles its centre, spread among `of`, facing as told") {
    World world;
    rmp::Object &sun = box(world, { 100, 100 }, { 10, 10 });
    rmp::Object &a = box(world, { 0, 0 }, { 4, 4 });
    rmp::Object &b = box(world, { 0, 0 }, { 4, 4 });
    a.add<Orbit>({ .centre = sun.handle<>(),
                   .radius = 50,
                   .degrees_per_second = 90,
                   .face = Face::CENTRE,
                   .index = 0,
                   .of = 2 });
    b.add<Orbit>({ .centre = sun.handle<>(),
                   .radius = 50,
                   .degrees_per_second = 90,
                   .face = Face::OUTWARD,
                   .index = 1,
                   .of = 2 });
    frames(world, 60);
    CHECK(distance(a.position, sun.position) == doctest::Approx(50));
    CHECK(a.position.y == doctest::Approx(150).epsilon(0.01)); // 90 degrees on
    CHECK(b.position.y == doctest::Approx(50).epsilon(0.01));
    CHECK(a.rotation == doctest::Approx(270).epsilon(0.01));
    CHECK(b.rotation == doctest::Approx(270).epsilon(0.01));
    sun.destroy();
    frames(world, 2);
    const Vector2 left = a.position;
    frames(world, 10);
    CHECK(a.position.x == left.x);

    rmp::Object &c = box(world, { 0, 0 }, { 4, 4 });
    c.add<Orbit>({ .point = { 10, 10 }, .radius = 5, .degrees_per_second = 0 });
    frame(world);
    CHECK(c.position.x == doctest::Approx(15));
}

// ---------------------------------------------------------------------------
// Animator
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "Animator: idle, run, flip, hurt and die, skipping what is not there") {
    World world;
    rmp::Object &hero = box(world, { 0, 0 }, { 10, 10 });
    hero.sprite.sheet = sheet_with({ "idle", "run", "die" });
    hero.add<Health>({ .hp = 2, .invulnerable_for = 1, .destroy_on_death = false });
    hero.add<Animator>();
    frame(world);
    CHECK(hero.get<Animator>()->playing() == "idle");
    hero.velocity = { -50, 0 };
    frame(world);
    CHECK(hero.get<Animator>()->playing() == "run");
    CHECK(hero.flip_x);
    hero.get<Health>()->damage(hero, 1); // hurt: no tag, so it runs on
    frame(world);
    CHECK(hero.get<Animator>()->playing() == "run");
    hero.get<Health>()->ours.invulnerable_left = 0;
    hero.get<Health>()->damage(hero, 1);
    frame(world);
    CHECK(hero.get<Animator>()->playing() == "die");
    CHECK_FALSE(hero.sprite.ours.loop);
}

TEST_CASE_FIXTURE(Fixture, "Animator: in the air, jump and fall; its rules come first") {
    World world;
    rmp::Object &hero = box(world, { 0, 0 }, { 10, 10 });
    hero.gravity_scale = 1;
    hero.sprite.sheet = sheet_with({ "idle", "jump", "fall", "dash" });
    bool dashing = false;
    hero.add<Animator>({ .rules = {} });
    hero.get<Animator>()->rules.push_back(
        AnimationRule{ .tag = "dash", .when = [&](rmp::Object &) { return dashing; } });
    hero.velocity = { 0, -300 };
    rmp::objects::detail::late_update_behaviors(hero, STEP);
    CHECK(hero.get<Animator>()->playing() == "jump");
    hero.velocity = { 0, 300 };
    rmp::objects::detail::late_update_behaviors(hero, STEP);
    CHECK(hero.get<Animator>()->playing() == "fall");
    dashing = true;
    rmp::objects::detail::late_update_behaviors(hero, STEP);
    CHECK(hero.get<Animator>()->playing() == "dash");

    rmp::Object &bare = box(world, { 50, 0 }, { 10, 10 });
    bare.add<Animator>();
    bare.velocity = { 50, 0 };
    rmp::objects::detail::late_update_behaviors(bare, STEP);
    CHECK(bare.get<Animator>()->playing().empty());
    CHECK_FALSE(bare.flip_x);
}

// ---------------------------------------------------------------------------
// Draggable
// ---------------------------------------------------------------------------

namespace {

void drag(rmp::Object &object, Vector2 moved) {
    fake.devices.mouse[0] = true;
    rmp::Storage::notify_drag(object, moved);
}

void let_go() { fake.devices.mouse[0] = false; }

} // namespace

TEST_CASE_FIXTURE(Fixture,
                  "Draggable: follows the pointer, and lands on an Area that takes it") {
    World world;
    rmp::Object &slot = box(world, { 100, 0 }, { 40, 40 });
    slot.add<Area>();
    rmp::Object &gem = box(world, { 0, 0 }, { 10, 10 });
    int picks = 0;
    int drops = 0;
    gem.add<Draggable>({ .on_pick = [&](rmp::Object &) { ++picks; },
                         .on_drop = [&](rmp::Object &, rmp::Object &) { ++drops; } });
    drag(gem, { 50, 0 });
    frame(world);
    CHECK(gem.get<Draggable>()->held());
    CHECK(gem.position.x == doctest::Approx(50));
    drag(gem, { 45, 3 });
    frame(world);
    CHECK(gem.position.x == doctest::Approx(95));
    let_go();
    frame(world);
    CHECK_FALSE(gem.get<Draggable>()->held());
    CHECK(picks == 1);
    CHECK(drops == 1);
    CHECK(gem.position.x == doctest::Approx(100));
    CHECK(gem.position.y == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture, "Draggable: rejected, it flies back where it was picked up") {
    World world;
    rmp::Object &slot = box(world, { 100, 0 }, { 40, 40 });
    slot.add<Area>();
    rmp::Object &gem = box(world, { 0, 0 }, { 10, 10 });
    int rejects = 0;
    gem.add<Draggable>({ .accepts = [](rmp::Object &, rmp::Object &) { return false; },
                         .on_reject = [&](rmp::Object &) { ++rejects; } });
    drag(gem, { 100, 0 });
    frame(world);
    let_go();
    frames(world, 30);
    CHECK(rejects == 1);
    CHECK(gem.position.x == doctest::Approx(0));
}

TEST_CASE_FIXTURE(Fixture, "Draggable: snapped, bounded, and thrown") {
    World world;
    rmp::Object &tile = box(world, { 0, 0 }, { 10, 10 });
    tile.add<Draggable>({ .snap = { 32, 32 },
                          .bounds = { -100, -100, 200, 200 },
                          .return_if_rejected = false });
    drag(tile, { 500, 37 });
    frame(world);
    CHECK(tile.position.x == doctest::Approx(100)); // the bounds
    let_go();
    frame(world);
    CHECK(tile.position.x == doctest::Approx(96));
    CHECK(tile.position.y == doctest::Approx(32));

    rmp::Object &puck = box(world, { 0, 200 }, { 10, 10 });
    puck.add<Draggable>({ .throwable = true, .return_if_rejected = false });
    for (int i = 0; i < 4; ++i) {
        drag(puck, { 5, 0 });
        frame(world);
    }
    let_go();
    frame(world);
    CHECK(puck.velocity.x > 100);
    frames(world, 300);
    CHECK(puck.velocity.x == 0);
}

// ---------------------------------------------------------------------------
// Patrol
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "Patrol: waypoints in a loop, waiting at each") {
    World world;
    rmp::Object &bat = box(world, { 0, 0 }, { 8, 8 });
    bat.add<Patrol>(
        { .waypoints = { { 0, 0 }, { 60, 0 }, { 60, 60 } }, .speed = 60, .wait = 0.5f });
    frame(world); // at the first: waits
    CHECK(bat.get<Patrol>()->heading_for() == 1);
    frames(world, 30);
    frames(world, 62);
    CHECK(bat.position.x == doctest::Approx(60));
    CHECK(bat.get<Patrol>()->heading_for() == 2);
    frames(world, 200);
    CHECK(bat.get<Patrol>()->heading_for() == 0); // round to the first
}

TEST_CASE_FIXTURE(Fixture, "Patrol: ping_pong turns back at each end, and flips") {
    World world;
    rmp::Object &bat = box(world, { 0, 0 }, { 8, 8 });
    bat.sprite.sheet = sheet_with({ "walk", "idle" });
    bat.add<Patrol>({ .waypoints = { { 0, 0 }, { 30, 0 }, { 60, 0 } },
                      .speed = 120,
                      .ping_pong = true });
    std::vector<int> heading;
    for (int i = 0; i < 120; ++i) {
        frame(world);
        const int h = bat.get<Patrol>()->heading_for();
        if (heading.empty() || heading.back() != h) heading.push_back(h);
    }
    REQUIRE(heading.size() >= 5);
    CHECK(heading[0] == 1);
    CHECK(heading[1] == 2);
    CHECK(heading[2] == 1);
    CHECK(heading[3] == 0);
    CHECK(heading[4] == 1);
    CHECK(bat.sprite.playing() == "walk");
}

TEST_CASE_FIXTURE(Fixture,
                  "Patrol: walking the floor turns at a wall and before a ledge") {
    World world;
    give_map(world.map,
             {
                 "..........",
                 "..........",
                 "#........#",
                 "######....",
             });
    rmp::Object &goomba = box(world, cell(2, 2), { 14, 14 });
    goomba.add<Patrol>({ .speed = 60 });
    float right_most = 0;
    float left_most = 1000;
    for (int i = 0; i < 400; ++i) {
        frame(world);
        right_most = std::max(right_most, goomba.position.x);
        left_most = std::min(left_most, goomba.position.x);
    }
    CHECK(right_most < 6 * 16 + 1); // the ledge, at x = 96
    CHECK(right_most > 4 * 16);
    CHECK(left_most > 16 - 1); // the wall, ending at x = 16
    CHECK(left_most < 2 * 16);

    rmp::Object &fearless = box(world, cell(3, 2), { 14, 14 });
    fearless.add<Patrol>({ .speed = 60, .turn_at_ledges = false });
    float furthest = 0;
    for (int i = 0; i < 120; ++i) {
        frame(world);
        furthest = std::max(furthest, fearless.position.x);
    }
    CHECK(furthest > 7 * 16); // past the ledge, to the wall
    CHECK(furthest < 9 * 16);
}

// ---------------------------------------------------------------------------
// find_path and Pathfind
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "find_path: around a wall, never through a corner") {
    World world;
    give_map(world.map,
             {
                 "......",
                 ".####.",
                 "......",
             });
    const auto path = rmp::find_path(world.map, cell(0, 0), cell(5, 2));
    REQUIRE_FALSE(path.empty());
    CHECK(path.back().x == doctest::Approx(cell(5, 2).x));
    Vector2 was = cell(0, 0);
    for (const Vector2 p : path) {
        CHECK_FALSE(world.map.solid_at(p));
        // No step crosses a solid cell's corner: the midpoint of each step
        // and the two cells beside a diagonal are open.
        CHECK_FALSE(world.map.solid_at(Vector2{ (was.x + p.x) / 2, (was.y + p.y) / 2 }));
        if (p.x != was.x && p.y != was.y) {
            CHECK_FALSE(world.map.solid_at(Vector2{ p.x, was.y }));
            CHECK_FALSE(world.map.solid_at(Vector2{ was.x, p.y }));
        }
        was = p;
    }
    // Four directions: the steps are all straight, and there are more.
    const auto straight =
        rmp::find_path(world.map, cell(0, 0), cell(5, 2), { .diagonal = false });
    REQUIRE_FALSE(straight.empty());
    CHECK(straight.size() >= path.size());
    was = cell(0, 0);
    for (const Vector2 p : straight) {
        CHECK((p.x == was.x || p.y == was.y));
        was = p;
    }
}

TEST_CASE_FIXTURE(Fixture, "find_path: the ends, no way, and clearance") {
    World world;
    give_map(world.map,
             {
                 "..#...",
                 "..#...",
                 "..#...",
             });
    CHECK(rmp::find_path(world.map, cell(0, 0), cell(5, 0)).empty()); // no way
    CHECK(rmp::find_path(world.map, cell(0, 0), cell(2, 0)).empty()); // into a wall
    CHECK(rmp::find_path(world.map, { -50, 0 }, cell(1, 1)).empty()); // outside
    CHECK(rmp::find_path(world.map, cell(0, 0), { NAN, 0 }).empty());
    const auto here = rmp::find_path(world.map, cell(0, 0), { 3, 4 });
    REQUIRE(here.size() == 1);
    CHECK(here[0].x == 3);
    const rmp::Tilemap none;
    CHECK(rmp::find_path(none, { 0, 0 }, { 1, 1 }).empty());

    World corridor;
    give_map(corridor.map,
             {
                 "#########",
                 "#.......#",
                 "####.####",
                 "#.......#",
                 "#########",
             });
    CHECK_FALSE(rmp::find_path(corridor.map, cell(1, 1), cell(1, 3)).empty());
    // Wider than the gap: no way through it.
    CHECK(rmp::find_path(corridor.map, cell(1, 1), cell(1, 3), { .clearance = 10 })
              .empty());
}

TEST_CASE_FIXTURE(Fixture,
                  "Pathfind: walks round the wall to its goal, and to a target") {
    World world;
    give_map(world.map,
             {
                 "........",
                 ".######.",
                 "........",
             });
    rmp::Object &rat = box(world, cell(0, 2), { 8, 8 });
    rat.add<Pathfind>({ .goal = cell(7, 0), .speed = 120 });
    for (int i = 0; i < 240 && !rat.get<Pathfind>()->arrived(); ++i) {
        frame(world);
        CHECK_FALSE(world.map.solid_at(rat.position));
    }
    CHECK(rat.get<Pathfind>()->arrived());
    CHECK(distance(rat.position, cell(7, 0)) < 3);
    CHECK(rat.get<Pathfind>()->path().empty());

    rmp::Object &cheese = box(world, cell(0, 0), { 4, 4 });
    cheese.collision_layer = 0;
    cheese.collision_mask = 0;
    rat.get<Pathfind>()->target = cheese.handle<>();
    frame(world);
    CHECK_FALSE(rat.get<Pathfind>()->path().empty());
    for (int i = 0; i < 240 && !rat.get<Pathfind>()->arrived(); ++i) frame(world);
    CHECK(distance(rat.position, cheese.position) < 3);
    cheese.destroy();
    frame(world);
    rat.velocity = { 7, 0 };
    frame(world);
    CHECK(rat.velocity.x == 7); // nowhere to go: it leaves the velocity alone
}

// ---------------------------------------------------------------------------
// Chase
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "Chase: spots in sight, pauses the Patrol, gives up and goes home") {
    World world;
    give_map(world.map,
             {
                 "....................",
                 "....................",
                 "....................",
             });
    rmp::Object &player = box(world, cell(18, 1), { 8, 8 });
    rmp::Object &guard = box(world, cell(2, 1), { 8, 8 });
    int spots = 0;
    int loses = 0;
    guard.add<Patrol>({ .waypoints = { cell(2, 1), cell(3, 1) }, .speed = 10 });
    guard.add<Chase>({ .target = player.handle<>(),
                       .speed = 60,
                       .sight = 100,
                       .lose_after = 0.5f,
                       .on_spot = [&](rmp::Object &, rmp::Object &) { ++spots; },
                       .on_lose = [&](rmp::Object &) { ++loses; } });
    frames(world, 5);
    CHECK(spots == 0);
    CHECK(guard.enabled<Patrol>());
    player.position = cell(7, 1);
    frame(world);
    CHECK(spots == 1);
    CHECK(guard.get<Chase>()->chasing());
    CHECK_FALSE(guard.enabled<Patrol>());
    frames(world, 10);
    CHECK(guard.velocity.x > 0);
    player.position = cell(19, 1); // out of sight
    player.position.y += 100;
    frames(world, 40);
    CHECK(loses == 1);
    CHECK_FALSE(guard.get<Chase>()->chasing());
    frames(world, 200);
    CHECK(guard.enabled<Patrol>()); // home, and patrolling again
}

TEST_CASE_FIXTURE(Fixture, "Chase: a wall hides the target, and so does its back") {
    World world;
    give_map(world.map,
             {
                 "..........",
                 ".....#....",
                 "..........",
             });
    rmp::Object &player = box(world, cell(8, 1), { 8, 8 });
    rmp::Object &guard = box(world, cell(2, 1), { 8, 8 });
    guard.add<Chase>({ .target = player.handle<>(), .sight = 0 });
    frame(world);
    CHECK_FALSE(guard.get<Chase>()->sees());
    guard.get<Chase>()->line_of_sight = false;
    frame(world);
    CHECK(guard.get<Chase>()->sees());

    rmp::Object &watcher = box(world, cell(2, 0), { 8, 8 });
    watcher.flip_x = true; // looks left
    watcher.add<Chase>({ .target = player.handle<>(), .sight = 0, .fov = 90 });
    player.position = cell(8, 0);
    frame(world);
    CHECK_FALSE(watcher.get<Chase>()->sees());
    player.position = cell(0, 0);
    frame(world);
    CHECK(watcher.get<Chase>()->sees());
}

TEST_CASE_FIXTURE(Fixture, "Chase: steers a Pathfind round the wall when it has one") {
    World world;
    give_map(world.map,
             {
                 "..........",
                 "..#######.",
                 "..........",
             });
    const rmp::Object &player = box(world, cell(9, 2), { 8, 8 });
    rmp::Object &hound = box(world, cell(9, 0), { 8, 8 });
    hound.add<Pathfind>();
    hound.add<Chase>({ .target = player.handle<>(),
                       .speed = 90,
                       .sight = 0,
                       .line_of_sight = false,
                       .stop_distance = 4 });
    for (int i = 0; i < 120; ++i) {
        frame(world);
        CHECK_FALSE(world.map.solid_at(hound.position));
    }
    CHECK(distance(hound.position, player.position) < 10);
}

// ---------------------------------------------------------------------------
// StateMachine
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "StateMachine: initial, transitions, go(), and what each enables") {
    World world;
    rmp::Object &enemy = box(world, { 0, 0 }, { 8, 8 });
    enemy.add<Counter>();
    enemy.add<Other>();
    bool alarmed = false;
    std::vector<std::string> said;
    enemy.add<StateMachine>({
        .states = {},
        .initial = "idle",
    });
    auto machine = enemy.get<StateMachine>();
    machine->states.push_back(State{ .name = "patrol", .enables = kinds<Counter>() });
    machine->states.push_back(
        State{ .name = "idle",
               .on_enter = [&](rmp::Object &) { said.emplace_back("enter idle"); },
               .on_exit = [&](rmp::Object &) { said.emplace_back("exit idle"); } });
    machine->states.push_back(
        State{ .name = "chase",
               .on_enter = [&](rmp::Object &) { said.emplace_back("enter chase"); },
               .on_update = [&](rmp::Object &, float) { said.emplace_back("chasing"); },
               .enables = kinds<Other>() });
    machine->transitions.push_back(Transition{
        .from = "idle", .to = "chase", .when = [&](rmp::Object &) { return alarmed; } });
    machine->transitions.push_back(Transition{
        .to = "patrol", .when = [&](rmp::Object &self) {
            return self.get<StateMachine>()->seconds_in_state() > 0.5f && !alarmed;
        } });
    CHECK(machine->state().empty());
    frame(world);
    CHECK(machine->state() == "idle");
    CHECK_FALSE(enemy.enabled<Counter>());
    CHECK_FALSE(enemy.enabled<Other>());
    alarmed = true;
    frame(world);
    CHECK(machine->state() == "chase");
    CHECK(enemy.enabled<Other>());
    CHECK_FALSE(enemy.enabled<Counter>());
    CHECK(
        said ==
        std::vector<std::string>{ "enter idle", "exit idle", "enter chase", "chasing" });
    alarmed = false;
    frames(world, 40);
    CHECK(machine->state() == "patrol");
    CHECK(enemy.enabled<Counter>());
    CHECK_FALSE(enemy.enabled<Other>());
    machine->go("idle");
    frame(world);
    CHECK(machine->state() == "idle");
    CHECK(machine->seconds_in_state() == doctest::Approx(STEP));
    machine->go("nowhere");
    frame(world);
    CHECK(machine->state() == "idle");
}

TEST_CASE_FIXTURE(
    Fixture, "StateMachine: no states does nothing; an unknown initial is the first") {
    World world;
    rmp::Object &a = box(world, { 0, 0 }, { 8, 8 });
    a.add<StateMachine>();
    frame(world);
    CHECK(a.get<StateMachine>()->state().empty());
    rmp::Object &b = box(world, { 50, 0 }, { 8, 8 });
    b.add<StateMachine>({ .initial = "missing" });
    b.get<StateMachine>()->states.push_back(State{ .name = "first" });
    frame(world);
    CHECK(b.get<StateMachine>()->state() == "first");
}

// ---------------------------------------------------------------------------
// From the editor
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "B::from(MapObject) reads the documented fields, and only those") {
    std::ifstream in(std::string(RMP_TEST_FIXTURES) + "ldtk/behaviors.ldtk",
                     std::ios::binary);
    const std::string text{ std::istreambuf_iterator<char>(in),
                            std::istreambuf_iterator<char>() };
    REQUIRE_FALSE(text.empty());
    World world;
    rmp::tilemap::detail::Access::adopt(
        world.map,
        rmp::tilemap::detail::parse_map(text.data(), static_cast<int>(text.size()),
                                        "behaviors.ldtk", ""));
    REQUIRE(world.map.valid());
    int seen = 0;
    world.map.on_object("Bat", [&](rmp::Scene &, const rmp::MapObject &o) {
        const Patrol p = Patrol::from(o);
        REQUIRE(p.waypoints.size() == 3);
        CHECK(p.waypoints[0].x == o.position.x);
        CHECK(p.waypoints[1].x - p.waypoints[0].x == doctest::Approx(3 * 16));
        CHECK(p.speed == 40);
        CHECK(p.wait == 0.5f);
        CHECK(p.ping_pong);
        ++seen;
    });
    world.map.on_object("Guard", [&](rmp::Scene &, const rmp::MapObject &o) {
        const Chase c = Chase::from(o);
        CHECK(c.speed == 90);
        CHECK(c.sight == 200);
        CHECK(c.fov == 120);
        ++seen;
    });
    world.map.on_object("Ring", [&](rmp::Scene &, const rmp::MapObject &o) {
        const Orbit r = Orbit::from(o);
        CHECK(r.radius == 30);
        CHECK(r.degrees_per_second == 45);
        CHECK(r.angle == 90);
        ++seen;
    });
    world.map.on_object("Turret", [&](rmp::Scene &, const rmp::MapObject &o) {
        const Shooter s = Shooter::from(o);
        CHECK(s.every == 1.5f);
        CHECK(s.speed == 200);
        CHECK(s.count == 3);
        CHECK(s.spread == 30);
        CHECK(s.rotate_per_shot == 10);
        CHECK(s.range == 150);
        ++seen;
    });
    world.map.on_object("Spike", [&](rmp::Scene &, const rmp::MapObject &o) {
        const Hitbox h = Hitbox::from(o);
        CHECK(h.damage == 2);
        CHECK(h.knockback == 300);
        ++seen;
    });
    world.map.on_object("Coin", [&](rmp::Scene &, const rmp::MapObject &o) {
        CHECK(Collectible::from(o).magnet == 40);
        ++seen;
    });
    world.map.on_object("Plain", [&](rmp::Scene &, const rmp::MapObject &o) {
        // No fields: every default, and a Patrol with no path walks the floor.
        CHECK(Patrol::from(o).waypoints.empty());
        CHECK(Patrol::from(o).speed == Patrol{}.speed);
        CHECK(Chase::from(o).sight == Chase{}.sight);
        CHECK(Orbit::from(o).radius == Orbit{}.radius);
        CHECK(Shooter::from(o).count == Shooter{}.count);
        CHECK(Hitbox::from(o).damage == Hitbox{}.damage);
        CHECK(Collectible::from(o).magnet == Collectible{}.magnet);
        ++seen;
    });
    world.map.spawn_objects(world);
    CHECK(seen == 7);
}

// ---------------------------------------------------------------------------
// Projectile at a wall
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture, "Projectile: destroy_on_wall ends it in a solid cell") {
    World world;
    give_map(world.map,
             {
                 "....#",
             });
    rmp::Object &shot = box(world, cell(0, 0), { 2, 2 });
    shot.add<Projectile>({ .destroy_on_wall = true });
    shot.velocity = { 120, 0 };
    rmp::Object &ghost = box(world, cell(0, 0), { 2, 2 });
    ghost.collision_layer = 0;
    ghost.collision_mask = 0;
    ghost.add<Projectile>();
    ghost.velocity = { 120, 0 };
    const rmp::Handle<> shot_left = shot.handle<>();
    frames(world, 30);
    CHECK_FALSE(shot_left);
    CHECK(ghost.alive());
}

// ---------------------------------------------------------------------------
// What the property found
// ---------------------------------------------------------------------------

TEST_CASE_FIXTURE(Fixture,
                  "found: a snap that is not a number leaves the position alone") {
    World world;
    rmp::Object &tile = box(world, { 10, 20 }, { 10, 10 });
    tile.add<Draggable>({ .snap = { std::numeric_limits<float>::infinity(), 1e-42f },
                          .return_if_rejected = false });
    drag(tile, { 3, 3 });
    frame(world);
    let_go();
    frame(world);
    CHECK(std::isfinite(tile.position.x));
    CHECK(std::isfinite(tile.position.y));
}

TEST_CASE_FIXTURE(Fixture, "found: a vanish of a denormal's length is no infinite rise") {
    World world;
    rmp::Object &coin = box(world, { 0, 0 }, { 8, 8 });
    coin.add<Collectible>({ .vanish_seconds = 1e-42f });
    box(world, { 2, 0 }, { 8, 8 });
    frame(world);
    CHECK(std::isfinite(coin.velocity.y));
}

TEST_CASE_FIXTURE(Fixture,
                  "found: a clearance wider than any map is searched in bounded time") {
    World world;
    give_map(world.map, { "....", ".#..", "...." });
    // Every cell is too near the wall but the two ends: no way between them.
    CHECK(rmp::find_path(world.map, cell(0, 0), cell(3, 2),
                         { .clearance = std::numeric_limits<float>::max() })
              .empty());
}
