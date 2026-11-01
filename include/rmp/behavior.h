#pragma once
// ---------------------------------------------------------------------------
// rmp/behavior.h — the catalogue.
//
//     ball.add<rmp::behavior::Ball>({ .speed = 320 });
//     player.add<rmp::behavior::TopDown>({ .speed = 220 });
//
// A behavior is any struct with `void _update(rmp::Object &, float)`. The model
// is explained above Object::add in rmp/object.h, and the part worth repeating
// here is that every one of these is a struct exactly like one of yours: there
// is no extension API, because there is nothing to extend.
//
// THE FIELDS ARE THE CONFIGURATION, and they are hot:
//
//     player.get<rmp::behavior::TopDown>()->speed = 400;   // a power-up
//
// NONE OF THEM CHOOSES A DIRECTION FOR YOU. A behavior contributes behaviour,
// not intention: which way the Pong ball is served is a rule of the game, and
// picking it would be the framework deciding something that is not its to
// decide. So a Ball starts still, a Projectile travels the way you pushed it,
// a Follow chases the handle you gave it, and a TopDown reads the actions you
// named.
//
// WHAT THEY WRITE IS A NUMBER. A behavior writes into its object -- the
// position, the velocity, the scale -- only finite numbers: a field that is
// NaN or an infinity, or one so large the arithmetic overflows, leaves the
// object as it was, and the behavior says so once (a field that is NaN is
// named). A hook handed a delta that is not a number of seconds, 0 or more,
// counts it as 0 and says so once; the framework never hands one.
//
// PAUSED, NOT REMOVED. `object.enable<B>(false)` stops a behavior's hooks and
// keeps its state; a Chase pauses the Patrol it interrupts that way, and a
// StateMachine's states say which behaviors run while they are on.
//
// FROM THE EDITOR. The behaviors a level designer places -- Patrol, Chase,
// Orbit, Shooter, Hitbox, Collectible -- have `B::from(const MapObject &)`,
// which reads the entity's fields by the names written above each of them,
// and leaves every field the entity does not have at its default:
//
//     map.on_object("Bat", [](rmp::Scene &s, const rmp::MapObject &o) {
//         auto &bat = s.spawn({ .position = o.position });
//         bat.add(rmp::behavior::Patrol::from(o));
//     });
//
// WHAT IS NOT HERE, and why, in short: `tetromino`, `formation` and `paddle`
// are three games with a different name on them.
// ---------------------------------------------------------------------------

// <rmp/input.h> is NOT here, and that is deliberate: nothing this header
// declares names a type or a function from rmp::input -- the catalogue reads
// actions in src/rmp/behaviors.cpp, which includes it there. Here it would cost
// 52 ms of every translation unit with a behavior in it, which is every
// gameplay file, for a header none of them need. tools/header_check.sh compiles
// this one on its own, which is what says it is not needed.
#include <raylib.h>
#include <rmp/config.h>
#include <rmp/object.h> // rmp::Object, Handle and Callback, held by value in the catalogue
#include <rmp/scene.h> // rmp::Scene, for Spawner's on_spawn -- and what every behavior game uses

#include <array>
#include <optional> // a Pathfind's goal, which may be none
#include <span> // what overlapping() and path() hand back, without a copy
#include <string> // the action and tag names a behavior is given, owned: a
// std::string's c_str() or a temporary passed in cannot dangle
#include <vector> // waypoints, states, rules and paths: lists the game writes

namespace rmp {

// How rmp::find_path() looks for a way through a map.
struct PathOptions {
    bool diagonal = true; // eight directions; false = four. A corner is never cut
    float clearance =
        0; // keeps this many units from every solid cell: half the walker's width
};

// The way from `from` to `to` across the map's cells, around its solid ones:
// the centres of the cells to walk through, ending on `to` itself. A* on the
// grid, with diagonal steps that never cut the corner of a solid cell. Empty
// when there is no way, when either end is outside the map or solid, and for a
// map with no cells. `from` itself is not on it.
[[nodiscard]] std::vector<Vector2> find_path(const Tilemap &map, Vector2 from, Vector2 to,
                                             const PathOptions &options = {});

} // namespace rmp

namespace rmp::behavior {

// ---------------------------------------------------------------------------
// Character movement — pick one
// ---------------------------------------------------------------------------

// Eight directions, and the sprite chosen from the one you are facing.
//
// The eight-way movement is free and correct because rmp::input::vector()
// normalises: going diagonally is NOT 41 % faster, which is the bug in half the
// raylib tutorials on the internet.
struct TopDown {
    float speed = 220; // design units per second
    bool eight_way = true; // false = four directions only
    float acceleration = 0; // 0 = instant, which is what nearly all 2D wants

    // THE ANIMATION NAMES ARE YOURS: they are the tags in your .aseprite, and
    // "idle" and "walk" are only the defaults, not a convention imposed on you.
    // The tag actually chosen is "<walk>_<suffix>" if the sheet has it and
    // "<walk>" with flip_x if it does not, so a two-, four- or eight-direction
    // sheet all work with nothing configured. `idle` is played as it is, with no
    // suffix. With no sheet on the object's sprite only flip_x is set, and an
    // empty name plays nothing.
    //
    // THE ART FACES RIGHT. flip_x is set while facing left, to mirror the
    // frames that have no direction of their own -- `walk` without a suffix,
    // `idle`, or a sprite with no sheet -- so those must be drawn facing east.
    // A tag with a suffix is drawn for its direction and plays unflipped.
    std::string idle = "idle";
    std::string walk = "walk";
    std::array<std::string, 8> suffixes = { "e", "ne", "n", "nw", "w", "sw", "s", "se" };

    // Which actions move it. Empty = the move_* actions that come as standard.
    std::string left;
    std::string right;
    std::string up;
    std::string down;

    // The last non-zero direction, normalised. What a game reads to fire a
    // weapon the way the character is looking.
    [[nodiscard]] Vector2 direction() const { return ours.facing; }

    // Which of the eight sectors `ours.facing` is in, as an index into `suffixes`.
    [[nodiscard]] int sector() const;

    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        Vector2 facing{ 1, 0 };
    } ours;
};

// Gravity, jumping, ground, coyote time and a buffered jump. The last two are
// what separates a platformer that feels right from one that does not, and
// neither is something a player can name -- they only notice their absence.
struct Platformer {
    float speed = 260; // units per second while a direction is held
    float acceleration = 0; // 0 = instant
    float gravity = 2000; // units per second squared, pulling down while off the ground
    float jump = 700; // the upward speed a jump starts with, in units per second
    int air_jumps = 0; // 1 = a double jump

    // Still jumpable for this long after walking off a ledge. Named after the
    // coyote who keeps running for a moment past the cliff.
    float coyote_time = 0.1f;
    // A jump pressed this long before landing still fires on landing.
    float jump_buffer = 0.12f;

    // Which actions move and jump it. Empty = move_left, move_right and
    // ui_accept, the actions that come as standard.
    std::string left;
    std::string right;
    std::string jump_action;

    // Whether it was standing on something at its last update: a solid object
    // or a solid map cell just under its feet, while not moving up.
    [[nodiscard]] bool grounded() const { return ours.grounded; }

    void _ready(Object &self);
    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        bool grounded = false;
        float since_grounded = 1000;
        float since_jump_pressed = 1000;
        int jumps_used = 0;
    } ours;
};

// Constant forward motion that SPEEDS UP over time, with a jump and a duck.
//
// A Platformer with the x fixed is not this, and the difference is `accelerate`
// and `distance()`: in an endless runner the speed climbs on its own, and the
// distance travelled is both the score and the clock the difficulty hangs off
// -- and a Spawner with `every_distance` and `track` set to the runner
// produces by that same distance rather than by time.
struct Runner {
    float speed = 320; // units per second, right now
    float accelerate = 6; // how much `speed` gains per second. 0 = constant
    float max_speed = 900; // the most `speed` is allowed to reach, units per second
    float gravity = 2200; // units per second squared, pulling down while off the ground
    float jump = 720; // the upward speed a jump starts with, in units per second
    int air_jumps = 0; // 1 = a double jump

    std::string jump_action; // empty = ui_accept
    std::string duck_action; // held to duck; empty = it never ducks

    // How far it has run, in units: `speed` summed over every update since it
    // was added. It counts the running, not where the object got to.
    [[nodiscard]] float distance() const { return ours.distance; }
    // Whether `duck_action` was held at its last update. That is all ducking
    // does here: a smaller collider or another animation is the game's to set.
    [[nodiscard]] bool ducking() const { return ours.ducking; }

    void _ready(Object &self);
    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        float distance = 0;
        bool ducking = false;
        bool grounded = false;
        int jumps_used = 0;
    } ours;
};

// ---------------------------------------------------------------------------
// Object movement
// ---------------------------------------------------------------------------

// Bounces, keeps its pace, and leaves a paddle at an angle that depends on
// where it hit. STARTS STILL -- see the note at the top of this file.
//
//     ball.add<rmp::behavior::Ball>({ .speed = 320 });
//     ball.apply_force({ 1, 0.35f });   // the serve, and it is the game's
//
// _update normalises the velocity to `speed` ONLY when it is not zero, and the
// two properties that fall out of that are what make the serve one line: only
// the DIRECTION of the push counts, so nobody has to think about magnitudes,
// and `velocity = {}` is a stable state, so stopping the ball between points is
// one assignment rather than removing the behavior.
struct Ball {
    float speed = 300; // the pace it KEEPS, not the one it leaves at
    float max_bounce_deg = 60; // how far a hit near the end of a paddle tilts it
    float speed_up = 1.02f; // per bounce off a solid. 1 = constant

    void _ready(Object &self);
    void _collision(Object &self, Object &other);
    void _update(Object &self, float delta);
};

// Travels the way you pushed it and is discarded when it leaves or hits.
struct Projectile {
    float speed = 0; // 0 = keep whatever velocity it was given
    bool destroy_on_hit = true; // discarded on touching any object it collides with
    bool destroy_on_wall = false; // discarded when its centre is in a solid map cell

    void _ready(Object &self);
    void _collision(Object &self, Object &other);
    void _update(Object &self, float delta);
};

// Chases a handle, with a speed and a distance it stops at. A handle and not a
// pointer, because the thing being chased is exactly the thing most likely to
// die while being chased.
struct Follow {
    Handle<Object> target; // what it chases; empty or gone = it keeps its velocity
    float speed = 150; // units per second while chasing
    float stop_distance = 0; // within this many units of the target's centre it stops
    float acceleration = 0; // 0 = instant

    void _update(Object &self, float delta);
};

// ---------------------------------------------------------------------------
// Tween: animate one property, with easing, looping and ping-pong.
// ---------------------------------------------------------------------------

// The curve a Tween's value follows from `from` to `to` over its `seconds`.
// LINEAR moves at a constant rate; IN starts slow and speeds up; OUT starts
// fast and slows into `to`; IN_OUT is slow at both ends and fastest in the
// middle. The last three are quadratic, and a ping_pong's way back runs the
// same curve in reverse.
enum class Ease { LINEAR, IN, OUT, IN_OUT };

// Which field of its object a Tween writes, on every update it runs. ALPHA
// takes 0 to 255 and clamps to that range.
enum class Property {
    POSITION_X, // position.x, the centre
    POSITION_Y, // position.y, the centre
    SCALE_X, // scale.x
    SCALE_Y, // scale.y
    ROTATION, // rotation, in degrees
    ALPHA, // the shape's colour, or the sprite's tint
};

// Moves one property of its object from `from` to `to` over `seconds`, along
// an Ease. With neither `loop` nor `ping_pong` it stops exactly on `to` and
// finished() turns true.
struct Tween {
    Property property = Property::POSITION_Y; // the field it writes
    float from = 0; // the value at the start of a pass, in that field's unit
    float to = 0; // the value at the end of a pass
    float seconds = 1; // how long one pass takes; 0 = straight to `to`
    Ease ease = Ease::IN_OUT; // the curve of each pass
    bool loop = false; // start again from `from` at the end of each pass
    bool ping_pong = false; // turn back at each end, forever; wins over `loop`

    // Whether it has stopped on `to`: a tween with neither `loop` nor
    // `ping_pong` does at the end of its pass, and one of 0 seconds at once,
    // whatever those two say.
    [[nodiscard]] bool finished() const { return ours.done; }
    // Starts it again from `from`, going forwards, as if it had just been added.
    void restart() {
        ours.elapsed = 0;
        ours.done = false;
        ours.back = false;
    }

    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        float elapsed = 0;
        bool done = false;
        bool back = false;
    } ours;
};

// Squares the position with a grid, AFTER everything that moves it. That is
// what keeps a puzzle piece from sitting on half a pixel when it rotates or
// falls, and it is why this declares _late_update and not _update.
struct GridSnap {
    Vector2 cell{ 32, 32 }; // the grid's spacing per axis; 0 = that axis is left alone
    Vector2 offset{}; // where the grid's origin is

    void _late_update(Object &self, float delta);
};

// ---------------------------------------------------------------------------
// World and production
// ---------------------------------------------------------------------------

// A background layer that scrolls at its own factor and repeats itself with no
// seam. Twenty lines everybody writes wrong the first time: the texture has to
// be drawn TWICE with the offset taken modulo its width, and it has to be
// recomputed when the window changes size.
struct Parallax {
    std::string texture; // a name for rmp::assets::load_texture
    float factor = 0.5f; // 1 = scrolls with the world, 0 = pinned to the screen
    float speed = 0; // units per second of its own, for a sky that drifts
    float y = 0; // where the top of the strip sits
    Color tint = WHITE; // multiplies the texture's colours; WHITE = drawn as it is

    void _ready(Object &self);
    void _draw(Object &self);
    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        rmp::Texture art;
        float scroll = 0;
    } ours;
};

// How many of its own objects one Spawner keeps track of, which is the largest
// `max_alive` it can enforce. A fixed array and not a std::vector because
// <vector> costs 92 ms in every translation unit that includes a behavior, and
// a spawner that needs hundreds of things alive at once is a pool, not a
// spawner.
// A `max_alive` above this warns once and is treated as this.
inline constexpr int MAX_SPAWNED = 64;

// Produces objects every N seconds, or every N units TRAVELLED.
//
// By distance is not the same as a Timer, and the difference is the whole
// reason this is not one: in an endless runner the speed climbs, so "every two
// seconds" puts the obstacles further and further apart and the game gets
// EASIER the faster you go, which is the opposite of the intention.
struct Spawner {
    float every_seconds = 0; // one of these two, not both
    float every_distance = 0; // units travelled between spawns; wins if both are set
    float jitter = 0; // +/- this much, uniformly
    int max_alive = 0; // 0 = no limit, and it is a cap on the LIVE ones

    // Where "travelled" is measured from. Empty = this object's own position.
    Handle<Object> track;

    // Makes what is spawned, given the scene and where `track` (or this object)
    // is now. Every object it adds counts towards `max_alive`. Empty = the
    // Spawner does nothing.
    Callback<Scene &, Vector2> on_spawn;

    // How many of the objects it made are still alive RIGHT NOW. It goes down
    // when they die, which is the whole difference between a cap and a quota.
    [[nodiscard]] int alive() const;

    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        float countdown = -1;
        float next_at = -1;
        float travelled = 0;
        Vector2 last_at{};
        bool started = false;
        // HANDLES AND NOT A COUNTER. A counter only ever goes up: nothing tells
        // a spawner that what it made has died, so with a counter `max_alive`
        // would cap how many it ever produced, and an endless runner would stop
        // making obstacles thirty seconds in. A handle answers on its own.
        std::array<Handle<Object>, MAX_SPAWNED> made;
        int made_count = 0;
    } ours;

    // A fresh random amount between -jitter and +jitter from rmp::random::range,
    // or 0 when `jitter` is 0 or less or not a finite number: what each
    // interval after the first is moved by.
    [[nodiscard]] float jitter_amount() const;
};

// Calls you every N seconds. Shooting, blinking, a countdown.
struct Timer {
    float seconds = 1; // between calls; 0 = it never fires
    bool repeat = true; // false = one call, and then it is spent
    // Called with the object the Timer is on. A frame longer than `seconds`
    // calls it once for every interval that frame covered, so a countdown
    // keeps time on a slow machine -- up to a thousand: more than that owed in
    // one frame is an interval far shorter than any frame, and the rest of the
    // time is dropped and said once.
    Callback<Object &> on_timeout;

    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        float elapsed = 0;
        bool spent = false;
    } ours;
};

// Discarded after N seconds. Particles, bullets.
struct Lifespan {
    float seconds = 1; // how long the object lasts before it is destroyed

    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        float elapsed = 0;
    } ours;
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

// Hit points, a window of invulnerability with a blink, and death.
struct Health {
    int hp = 3; // hit points left, and the number it starts with
    int max_hp = 0; // 0 = whatever hp starts at, so `{ .hp = 5 }` needs no second number
    float invulnerable_for = 0.6f; // 0 = no window
    float blink_hz = 12; // blinks per second while invulnerable; 0 = no blink
    bool destroy_on_death = true; // destroy the object when it dies, after on_death

    Callback<Object &> on_death; // once, when damage() takes hp to 0
    Callback<Object &, int> on_damage; // the amount that actually landed

    // WHICH LAYERS HURT. 0, the default, is "nothing does": contact costs
    // nothing until a game says which contact costs something, and then it says
    // it once here instead of writing the same `if` at the top of every
    // _collision.
    //
    //     player.add<rmp::behavior::Health>({ .hp = 3, .hurt_by = layer::ENEMY });
    //
    // It is the OTHER object's `collision_layer` that is tested, the same
    // numbers rmp::Object::collision_mask uses -- so a spike, an enemy and a
    // bullet are one layer each and a player is hurt by whichever it names.
    // The invulnerable window applies, which is what stops standing inside a
    // fire costing sixty hearts a second.
    unsigned hurt_by = 0;
    int damage_on_hit = 1; // what one touch costs

    // Returns whether it landed. Damage during the invulnerable window does
    // not, which is what stops one spike costing three hearts in three frames.
    bool damage(Object &self, int amount = 1);
    void heal(int amount = 1);

    // Whether the window after a hit is still open, during which damage()
    // lands nothing.
    [[nodiscard]] bool invulnerable() const { return ours.invulnerable_left > 0; }
    // Whether hp is 0 or less. A death through damage() is final: heal() does
    // nothing after it.
    [[nodiscard]] bool dead() const { return hp <= 0; }

    void _ready(Object &self);
    void _update(Object &self, float delta);
    void _collision(Object &self, Object &other);
    void _end(Object &self);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        float invulnerable_left = 0;
        bool died = false;
        bool hid = false;
    } ours;
};

// ---------------------------------------------------------------------------
// Places, hits and pickups
// ---------------------------------------------------------------------------

// A region that says who comes and goes: a trigger, a door's sensor, a pit, a
// shop's counter. It is the object it is on -- its collider, its layers -- so
// it sees what that object collides with, and solid or not is the object's
// business (a trigger is not solid, which is the default).
//
//     door.collision_mask = layer::PLAYER;
//     door.add<rmp::behavior::Area>({ .on_enter = [](rmp::Object &, rmp::Object &who) {
//         open_the_door(who);
//     } });
//
// Five moments, each told with the area and the other object:
//   on_enter    the first frame the other touches it
//   on_stay     every frame after that it still does
//   on_exit     the frame after the first one it no longer does
//   on_inside   its collider is wholly inside the area's, from a frame it was not
//   on_outside  it is no longer wholly inside: part of it is out, or it left
// An object destroyed while it touches the area leaves overlapping() and is
// told nothing: it is gone, and there is no object to hand over.
struct Area {
    Callback<Object &, Object &> on_enter; // (the area, who came in)
    Callback<Object &, Object &> on_stay; // (the area, who is still touching)
    Callback<Object &, Object &> on_exit; // (the area, who left)
    Callback<Object &, Object &> on_inside; // (the area, who is wholly inside now)
    Callback<Object &, Object &> on_outside; // (the area, who is no longer wholly inside)

    // Who touched it in the last collision pass, as handles: one destroyed
    // since then reads empty.
    [[nodiscard]] std::span<const Handle<Object>> overlapping() const {
        return ours.touching;
    }
    // Whether `other` touched it in the last collision pass.
    [[nodiscard]] bool contains(const Object &other) const;

    void _update(Object &self, float delta);
    void _collision(Object &self, Object &other);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        std::vector<Handle<Object>> touching; // the last pass's, settled
        std::vector<bool> inside; // beside `touching`: wholly inside, then
        std::vector<Handle<Object>> now; // the pass under way
        std::vector<bool> now_inside;
    } ours;
};

// What hurts: a sword's swing, a spike, a bullet, an explosion. On touching an
// object with an rmp::behavior::Health on one of the layers it `hits`, it
// calls that Health's damage() -- the invulnerable window, on_damage and the
// once-only death all apply -- and pushes it away. A Health's `hurt_by` does
// not count an object with a Hitbox: the Hitbox is what decides, so nothing is
// hurt twice for one touch.
//
// From the editor: "damage" (Int), "knockback" (Float).
struct Hitbox {
    int damage = 1; // what one hit costs
    unsigned hits =
        0xFFFFFFFFu; // the layers it hurts; the collision layers decide who it touches
    float knockback = 0; // the impulse that pushes the one hit away from it
    bool destroy_on_hit = false; // discarded after its first hit, landed or not: a bullet
    bool once_per_target = false; // hits each object once at most: a sword's one swing
    Callback<Object &, Object &>
        on_hit; // (the hitbox, who it hurt), when the damage landed

    [[nodiscard]] static Hitbox from(const MapObject &object);

    void _update(Object &self, float delta);
    void _collision(Object &self, Object &other);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        std::vector<Handle<Object>> struck; // for once_per_target
    } ours;
};

// A coin, a key, a heart. Touched by an object on a layer of `collected_by`,
// it calls on_collect, plays its sound, stops colliding and vanishes -- it
// rises, grows and fades for `vanish_seconds` -- and is destroyed. Within
// `magnet` of a collector it flies to it first. What a coin is worth is the
// game's: on_collect is where it counts.
//
// From the editor: "magnet" (Float).
struct Collectible {
    unsigned collected_by = 0xFFFFFFFFu; // the layers that pick it up
    std::string sound; // played by rmp::audio when it is picked up; "" = silent
    float magnet = 0; // within this many units of a collector it flies to it; 0 = never
    float magnet_speed = 360; // how fast, in units per second
    float vanish_seconds = 0.25f; // how long it takes to go; 0 = at once
    Callback<Object &, Object &> on_collect; // (the pickup, who picked it up), once

    // Whether it has been picked up: from then on it only vanishes.
    [[nodiscard]] bool collected() const { return ours.collected; }

    [[nodiscard]] static Collectible from(const MapObject &object);

    void _update(Object &self, float delta);
    void _collision(Object &self, Object &other);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        bool collected = false;
        float vanished = 0; // seconds since it was picked up
        Vector2 scale{ 1, 1 }; // what it had then
        unsigned char alpha = 255;
        bool pulled = false; // a magnet moved it at its last update
    } ours;
};

// Fires. On an action held down, on its own every `every` seconds, or at a
// target -- one bullet or a pattern of them: `count` spread over `spread`
// degrees, the whole pattern turned by `rotate_per_shot` after each volley,
// which is how a spiral is made.
//
// THE BULLET is a circle of `bullet_radius` in `bullet_color`, with a
// Projectile (gone at a wall or a hit), a Lifespan of `bullet_life` and a
// Hitbox of `damage` on the layers of `hits`. Or it is whatever `on_shoot`
// makes, handed the scene, where it leaves from and its velocity:
//
//     .on_shoot = [](rmp::Scene &s, Vector2 at, Vector2 velocity) {
//         auto &arrow = s.spawn({ .position = at, .velocity = velocity });
//         arrow.sprite.texture = rmp::assets::load_texture("arrow.png");
//     }
//
// From the editor: "every" (Float), "speed" (Float), "count" (Int), "spread"
// (Float), "rotate" (Float, rotate_per_shot), "range" (Float).
struct Shooter {
    std::string action; // fires while it is held; "" = never on input
    bool automatic = false; // fires every `every` seconds on its own
    float every = 0.25f; // the least time between two volleys, in seconds
    Handle<Object> target; // aims at it; while it is set and gone, it does not fire
    float range = 0; // with a target: fires only when it is this close; 0 = any distance
    Vector2 direction{}; // where it aims without a target; {0,0} = the way it faces
    float speed = 360; // the bullets', in units per second
    int count = 1; // bullets per volley
    float spread = 0; // degrees the volley covers; 360 = all around, evenly
    float rotate_per_shot = 0; // degrees the pattern turns after each volley
    Vector2 offset{}; // where the bullets leave from, from its centre
    std::string sound; // played by rmp::audio on each volley; "" = silent

    float bullet_radius = 4; // the default bullet's
    Color bullet_color = WHITE;
    float bullet_life = 2; // seconds before it is discarded
    int damage = 1; // what the default bullet's Hitbox costs
    unsigned hits = 0; // the layers the bullets hit; 0 = its own mask, less its own layer

    // Makes the bullet instead: (the scene, where it leaves from, its velocity).
    Callback<Scene &, Vector2, Vector2> on_shoot;

    // Fires a volley now, if `every` has passed since the last; true if it did.
    bool fire(Object &self);
    // How many volleys it has fired.
    [[nodiscard]] int shots() const { return ours.shots; }

    [[nodiscard]] static Shooter from(const MapObject &object);

    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        float cooldown = 0; // seconds until it may fire again
        float turned = 0; // degrees rotate_per_shot has added up to
        int shots = 0;
    } ours;
};

// ---------------------------------------------------------------------------
// Moving on its own
// ---------------------------------------------------------------------------

// Goes from waypoint to waypoint -- round in a loop, or back and forth with
// `ping_pong` -- waiting `wait` seconds at each. With no waypoints it walks
// the floor instead: left and right at `speed`, turning at a wall and, with
// `turn_at_ledges`, before a drop, which is a platformer's goomba. The way it
// goes sets flip_x, and its sprite plays `walk` and `idle` when its sheet has
// them -- unless the object has an Animator, which then chooses.
//
// Waypoints are for what flies or walks top-down: it moves straight to each,
// on both axes. Floor walking moves only along x, and gravity is the object's.
//
// From the editor: "path" (Array<Point>; the entity's own position is the
// first waypoint), "speed" (Float), "wait" (Float), "ping_pong" (Bool).
struct Patrol {
    std::vector<Vector2> waypoints; // world positions; empty = it walks the floor
    float speed = 60; // units per second
    float wait = 0; // seconds it stays at each waypoint
    bool ping_pong = false; // back and forth; false = from the last straight to the first
    bool turn_at_ledges = true; // walking the floor: turns before a drop
    std::string walk = "walk"; // the tag played while it moves; "" = none
    std::string idle = "idle"; // the tag played while it waits; "" = none

    // The waypoint it is heading for, an index into `waypoints`.
    [[nodiscard]] int heading_for() const { return ours.index; }

    [[nodiscard]] static Patrol from(const MapObject &object);

    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        int index = 0;
        int step = 1; // +1 or -1, for ping_pong
        float waited = 0;
        float way = 1; // walking the floor: +1 right, -1 left
        float was_x = 0; // where it was a frame ago, to see that it is stuck
        bool walked = false; // it pushed itself along at its last update
        bool started = false;
    } ours;
};

// Walks to a target, or to a point, around the map's walls: rmp::find_path(),
// asked again every `repath_every` seconds, and followed at `speed`. Top-down:
// it moves on both axes. A Chase on the same object steers it.
struct Pathfind {
    Handle<Object> target; // where it goes, while it is there
    std::optional<Vector2> goal; // where it goes without a target; empty = it stays
    float speed = 100; // units per second
    bool diagonal = true; // eight directions; false = four
    float clearance = 0; // keeps this far from the walls; see PathOptions
    float repath_every = 0.5f; // seconds between two searches while it walks

    // The cells still ahead, the last one the goal.
    [[nodiscard]] std::span<const Vector2> path() const {
        return std::span<const Vector2>(ours.path).subspan(
            static_cast<std::size_t>(ours.next) < ours.path.size()
                ? static_cast<std::size_t>(ours.next)
                : ours.path.size());
    }
    // Whether it is where it was going: at the goal, or with no way there.
    [[nodiscard]] bool arrived() const { return ours.arrived; }

    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        std::vector<Vector2> path;
        int next = 0;
        float since = 1000; // seconds since the last search
        Vector2 aimed{}; // the goal the path was found for
        bool arrived = false;
    } ours;
};

// Sees a target and goes after it. It sees it within `sight` units, inside a
// cone of `fov` degrees around the way it faces, and -- with `line_of_sight`
// -- when no solid map cell stands between them. While it chases, a Patrol on
// the same object is paused, and a Pathfind, when there is one, finds the way;
// without one it runs straight at the target. After `lose_after` seconds
// without seeing it, it gives up: on_lose, back home with `return_home`, and
// the Patrol resumes.
//
// From the editor: "speed" (Float), "sight" (Float), "fov" (Float).
struct Chase {
    Handle<Object> target; // whom it looks for; the game sets it, usually the player
    float speed = 120; // units per second while it chases
    float sight = 160; // how far it sees; 0 = any distance
    float fov = 360; // degrees of its view, centred on the way it faces
    bool line_of_sight = true; // solid map cells hide the target
    float lose_after = 2; // seconds without seeing it before it gives up
    bool return_home = true; // goes back where it started after giving up
    float stop_distance = 0; // stops this close to the target
    Callback<Object &, Object &> on_spot; // (the chaser, the target), when a chase starts
    Callback<Object &> on_lose; // when it gives up

    // Whether it is chasing now.
    [[nodiscard]] bool chasing() const { return ours.chasing; }
    // Whether it saw the target at its last update.
    [[nodiscard]] bool sees() const { return ours.sees; }

    [[nodiscard]] static Chase from(const MapObject &object);

    void _ready(Object &self);
    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        bool chasing = false;
        bool sees = false;
        bool going_home = false;
        float unseen = 0; // seconds since it last saw the target
        Vector2 home{};
        Vector2 last_seen{};
        Vector2 facing{ 1, 0 };
    } ours;
};

// Which way an Orbit turns its object.
enum class Face {
    NONE, // its rotation is left alone
    CENTRE, // it looks at what it circles
    OUTWARD, // it looks away from it
};

// Circles a handle or a point: a shield of blades, a moon, a fairy. Several on
// one centre spread around it evenly with `index` and `of`.
//
// From the editor: "radius" (Float), "speed" (Float, degrees_per_second),
// "angle" (Float).
struct Orbit {
    Handle<Object> centre; // what it circles, while it is there
    Vector2 point{}; // what it circles without a centre
    float radius = 48; // units from the centre
    float degrees_per_second = 90; // negative turns the other way
    float angle = 0; // degrees, where it is now; it is where it starts too
    Face face = Face::NONE;
    int index = 0; // this one's place among `of` on the same centre
    int of = 1; // how many share the centre, spread evenly

    [[nodiscard]] static Orbit from(const MapObject &object);

    void _late_update(Object &self, float delta);
};

// ---------------------------------------------------------------------------
// The sprite, the pointer and the brain
// ---------------------------------------------------------------------------

// A rule of an Animator: play `tag` while `when` says so. The rules are asked
// in order, before anything the Animator works out itself.
struct AnimationRule {
    std::string tag;
    Condition<Object &> when;
};

// Plays the tag that fits what the object is doing: `die` once its Health is
// dead, `hurt` while it is invulnerable after a hit, `jump` and `fall` in the
// air (with gravity), `run` while it moves and `idle` while it does not -- and
// mirrors it with flip_x by the way it moves. The names are fields, the tags
// of your .aseprite; a tag the sheet lacks is skipped for the next that fits,
// and without a sheet only flip_x is set. Runs after everything has moved.
struct Animator {
    std::string idle = "idle";
    std::string run = "run";
    std::string jump = "jump";
    std::string fall = "fall";
    std::string hurt = "hurt";
    std::string die = "die"; // played once, not looped
    float moving_at = 10; // units per second under which it stands still
    bool flip = true; // sets flip_x while it moves left; the art faces right
    std::vector<AnimationRule> rules; // asked first, in order

    // The tag it chose at its last update; "" when it chose none.
    [[nodiscard]] std::string_view playing() const { return ours.chosen; }

    void _late_update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        std::string chosen;
    } ours;
};

// Picked up with the pointer and put down somewhere. It takes the object's
// on_drag. Let go over an object with an Area -- on a layer of `drop_on`, and
// `accepts` saying yes -- it is dropped there: on_drop, and centred on it with
// `centre_on_drop`. Let go anywhere else it goes back where it was picked up,
// with `return_if_rejected`, or stays, snapped to `snap`, and with `throwable`
// keeps the pointer's speed and slides to a stop.
struct Draggable {
    Vector2 snap{}; // the grid it lands on; {0,0} = none
    bool throwable = false; // keeps the pointer's speed when let go
    float friction = 6; // how fast a throw slows: the share of its speed lost per second
    Rectangle bounds{}; // where it can be dragged; {0,0,0,0} = anywhere
    unsigned drop_on = 0xFFFFFFFFu; // the layers of the Areas it can be dropped on
    bool return_if_rejected = true; // no Area took it: back where it was picked up
    bool centre_on_drop = true; // dropped on an Area: put on its centre
    Condition<Object &, Object &> accepts; // (this, the area); empty = every Area does
    Callback<Object &> on_pick; // the first time it moves under the pointer
    Callback<Object &, Object &> on_drop; // (this, the area it was dropped on)
    Callback<Object &> on_reject; // let go where no Area took it

    // Whether the pointer holds it now.
    [[nodiscard]] bool held() const { return ours.held; }
    // Lets go of it here, as the pointer letting go would: onto the Area under
    // it, or back, or where it is.
    void drop(Object &self);

    void _ready(Object &self);
    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        bool held = false;
        bool moved = false; // a drag event since the last update
        bool going_back = false;
        Vector2 picked_at{};
        Vector2 dragged{}; // this frame's movement, in world units
        Vector2 pace{}; // the pointer's recent velocity
    } ours;
};

// One state of a StateMachine: what it does on the way in, every frame, on
// the way out, and which behaviors run while it is on.
struct State {
    std::string name;
    Callback<Object &> on_enter;
    Callback<Object &, float> on_update; // (the object, delta)
    Callback<Object &> on_exit;
    // The behaviors that run in this state: rmp::behavior::kinds<Patrol>().
    // Every behavior some state names is paused in the states that do not.
    std::vector<int> enables;
};

// A move from one state to another, taken when `when` says so.
struct Transition {
    std::string from; // "" = from any state
    std::string to;
    Condition<Object &> when;
};

// The behaviors a State turns on, by type: `.enables = kinds<Patrol, Animator>()`.
template <class... B> [[nodiscard]] std::vector<int> kinds() {
    return { rmp::detail::type_id<B>()... };
}

// Named states and the moves between them: an enemy that idles, patrols,
// chases and flees; a boss's phases. It starts in `initial` at its first
// update, asks the transitions in order every frame -- the first whose `from`
// matches and whose `when` says yes is taken -- and runs the state's
// on_update. go() moves by name, at its next update.
struct StateMachine {
    std::vector<State> states;
    std::string initial; // "" = the first state
    std::vector<Transition> transitions;

    // Moves to the state called `name` at its next update; a name it does not
    // have is said once and changes nothing.
    void go(std::string_view name) { ours.pending = std::string(name); }
    // The state it is in; "" before its first update.
    [[nodiscard]] std::string_view state() const;
    // How long it has been in it, in seconds.
    [[nodiscard]] float seconds_in_state() const { return ours.seconds; }

    void _update(Object &self, float delta);

    // What it is keeping track of. Readable, because sometimes you want it;
    // not yours to write. It is one PUBLIC member and not a private block
    // because a struct with a private member is not an aggregate in C++20, and
    // then `{ .speed = 320 }` stops compiling -- which is the whole design. The
    // name is the fence.
    struct {
        int current = -1; // an index into `states`
        float seconds = 0;
        std::string pending;
    } ours;
};

} // namespace rmp::behavior
