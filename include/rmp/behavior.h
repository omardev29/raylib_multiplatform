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
// WHAT IS NOT HERE, and why, in short: `patrol` is a Tween, `magnet` is a
// Follow, `draggable` is Object::on_drag, and `tetromino`, `formation` and
// `paddle` are three games with a different name on them.
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
#include <string> // the action and tag names a behavior is given, owned: a
// std::string's c_str() or a temporary passed in cannot dangle

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
    [[nodiscard]] bool on_ground() const { return ours.grounded; }

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
//
// `patrol` is not in this catalogue because it is this: a position that goes
// back and forth between two points with ping_pong on.
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

    // Whether it has stopped on `to`, which only a tween with neither `loop`
    // nor `ping_pong` ever does.
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
        Handle<Object> made[MAX_SPAWNED];
        int made_count = 0;
    } ours;

    // A fresh random amount between -jitter and +jitter from rmp::random::range,
    // or 0 when `jitter` is 0 or less: what each interval after the first is
    // moved by.
    [[nodiscard]] float jitter_amount() const;
};

// Calls you every N seconds. Shooting, blinking, a countdown.
struct Timer {
    float seconds = 1; // between calls; 0 = it never fires
    bool repeat = true; // false = one call, and then it is spent
    // Called with the object the Timer is on. A frame longer than `seconds`
    // calls it once for every interval that frame covered, so a countdown
    // keeps time on a slow machine.
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

} // namespace rmp::behavior
