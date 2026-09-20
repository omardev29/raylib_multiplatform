#pragma once
// ---------------------------------------------------------------------------
// rmp/behavior.h — the catalogue.
//
//     ball.add<rmp::behavior::Ball>({ .speed = 320 });
//     player.add<rmp::behavior::TopDown>({ .speed = 220 });
//
// A behavior is any struct with `void _update(rmp::Object &, float)`. The model
// is explained above Object::add in rmp/object.h, and the part worth repeating
// here is that these thirteen are structs exactly like one of yours: there is
// no extension API, because there is nothing to extend.
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
// WHAT IS NOT HERE, and why, is the table at the end of
// next_architecture/05-behaviors.md. The short version: `patrol` is `tween`,
// `magnet` is `follow`, `draggable` is `on_drag`, and `tetromino`, `formation`
// and `paddle` are three games with a different name on them.
// ---------------------------------------------------------------------------

#include <raylib.h>
#include <rmp/config.h>
#include <rmp/input.h>
#include <rmp/object.h>
#include <rmp/scene.h>

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

    // THE ANIMATION NAMES ARE YOURS: they are the tags in your .aseprite. These
    // two are what Aseprite puts there by default, not a convention imposed on
    // you. The tag actually chosen is "<walk>_<suffix>" if the sheet has it and
    // "<walk>" with flip_x if it does not, so a two-, four- or eight-direction
    // sheet all work with nothing configured. Wired up in phase 9, with
    // rmp::SpriteSheet; until then the direction and the flip are set and the
    // names are carried.
    const char *idle = "idle";
    const char *walk = "walk";
    const char *suffixes[8] = { "e", "ne", "n", "nw", "w", "sw", "s", "se" };

    // Which actions move it. Empty = the move_* actions that come as standard.
    const char *left = "";
    const char *right = "";
    const char *up = "";
    const char *down = "";

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
    float speed = 260;
    float acceleration = 0; // 0 = instant
    float gravity = 2000;
    float jump = 700;
    int air_jumps = 0; // 1 = a double jump

    // Still jumpable for this long after walking off a ledge. Named after the
    // coyote who keeps running for a moment past the cliff.
    float coyote_time = 0.1f;
    // A jump pressed this long before landing still fires on landing.
    float jump_buffer = 0.12f;

    const char *left = "";
    const char *right = "";
    const char *jump_action = "";

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
// -- Spawner reads it to produce by distance rather than by time.
struct Runner {
    float speed = 320; // units per second, right now
    float accelerate = 6; // how much `speed` gains per second. 0 = constant
    float max_speed = 900;
    float gravity = 2200;
    float jump = 720;
    int air_jumps = 0;

    const char *jump_action = ""; // empty = ui_accept
    const char *duck_action = "";

    [[nodiscard]] float distance() const { return ours.distance; }
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
        float ground_y = 0;
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
    bool destroy_on_hit = true;

    void _ready(Object &self);
    void _collision(Object &self, Object &other);
    void _update(Object &self, float delta);
};

// Chases a handle, with a speed and a distance it stops at. A handle and not a
// pointer, because the thing being chased is exactly the thing most likely to
// die while being chased.
struct Follow {
    Handle<Object> target;
    float speed = 150;
    float stop_distance = 0;
    float acceleration = 0; // 0 = instant

    void _update(Object &self, float delta);
};

// ---------------------------------------------------------------------------
// Tween: animate one property, with easing, looping and ping-pong.
//
// `patrol` is not in this catalogue because it is this: a position that goes
// back and forth between two points with ping_pong on.
// ---------------------------------------------------------------------------

enum class Ease { LINEAR, IN, OUT, IN_OUT };

enum class Property {
    POSITION_X,
    POSITION_Y,
    SCALE_X,
    SCALE_Y,
    ROTATION,
    ALPHA, // the shape's colour, or the sprite's tint
};

struct Tween {
    Property property = Property::POSITION_Y;
    float from = 0;
    float to = 0;
    float seconds = 1;
    Ease ease = Ease::IN_OUT;
    bool loop = false;
    bool ping_pong = false;

    [[nodiscard]] bool finished() const { return ours.done; }
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
    Vector2 cell{ 32, 32 };
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
    const char *texture = ""; // a name for rmp::assets::load_texture
    float factor = 0.5f; // 1 = moves with the camera, 0 = pinned
    float speed = 0; // units per second of its own, for a sky that drifts
    float y = 0; // where the top of the strip sits
    Color tint = WHITE;

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

// Produces objects every N seconds, or every N units TRAVELLED.
//
// By distance is not the same as a Timer, and the difference is the whole
// reason this is not one: in an endless runner the speed climbs, so "every two
// seconds" puts the obstacles further and further apart and the game gets
// EASIER the faster you go, which is the opposite of the intention.
struct Spawner {
    float every_seconds = 0; // one of these two, not both
    float every_distance = 0;
    float jitter = 0; // +/- this much, uniformly
    int max_alive = 0; // 0 = no limit

    // Where "travelled" is measured from. Empty = this object's own position.
    Handle<Object> track;

    Callback<Scene &, Vector2> on_spawn;

    [[nodiscard]] int alive() const { return ours.alive; }

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
        int alive = 0;
    } ours;

    [[nodiscard]] float jitter_amount() const;
};

// Calls you every N seconds. Shooting, blinking, a countdown.
struct Timer {
    float seconds = 1;
    bool repeat = true;
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
    float seconds = 1;

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
    int hp = 3;
    int max_hp = 3;
    float invulnerable_for = 0.6f; // 0 = no window
    float blink_hz = 12;
    bool destroy_on_death = true;

    Callback<Object &> on_death;
    Callback<Object &, int> on_damage; // the amount that actually landed

    // Returns whether it landed. Damage during the invulnerable window does
    // not, which is what stops one spike costing three hearts in three frames.
    bool damage(Object &self, int amount = 1);
    void heal(int amount = 1);

    [[nodiscard]] bool invulnerable() const { return ours.invulnerable_left > 0; }
    [[nodiscard]] bool dead() const { return hp <= 0; }

    void _update(Object &self, float delta);
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
