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

// No <memory> here, and that is the point: it costs 605 ms in a translation
// unit on this machine and every file with an entity in it would pay. The
// storage lives in src/rmp/object.cpp and this header hands over a raw pointer,
// exactly the way rmp/scene.h already does for the scene stack.

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

    // ---- what you override, all of it empty by default --------------------
    virtual void _ready() {}
    virtual void _update(float delta) { (void)delta; }
    virtual void _draw() {} // the sprite and the shape draw themselves
    virtual void _end() {} // on destruction: drop loot, tell somebody

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

    // Deferred: the object stops updating and drawing IMMEDIATELY, and its
    // memory is released after the frame. Calling it twice is harmless.
    void destroy();

    // False from the moment destroy() is called, not from the moment the
    // memory goes away.
    [[nodiscard]] bool alive() const { return alive_; }

private:
    friend class Scene;
    friend struct Storage;

    Scene *scene_ = nullptr;
    unsigned index_ = 0;
    unsigned generation_ = 0;
    bool alive_ = true;
    Vector2 pending_force_{}; // accumulated by apply_force, spent on integrate
};

} // namespace rmp
