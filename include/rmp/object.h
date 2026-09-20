#pragma once
// ---------------------------------------------------------------------------
// rmp/object.h — an entity inside a scene, and the handle that outlives it.
//
// A scene has objects. There is no tree, no hierarchy and no parent pointer:
// the player, an enemy, a bullet, a coin and a platform are all the same kind
// of thing, and what makes them different is the data you put in them.
//
//     auto &ball = spawn({ .position = { 400, 225 }, .shape = rmp::circle(6) });
//     ball.velocity = { 320, 120 };
//     ball.edges    = rmp::Edge::BOUNCE;
//
// THE FIELDS ARE PUBLIC ON PURPOSE. This is the class the user touches most
// often in a day, and `player.flip_x = player.velocity.x < 0;` is the point; a
// getter and a setter in front of every field is ceremony between them and it.
// (`misc-non-private-member-variables-in-classes` is off in .clang-tidy for
// exactly this, and says so there.)
//
// THE UNDERSCORE IS THE ACCESS RULE, the same one rmp/scene.h explains:
// `_name` is a method on YOUR type that WE call. You override it, you never
// call it.
//
// `position` IS THE CENTRE, which is the one deliberate disagreement with
// raylib in this whole API. raylib draws from the top-left corner. Here the
// centre is what `position` means, and it is paid for gladly: rotating about
// the centre is what 95 % of 2D sprites want, the shape stays symmetric about
// the position, and `Vector2Distance(a.position, b.position)` means what it
// looks like. The corner would turn every rotation and every distance check
// into a by-hand correction of half the size, which is the mechanical work this
// framework exists to absorb.
// ---------------------------------------------------------------------------

#include <raylib.h>
#include <rmp/assets.h> // rmp::Texture, for Sprite
#include <rmp/config.h>

// No <memory> and no <functional>, and that is the point. Measured on this
// machine against an empty file's 29 ms: <memory> costs 251, <functional> 160,
// <type_traits> 12. Every file with an entity in it includes this header, so
// the first two are 380 ms of nothing per translation unit. The storage lives in
// src/rmp/object.cpp and hands over a raw pointer the way rmp/scene.h already
// does; the callbacks below are forty lines of type erasure instead of
// std::function.
#include <type_traits>

namespace rmp {

class Object;
class Scene;

// ---------------------------------------------------------------------------
// Shapes: what gets drawn when there is no sprite, and — from phase 7 — what
// collides. Two of them, and not five.
//
// RECTANGLE and CIRCLE are the two the collision layer can genuinely resolve
// against each other: rect-rect, circle-circle and circle-rect are three cases
// and all three are there. A capsule that drew as a capsule and collided as a
// box would be a lie in the API, and that is the kind of lie that costs an
// afternoon. To draw anything else, override _draw() and use raylib.
// ---------------------------------------------------------------------------

enum class ShapeKind { NONE, RECTANGLE, CIRCLE };

struct Shape {
    // Declaration order IS the order these get written in a designated
    // initialiser, because C++20 requires that and rejects any other order.
    // Kind first because rect()/circle() set it, then the sizing, then the
    // appearance.
    ShapeKind kind = ShapeKind::NONE;
    Vector2 size{}; // RECTANGLE: width and height
    float radius = 0; // CIRCLE
    Vector2 offset{}; // from the object's centre
    Color color = WHITE; //
    bool filled = true; //
    float thickness = 1; // when filled = false
};

// The two spellings that read like what they make.
Shape rect(Vector2 size);
Shape circle(float radius);

// ---------------------------------------------------------------------------
// A sprite: a texture and how to put it on screen. Animation, sheets and
// per-frame durations are phase 9 and build on top of this without changing it.
// ---------------------------------------------------------------------------

struct Sprite {
    rmp::Texture texture; // empty = there is no sprite
    Rectangle source{}; // {0,0,0,0} = the whole texture
    Vector2 origin{ 0.5f, 0.5f }; // NORMALISED, and centred by default
    Vector2 size{}; // {0,0} = the source's own size
    Color tint = WHITE; //
};

// ---------------------------------------------------------------------------
// What happens at the edge of the world. Five answers to one question, so it is
// one field rather than five flags.
// ---------------------------------------------------------------------------

enum class Edge {
    NONE, // the default: objects may leave, and do
    CLAMP, // stops at the edge          a paddle, a player, a cursor
    BOUNCE, // flips the velocity         a Pong or Breakout ball
    WRAP, // comes back the other side  Asteroids
    DESTROY, // is discarded               bullets, particles
};

// ---------------------------------------------------------------------------
// A callback you hand us: `on_click`, `on_drag`, `on_collision`.
//
// This is std::function's job and <functional> is 131 ms over the baseline in
// every translation unit that includes this header, which is every file with an
// entity in it. What is actually needed here is a small fraction of
// std::function -- no copying, no target(), no comparison -- so it is written
// out: a pointer to the captured state and two function pointers, one to call
// it and one to free it.
//
// No small-buffer optimisation, deliberately. It would save one allocation per
// callback, and callbacks are registered once in _ready() and not per frame, so
// the thing it optimises does not happen.
// ---------------------------------------------------------------------------

template <class... A> class Callback {
public:
    Callback() = default;
    ~Callback() { clear(); }

    // An object is not copyable and neither is this. Moving would be fine and
    // is not offered because nothing needs it.
    Callback(const Callback &) = delete;
    Callback &operator=(const Callback &) = delete;

    template <class F> void set(F &&fn) {
        clear();
        using Fn = std::decay_t<F>;
        static_assert(
            std::is_invocable_v<Fn &, A...>,
            "the callback does not take the arguments this one is called with "
            "-- on_click is void(Object &), on_drag is void(Object &, Vector2), "
            "on_collision is void(Object &, Object &)");
        state_ = new Fn(static_cast<F &&>(fn));
        invoke_ = [](void *state, A... args) { (*static_cast<Fn *>(state))(args...); };
        destroy_ = [](void *state) { delete static_cast<Fn *>(state); };
    }

    void clear() {
        if (destroy_ != nullptr) destroy_(state_);
        state_ = nullptr;
        invoke_ = nullptr;
        destroy_ = nullptr;
    }

    explicit operator bool() const { return invoke_ != nullptr; }
    void operator()(A... args) const {
        if (invoke_ != nullptr) invoke_(state_, args...);
    }

private:
    void *state_ = nullptr;
    void (*invoke_)(void *, A...) = nullptr;
    void (*destroy_)(void *) = nullptr;
};

// ---------------------------------------------------------------------------
// A handle: index plus generation, and the answer to "I want to remember this
// object between frames".
//
//     Dentro del frame, referencia. Entre frames, handle.
//
// spawn() returns a reference and that reference is good for the whole frame
// you got it in, because destruction is deferred to the end of the frame. Kept
// across frames it can dangle, and that is what this is for. Checking one is an
// integer comparison, and it CANNOT come back to life: if the slot is reused
// for another object the generation no longer matches and the handle stays
// dead. That is the classic bug with stored indices and it costs nothing to
// avoid.
// ---------------------------------------------------------------------------

namespace detail {
// Defined in src/rmp/object.cpp, where the storage lives.
Object *resolve(unsigned index, unsigned generation);
} // namespace detail

template <class T = Object> class Handle {
public:
    Handle() = default;

    // Null until it points at something, and false the moment that something
    // stops existing. A default-constructed handle has generation 0, which no
    // live slot ever has.
    [[nodiscard]] T *get() const {
        return static_cast<T *>(rmp::detail::resolve(index_, generation_));
    }
    explicit operator bool() const { return get() != nullptr; }
    T *operator->() const { return get(); }
    T &operator*() const { return *get(); }

    friend bool operator==(const Handle &a, const Handle &b) {
        return a.index_ == b.index_ && a.generation_ == b.generation_;
    }

private:
    friend class rmp::Object;
    unsigned index_ = 0;
    unsigned generation_ = 0; // 0 is the "points at nothing" generation

public:
    // Public so Object::handle() can build one without befriending every
    // instantiation. Not for calling: the index and generation are ours.
    Handle(unsigned index, unsigned generation)
        : index_(index), generation_(generation) {}
};

// ---------------------------------------------------------------------------
// Raycasting. In Godot it is one line and here it has to be one too: right now
// the alternative is CheckCollisionLines against every object by hand, and that
// is not acceptable for something every game with a gun, a line of sight or a
// ground check needs.
//
//     if (auto hit = scene()->raycast(muzzle, muzzle + aim * 400)) {
//         spawn_spark(hit.point, hit.normal);
//     }
//
// Three things make it worth having over the hand-written version:
//
//   `ignore`, because without it the first thing every shot hits is the thing
//   that fired it. That is the number one bug in hand-written raycasts and it
//   costs one field to remove.
//
//   The normal, because it is what orients the spark, the bullet hole and the
//   bounce. Getting it right against a circle and against a box is not hard,
//   but it is worth doing once instead of in every game.
//
//   It walks the GRID, not the list. The same uniform grid the broad phase
//   builds, with a DDA, so only the objects in the cells the line crosses are
//   tested. With a thousand objects on screen a ray touches a handful -- and it
//   is nearly free precisely BECAUSE the grid is already there for the
//   collision pass. That is why the two live in the same phase.
// ---------------------------------------------------------------------------

struct RayHit {
    Object *object = nullptr; // what was hit
    Vector2 point{}; // where, in world coordinates
    Vector2 normal{}; // the surface normal there, pointing back at the ray
    float distance = 0; // from the ray's origin

    // `if (auto hit = raycast(a, b))` is the shape this is for.
    explicit operator bool() const { return object != nullptr; }
};

struct RayQuery {
    Vector2 from{};
    Vector2 to{};
    unsigned mask = 0xFFFFFFFFu; // the same layers as the collision pass
    Object *ignore = nullptr; // normally whoever is shooting
};

// ---------------------------------------------------------------------------
// What spawn() takes. The order is the order it gets written in.
// ---------------------------------------------------------------------------

struct ObjectOptions {
    Vector2 position{};
    Vector2 size{}; // shorthand for a RECTANGLE shape of this size
    Shape shape{}; // the full form; used instead of `size` when set
    Vector2 velocity{};
    Vector2 scale{ 1, 1 };
    float rotation = 0;
    int layer = 0;
    bool visible = true;
    Edge edges = Edge::NONE;
    Rectangle bounds{}; // {0,0,0,0} = the view
};

// ---------------------------------------------------------------------------

class Object {
public:
    Object() = default;
    virtual ~Object() = default;

    // Objects are owned by their scene and referred to by reference and handle.
    // A copy would be a second object that believes it is in the scene.
    Object(const Object &) = delete;
    Object &operator=(const Object &) = delete;

    // ---- transform --------------------------------------------------------
    Vector2 position{}; // THE CENTRE. See the header comment.
    Vector2 scale{ 1, 1 };
    float rotation = 0; // degrees, as raylib counts them
    Vector2 velocity{}; // integrated every frame, by us
    float mass = 1.0f; // only apply_force / apply_impulse read it

    // ---- appearance -------------------------------------------------------
    Sprite sprite; // if there is a sprite, the sprite is drawn
    Shape shape; // otherwise this is
    bool flip_x = false;
    bool flip_y = false;
    bool visible = true;
    int layer = 0; // draw order; ties break on creation order

    // ---- world ------------------------------------------------------------
    // 0 means gravity does not touch this object, and that default is not
    // negotiable: a top-down game cannot have things falling over. The scene
    // carrying a downward gravity that nobody uses until they ask is what lets
    // one Object serve a platformer and a Zelda.
    float gravity_scale = 0;
    Edge edges = Edge::NONE;
    Rectangle bounds{}; // empty = the view

    // What it collides AS. NONE means "the same as `shape`", or the sprite's
    // frame when there is no shape -- which is right until it is not, and the
    // moment it is not is the most common step up in the whole API: a
    // character's sprite includes hair, a cape and air, and nobody wants to
    // collide with the air.
    //
    //     player.sprite   = sheet;                    // drawn as the picture
    //     player.collider = rmp::rect({ 20, 44 });    // collided as a box
    Shape collider;

    // Three fields, three different questions, and keeping them apart is what
    // makes the common case free:
    //
    //   collider   what shape?          deduced from shape/sprite
    //   solid      resolve, or notify?  false: notify only
    //   immovable  who gets moved?      false: this one does
    //
    // `solid` is false by default because most objects in a 2D game are not
    // walls -- bullets, coins, particles, pickups, triggers -- and the two that
    // are are exactly the two you configure:
    //
    //     ground.solid = true;  ground.immovable = true;
    //     player.solid = true;  player.gravity_scale = 1;
    bool solid = false;
    bool immovable = false;

    // Godot's model, because it is the right one. Two objects interact when
    // either one's mask touches the other's layer. Both default to 1, so
    // everything collides with everything and the simple case pays nothing --
    // the Pong in 01-principles.md never mentions a layer.
    //
    // Without them there are three things you cannot say, and they turn up in
    // about the fourth game anybody writes: that the player's bullet must not
    // hit the player, that enemies pass through each other but not through
    // walls, and that a trigger sees the player and nothing else. The
    // alternative is an `if` at the top of every _collision, which is the
    // mechanical work this framework exists to absorb.
    //
    //     namespace layer { constexpr unsigned kPlayer = 1 << 0, kEnemy = 1 << 1,
    //                                          kBullet = 1 << 2, kWorld = 1 << 3; }
    //     bullet.collision_layer = layer::kBullet;
    //     bullet.collision_mask  = layer::kEnemy | layer::kWorld;   // not the player
    unsigned collision_layer = 1; // which layers I am ON
    unsigned collision_mask = 1; // which layers I collide WITH

    // ---- what you override, all of it empty by default --------------------
    virtual void _ready() {}
    virtual void _update(float delta) { (void)delta; }
    virtual void _draw() {} // the sprite and the shape draw themselves
    virtual void _end() {} // on destruction: drop loot, tell somebody

    // Once per pair per frame, on BOTH objects: a touching b calls
    // a._collision(b) and b._collision(a). It fires whether or not either one
    // is `solid` -- solid decides whether they are pushed apart, not whether
    // you are told.
    virtual void _collision(Object &other) { (void)other; }

    // ---- forces -----------------------------------------------------------
    // The difference between these two is the whole physics API:
    //
    //   if you call it every frame it is a force; if you call it once it is an
    //   impulse.
    //
    // Both exist because using the wrong one gives exactly the bug you would
    // expect. A force applied once barely moves anything, and an impulse
    // applied every frame goes twice as fast at 120 fps as at 60.
    void apply_force(Vector2 force); // sustained: velocity += f/mass*delta
    void apply_impulse(Vector2 impulse); // instant:   velocity += i/mass

    // ---- callbacks, for when a subclass is more than you want --------------
    //
    // The other half of the underscore rule: `_name` is yours and we call it,
    // `on_name` is ours and you hand us a function. They coexist without
    // ambiguity, which is the whole reason the rule exists -- an object can
    // override _collision AND carry an on_collision, and both run.
    //
    //     coin.on_collision([&](rmp::Object &self, rmp::Object &other) {
    //         if (&other == player) { score += 10; self.destroy(); }
    //     });
    //
    // Setting one twice replaces it. There is no list, because a list of
    // handlers is a signal system, and that is a bigger idea than this needs.
    template <class F> void on_click(F &&fn) { click_.set(static_cast<F &&>(fn)); }
    template <class F> void on_drag(F &&fn) { drag_.set(static_cast<F &&>(fn)); }
    template <class F> void on_collision(F &&fn) {
        collision_.set(static_cast<F &&>(fn));
    }

    // ---- identity and life ------------------------------------------------
    // handle() is the plain one and handle<Goblin>() is the typed one, which is
    // what a behavior holding a target wants: `if (!target) return;` and then
    // `target->hp` without a cast.
    template <class T = Object> [[nodiscard]] Handle<T> handle() const {
        return Handle<T>(index_, generation_);
    }
    [[nodiscard]] Scene *scene() const { return scene_; }

    // The axis-aligned box this object occupies right now, from the sprite or
    // the shape. Empty when it has neither, which is what an invisible logic
    // object is. Phase 7's collider defaults to this.
    [[nodiscard]] Rectangle world_bounds() const;

    // The box the collider occupies right now: `collider` if it has one, else
    // world_bounds(). For debug drawing and for a game doing its own query --
    // the collision pass works with the precise shape and not with this box, so
    // a circle collides as a circle.
    [[nodiscard]] Rectangle world_collider() const;

    // Deferred: the object stops updating and drawing IMMEDIATELY, and its
    // memory is released after the frame. Calling it twice is harmless.
    void destroy();

    // False from the moment destroy() is called, not from the moment the
    // memory goes away.
    [[nodiscard]] bool alive() const { return alive_; }

private:
    friend class Scene;
    friend struct Storage;

    Callback<Object &> click_;
    Callback<Object &, Vector2> drag_;
    Callback<Object &, Object &> collision_;

    Scene *scene_ = nullptr;
    unsigned index_ = 0;
    unsigned generation_ = 0;
    bool alive_ = true;
    Vector2 pending_force_{}; // accumulated by apply_force, spent on integrate

    // Where the centre was before this frame's integration. The swept test
    // needs it: an object that crossed a thin wall never overlapped it on any
    // frame, so the only evidence it was ever there is the segment it covered.
    Vector2 previous_position_{};
};

} // namespace rmp
