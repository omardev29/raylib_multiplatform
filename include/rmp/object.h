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

// <memory> for std::shared_ptr, which owns a Callback's captured state and an
// object's behaviors and carries each one's deleter, so neither needs a
// `delete` of ours. <type_traits> for the checks that turn a wrong callback,
// handle or behavior into a sentence instead of a page of template errors.
// There is no <functional>: Callback below does the part of std::function that
// is needed, and also takes a move-only lambda, which std::function cannot.
#include <cstddef> // std::nullptr_t, for `ref == nullptr`
#include <memory> // std::shared_ptr: what owns a callback's state and a behavior
#include <string_view> // a sprite's tag names
#include <type_traits>

namespace rmp {

class Object;
class Scene;

// ---------------------------------------------------------------------------
// Shapes: what gets drawn when there is no sprite, and what collides. Two of
// them, and not five.
//
// RECTANGLE and CIRCLE are the two the collision layer can genuinely resolve
// against each other: rect-rect, circle-circle and circle-rect are three cases
// and all three are there. A capsule that drew as a capsule and collided as a
// box would be a lie in the API, and that is the kind of lie that costs an
// afternoon. To draw anything else, override _draw() and use raylib.
// ---------------------------------------------------------------------------

// Which of the two a Shape is. RECTANGLE is a box of `size` and CIRCLE a disc
// of `radius`. NONE is no shape at all: nothing is drawn, and a `collider` of
// NONE means the object collides as its `shape`, or as its sprite's frame.
enum class ShapeKind { NONE, RECTANGLE, CIRCLE };

// A rectangle or a circle around an object's centre, moved by `offset` and
// sized by the object's `scale`. An Object's `shape` is what it draws when it
// has no sprite and, unless its `collider` says otherwise, what it collides
// as. rect() and circle() make one.
struct Shape {
    // Declaration order IS the order these get written in a designated
    // initialiser, because C++20 requires that and rejects any other order.
    // Kind first because rect()/circle() set it, then the sizing, then the
    // appearance.
    ShapeKind kind = ShapeKind::NONE;
    Vector2 size{}; // RECTANGLE: width and height
    float radius = 0; // CIRCLE
    Vector2 offset{}; // from the object's centre
    Color color = WHITE; // the fill, or the outline when not filled
    bool filled = true; // false draws the outline only
    float thickness = 1; // the outline's width when filled = false; a CIRCLE ignores it
};

// The two spellings that read like what they make.
Shape rect(Vector2 size);
Shape circle(float radius);

// ---------------------------------------------------------------------------
// A sprite: a picture and how to put it on screen. A loose texture, or an
// .aseprite sheet, which brings its frames, tags and per-frame durations.
// ---------------------------------------------------------------------------

// What an Object draws in place of its shape when it has one: a texture or a
// sheet, placed by `origin` around the object's position and turned, scaled and
// flipped with the object. A sheet also animates; play() picks the tag.
struct Sprite {
    rmp::Texture texture; // a loose picture, when there is no animation
    rmp::SpriteSheet sheet; // an .aseprite; `sheet` wins over `texture`
    Rectangle source{}; // {0,0,0,0} = the whole texture, or the sheet's frame
    Vector2 origin{ 0.5f, 0.5f }; // NORMALISED, and centred by default
    Vector2 size{}; // {0,0} = the source's own size
    Color tint = WHITE; // multiplies the picture's colours; WHITE leaves them as they are
    float speed = 1.0f; // 2 = twice as fast, 0 = frozen, -1 = backwards

    // ---- animation, when there is a sheet ---------------------------------
    //
    //     player.sprite.sheet = rmp::assets::load_sheet("player.aseprite");
    //     player.sprite.play("walk");     // "walk" is a tag of YOURS
    //
    // A literal, a std::string or a std::string_view, as it comes. playing()
    // hands back the sheet's own name for the tag, or "" when nothing plays:
    // valid for as long as the sheet is loaded, and the characters after it
    // are a NUL, which tests/animation_test.cpp holds it to.
    //
    // A tag that is not in the sheet WARNS ONCE and does nothing -- it does not
    // land on a blank frame, and it does not fill the console sixty times a
    // second saying so.
    void play(std::string_view tag, bool loop = true);
    void stop();
    [[nodiscard]] bool finished() const;
    [[nodiscard]] std::string_view playing() const;

    // The escape hatch: drive the frames yourself.
    [[nodiscard]] int frame_index() const { return ours.frame; }
    void set_frame(int index);

    // What it is keeping track of. See the note on `ours` in rmp/behavior.h:
    // public because a private member would stop this being an aggregate.
    struct {
        int tag = -1; // an index into the sheet's tags, -1 = none
        int frame = 0; // an index into the sheet's frames
        float elapsed = 0; // seconds spent on this frame
        bool loop = true;
        bool done = false;
        bool back = false; // ping-pong, on the way back
    } ours;
};

// ---------------------------------------------------------------------------
// What happens at the edge of the world. Five answers to one question, so it is
// one field rather than five flags.
// ---------------------------------------------------------------------------

// What an object does at the edge of its `bounds` -- the map's, or the view's,
// when `bounds` is empty. CLAMP and BOUNCE act as soon as world_bounds()
// crosses an edge; WRAP and DESTROY wait until it is entirely outside.
enum class Edge {
    NONE, // the default: objects may leave, and do
    CLAMP, // stops at the edge          a paddle, a player, a cursor
    BOUNCE, // flips the velocity         a Pong or Breakout ball
    WRAP, // comes back the other side  Asteroids
    DESTROY, // is discarded once it has left bullets, particles (it has to have been inside)
};

// ---------------------------------------------------------------------------
// A callback you hand us: `on_click`, `on_drag`, `on_collision`.
//
// This is std::function's job and <functional> is 131 ms over the baseline in
// every translation unit that includes this header, which is every file with an
// entity in it. What is actually needed here is a small fraction of
// std::function -- no copying, no target(), no comparison -- so it is written
// out: a shared_ptr to the captured state, which carries its own deleter, and
// one function pointer to call it. Not copying is also what lets it take a
// lambda that captures a std::unique_ptr; std::function requires a copyable
// one.
//
// No small-buffer optimisation, deliberately. It would save one allocation per
// callback, and callbacks are registered once in _ready() and not per frame, so
// the thing it optimises does not happen.
// ---------------------------------------------------------------------------

// A function the framework keeps and calls later with arguments of types A...:
// what on_click(), on_drag(), on_collision() and Tilemap::on_object() store, and
// the type of the `on_` fields of the behaviors. A lambda converts to it; it
// moves and does not copy.
template <class... A> class Callback {
public:
    Callback() = default;
    ~Callback() { clear(); }

    // Copying would mean copying the captured state, which needs a third
    // function pointer and is never asked for. Moving IS asked for: a behavior
    // that carries one is constructed by value -- `add<Spawner>({ .on_spawn =
    // [&]{...} })` -- and has to get here in one piece.
    Callback(const Callback &) = delete;
    Callback &operator=(const Callback &) = delete;

    Callback(Callback &&other) noexcept { steal(other); }
    Callback &operator=(Callback &&other) noexcept {
        if (this != &other) {
            clear();
            steal(other);
        }
        return *this;
    }

    // So that a lambda can be written straight into a designated initialiser,
    // which is the whole point of the behavior structs being aggregates.
    template <class F>
        requires(!std::is_same_v<std::decay_t<F>, Callback>)
    Callback(F &&fn) { // NOLINT(google-explicit-constructor)
        set(static_cast<F &&>(fn));
    }

    // Stores `fn` in place of whatever was there. A function that cannot be
    // called with A... is a compile error that says so.
    template <class F> void set(F &&fn) {
        clear();
        using Fn = std::decay_t<F>;
        static_assert(
            std::is_invocable_v<Fn &, A...>,
            "the callback does not take the arguments this one is called with "
            "-- on_click is void(Object &), on_drag is void(Object &, Vector2), "
            "on_collision is void(Object &, Object &)");
        // A shared_ptr<void> and not a unique_ptr: it carries Fn's deleter
        // inside it, so the type is erased with no `delete` of ours and no
        // <functional>. Ownership is still single -- the copy constructor above
        // is deleted -- the control block is just where the deleter lives.
        _state = std::make_shared<Fn>(static_cast<F &&>(fn));
        _invoke = [](void *state, A... args) { (*static_cast<Fn *>(state))(args...); };
    }

    // Drops the stored function, and with it whatever it captured.
    void clear() {
        _state.reset();
        _invoke = nullptr;
    }

    // Whether a function is stored.
    explicit operator bool() const { return _invoke != nullptr; }
    // Calls the stored function with `args`; an empty Callback does nothing.
    void operator()(A... args) const {
        if (_invoke != nullptr) _invoke(_state.get(), args...);
    }

private:
    using Invoke = void (*)(void *, A...);

    void steal(Callback &other) {
        _state = other.take_state();
        _invoke = other.take_invoke();
    }
    // What steal() takes from the other callback, leaving it empty.
    std::shared_ptr<void> take_state() {
        std::shared_ptr<void> had = std::move(_state);
        _state.reset();
        return had;
    }
    Invoke take_invoke() {
        const Invoke had = _invoke;
        _invoke = nullptr;
        return had;
    }

    std::shared_ptr<void> _state;
    Invoke _invoke = nullptr;
};

// ---------------------------------------------------------------------------
// The type tags and the stand-in behind rmp::Ref, below. Not for you.
// ---------------------------------------------------------------------------

namespace detail {

// A number per type, without RTTI: the first time type_id<T>() is asked for,
// T gets the next one, and keeps it -- the static lives in an inline function
// template, so it is one per program, whichever file asks. What tells one
// behavior, and one scene, from another. The counter is in src/rmp/object.cpp.
int next_type_id();
template <class T> int type_id() {
    static const int ID = next_type_id();
    return ID;
}

// Said once per type, the first time an empty Ref is reached through * or ->;
// and, for a type there is no stand-in for (one with no default constructor,
// or an abstract one), the end of the program. Both in src/rmp/object.cpp.
void report_empty_ref(int type);
[[noreturn]] void no_stand_in(int type);

// What * and -> reach on an empty Ref: a T made for the purpose, fresh on every
// use, so a read through it gives a default T and a write lands nowhere. It is
// never destroyed by the language -- a stand-in Object would otherwise go after
// the engine it has to tell it is going -- which is what the union is for.
template <class T> T &stand_in() {
    using Plain = std::remove_const_t<T>;
    report_empty_ref(type_id<Plain>());
    if constexpr (std::is_default_constructible_v<Plain>) {
        union Holder {
            Plain value;
            Holder() : value() {}
            ~Holder() {}
            Holder(const Holder &) = delete;
            Holder &operator=(const Holder &) = delete;
        };
        static Holder holder;
        std::destroy_at(&holder.value);
        std::construct_at(&holder.value);
        return holder.value;
    } else {
        no_stand_in(type_id<Plain>());
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// An optional reference: something that may not be there, and is not yours.
//
//     if (auto health = player.get<Health>()) health->hp -= 1;
//
// What Object::get<B>() and Handle::get() hand back. It is empty, or it refers
// to something; it owns nothing, and asking it is the `if` above -- true when
// there is something, and then -> and * reach it. Keep it within the frame,
// like the reference spawn() returns; across frames, keep the Handle.
//
// Not a `T *`: a pointer cannot say whether it may be null or whose it is, and
// this type says both. Not std::optional<std::reference_wrapper<T>> either,
// which says the same for the price of <functional> in every file with an
// object in it, and reads as `health->get().hp`.
//
// REACHING INTO AN EMPTY ONE is a mistake in the game, and is said once, per
// type: * and -> then reach a stand-in, a default T made for the purpose and
// made again on every use, so the line that forgot the `if` writes to nothing
// instead of taking the player's game down with it. ([dev] strict stops a
// debug build there instead.) A type with no default constructor has no
// stand-in, and that one mistake ends the program -- with the same message.
// ---------------------------------------------------------------------------
template <class T> class Ref {
public:
    // Empty: refers to nothing, and is false.
    Ref() = default;
    // Refers to `target`, which stays whoever's it was.
    explicit Ref(T &target) : _target(&target) {}

    // True when it refers to something.
    explicit operator bool() const { return _target != nullptr; }
    // What it refers to -- or, empty, the stand-in above, and a line in the log.
    T &operator*() const {
        return _target != nullptr ? *_target : rmp::detail::stand_in<T>();
    }
    T *operator->() const { return &**this; }

    // The same thing, or both empty. And `ref == nullptr` asks "empty?" in
    // the spelling everybody already reads.
    friend bool operator==(const Ref &, const Ref &) = default;
    friend bool operator==(const Ref &ref, std::nullptr_t) { return !ref; }

private:
    T *_target = nullptr;
};

// ---------------------------------------------------------------------------
// A handle: index plus generation, and the answer to "I want to remember this
// object between frames".
//
//     Within a frame, a reference. Across frames, a handle.
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
Ref<Object> resolve(unsigned index, unsigned generation);
} // namespace detail

// A reference to an object that is safe to keep across frames. It gives the
// object back while it is alive, and nothing from the moment destroy() is
// called on it, even once its slot holds another object. Object::handle() makes
// one; T is the type get() hands back.
template <class T = Object> class Handle {
public:
    // get() is an unchecked downcast, a static_cast. A dynamic_cast would make
    // it a checked one, and would make the framework need RTTI, which nothing
    // in it uses -- and a handle resolved every frame would pay for the type
    // walk. The generation stops a handle resolving to a DIFFERENT object; what
    // it cannot stop is resolving to the same object through a type it never
    // was. This catches the half a compiler can catch, at no runtime cost: a T
    // that is not an Object at all is a mistake, not a risk somebody took on
    // purpose.
    static_assert(std::is_base_of_v<Object, T>,
                  "Handle<T> holds an rmp::Object. T has to derive from it -- "
                  "handle<Goblin>() on an object that is a Goblin, not a handle "
                  "to something else entirely.");

    Handle() = default;

    // Empty until it points at something, and empty again the moment that
    // something stops existing. A default-constructed handle has generation 0,
    // which no live slot ever has.
    //
    // -> and * on a handle whose object is gone are the empty Ref's: said
    // once, and a stand-in instead of a crash. Ask first:
    //
    //     if (target) target->hp -= 1;
    [[nodiscard]] Ref<T> get() const {
        const Ref<Object> found = rmp::detail::resolve(_index, _generation);
        return found ? Ref<T>(static_cast<T &>(*found)) : Ref<T>{};
    }
    explicit operator bool() const { return static_cast<bool>(get()); }
    T *operator->() const { return get().operator->(); }
    T &operator*() const { return *get(); }

    // The same slot and the same generation: the same object, or the same
    // nothing. Compared member by member, which is what = default does.
    friend bool operator==(const Handle &, const Handle &) = default;

private:
    friend class rmp::Object;
    unsigned _index = 0;
    unsigned _generation = 0; // 0 is the "points at nothing" generation

public:
    // Public so Object::handle() can build one without befriending every
    // instantiation. Not for calling: the index and generation are ours.
    Handle(unsigned index, unsigned generation)
        : _index(index), _generation(generation) {}
};

// ---------------------------------------------------------------------------
// The behavior engine's erased half. Everything here is called by the templates
// at the bottom of this file and by nothing else.
// ---------------------------------------------------------------------------

namespace detail {

// The hooks a behavior may have, erased. A null entry means the struct did not
// declare that one, and the engine skips it -- which is how an optional hook
// costs nothing rather than costing a virtual call that does nothing.
struct BehaviorOps {
    void (*update)(void *, Object &, float) = nullptr;
    void (*late_update)(void *, Object &, float) = nullptr;
    void (*ready)(void *, Object &) = nullptr;
    void (*draw)(void *, Object &) = nullptr;
    void (*collision)(void *, Object &, Object &) = nullptr;
    void (*end)(void *, Object &) = nullptr;
};

template <class B> const BehaviorOps &ops_for() {
    static const BehaviorOps OPS = [] {
        BehaviorOps o;
        if constexpr (requires(B &b, Object &o2, float d) { b._update(o2, d); }) {
            o.update = [](void *self, Object &object, float delta) {
                static_cast<B *>(self)->_update(object, delta);
            };
        }
        if constexpr (requires(B &b, Object &o2, float d) { b._late_update(o2, d); }) {
            o.late_update = [](void *self, Object &object, float delta) {
                static_cast<B *>(self)->_late_update(object, delta);
            };
        }
        if constexpr (requires(B &b, Object &o2) { b._ready(o2); }) {
            o.ready = [](void *self, Object &object) {
                static_cast<B *>(self)->_ready(object);
            };
        }
        if constexpr (requires(B &b, Object &o2) { b._draw(o2); }) {
            o.draw = [](void *self, Object &object) {
                static_cast<B *>(self)->_draw(object);
            };
        }
        if constexpr (requires(B &b, Object &o2, Object &o3) { b._collision(o2, o3); }) {
            o.collision = [](void *self, Object &object, Object &other) {
                static_cast<B *>(self)->_collision(object, other);
            };
        }
        if constexpr (requires(B &b, Object &o2) { b._end(o2); }) {
            o.end = [](void *self, Object &object) {
                static_cast<B *>(self)->_end(object);
            };
        }
        return o;
    }();
    return OPS;
}

// Takes the behavior -- a shared_ptr<void> because that is what carries B's
// deleter without a `delete` of ours -- and returns a non-owning pointer to
// it. `type` is type_id<B>(). Defined in src/rmp/behavior.cpp.
void *attach(Object &self, int type, const BehaviorOps &ops, std::shared_ptr<void> data);
void *find_behavior(const Object &self, int type);
void detach(Object &self, int type);

} // namespace detail

// ---------------------------------------------------------------------------
// Raycasting. In Godot it is one line and here it has to be one too: right now
// the alternative is CheckCollisionLines against every object by hand, and that
// is not acceptable for something every game with a gun, a line of sight or a
// ground check needs.
//
//     if (auto hit = scene().raycast(muzzle, muzzle + aim * 400)) {
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
//   collision pass.
//
//   The other side of sharing it: the grid is rebuilt when the framework moves
//   the world -- a spawn, a destroy, the start of each update pass and of each
//   collision pass -- and not between two lines of your own code. A ray fired
//   straight after writing somebody else's `position` by hand answers from the
//   start of the current pass, which is what a stepped physics world does. The
//   ray's own origin is whatever you pass in, so a character asking about the
//   ground under itself is never the stale half.
//
// The query and the hit, RayQuery and RayHit, are declared below Object: both
// hold a Handle, and a Handle needs Object complete.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// What spawn() takes. The order is the order it gets written in.
// ---------------------------------------------------------------------------

// The starting values Scene::spawn() gives a new object. Each field sets the
// Object field of the same name, except `size`, which is a shorter way to
// write `shape`:
//
//     auto &wall = spawn({ .position = { 400, 225 }, .size = { 20, 200 } });
struct ObjectOptions {
    Vector2 position{}; // THE CENTRE, in world units
    Vector2 size{}; // shorthand for a RECTANGLE shape of this size
    Shape shape{}; // the full form; used instead of `size` when set
    Vector2 velocity{}; // world units per second
    Vector2 scale{ 1, 1 }; // multiplies the shape, the collider and the sprite
    float rotation = 0; // degrees, as raylib counts them
    int layer = 0; // draw order: a higher layer draws on top
    bool visible = true; // false: not drawn, but still updated and collided
    Edge edges = Edge::NONE; // what happens at the edge of `bounds`
    Rectangle bounds{}; // {0,0,0,0} = the map's bounds, or the view without a map
};

// ---------------------------------------------------------------------------

// An entity in a scene: the player, an enemy, a bullet, a coin, a platform.
// Scene::spawn() makes one and the scene owns it; its public fields are the
// whole of its configuration, read and written directly.
//
// Derive from it to override the hooks (_ready, _update, _draw, _collision,
// _end), add behaviors to it with add(), or hand it functions with on_click(),
// on_drag() and on_collision().
class Object {
public:
    Object() = default;
    // Out of line, in src/rmp/object.cpp, because it has work to do: an object
    // that goes away WITHOUT destroy() -- a stack variable, a member, anything
    // a test builds -- still owns its behaviors, and nothing else would ever
    // free them. Without it they would leak, and the engine would keep a record
    // keyed by memory that had been handed back.
    virtual ~Object();

    // Objects are owned by their scene and referred to by reference and handle.
    // A copy would be a second object that believes it is in the scene.
    Object(const Object &) = delete;
    Object &operator=(const Object &) = delete;

    // ---- transform --------------------------------------------------------
    Vector2 position{}; // THE CENTRE. See the header comment.
    Vector2 scale{ 1, 1 }; // multiplies the shape, the collider and the sprite
    float rotation = 0; // degrees, as raylib counts them
    Vector2 velocity{}; // integrated every frame, by us
    float mass = 1.0f; // only apply_force / apply_impulse read it

    // ---- appearance -------------------------------------------------------
    Sprite sprite; // if there is a sprite, the sprite is drawn
    Shape shape; // otherwise this is
    bool flip_x = false; // mirrors the sprite left to right
    bool flip_y = false; // mirrors the sprite top to bottom
    bool visible = true; // false: not drawn, but still updated and collided
    int layer = 0; // draw order; ties break on creation order

    // ---- world ------------------------------------------------------------
    // 0 means gravity does not touch this object, and that default is not
    // negotiable: a top-down game cannot have things falling over. The scene
    // carrying a downward gravity that nobody uses until they ask is what lets
    // one Object serve a platformer and a Zelda.
    float gravity_scale = 0;
    Edge edges = Edge::NONE; // what happens at the edge of `bounds`
    Rectangle bounds{}; // empty = the map's bounds, or the view without a map

    // What it collides AS. NONE means "the same as `shape`", or the sprite's
    // frame when there is no shape -- which is right until it is not, and the
    // moment it is not is the most common step up in the whole API: a
    // character's sprite includes hair, a cape and air, and nobody wants to
    // collide with the air.
    //
    //     player.sprite.sheet = sheet;                // drawn as the picture
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
    //
    // Whoever is pushed apart loses the part of its velocity going INTO the
    // contact, after the _collision hooks have run -- so a box resting on
    // another starts from rest when the one below moves away, a jump off it
    // keeps its jump, and a hook that flips the velocity to bounce loses
    // nothing.
    //
    // The scene's map counts as ground too: an object that is solid and not
    // immovable is stopped by the map's solid cells as it moves, each axis on
    // its own, so it runs along a floor of tiles without catching on the seams.
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
    //     namespace layer { constexpr unsigned PLAYER = 1 << 0, ENEMY = 1 << 1,
    //                                          BULLET = 1 << 2, WORLD = 1 << 3; }
    //     bullet.collision_layer = layer::BULLET;
    //     bullet.collision_mask  = layer::ENEMY | layer::WORLD;   // not the player
    unsigned collision_layer = 1; // which layers I am ON
    unsigned collision_mask = 1; // which layers I collide WITH

    // ---- what you override, all of it empty by default --------------------
    // Once, when spawn() has put the object in its scene and applied the
    // options: the place to load what it draws and to add its behaviors.
    virtual void _ready() {}
    // Every frame the object is alive, after its behaviors' _update and before
    // it is moved by its velocity. A new object's first _update depends on
    // where in the frame it was spawned: before the object pass -- from the
    // scene's _update or an on_click -- it is the same frame; during the pass
    // -- from an object's or a behavior's _update -- or after it, from a
    // _collision, it is the frame after.
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

    // ---- behaviors ---------------------------------------------------------
    //
    // A behavior is ANY struct with `void _update(rmp::Object &, float)`. It
    // inherits from nothing, has no virtuals and registers nowhere:
    //
    //     struct Spin {
    //         float degrees_per_second = 90;
    //         void _update(rmp::Object &self, float delta) {
    //             self.rotation += degrees_per_second * delta;
    //         }
    //     };
    //     coin.add<Spin>({ .degrees_per_second = 180 });
    //
    // That is the whole model, and the absence of a base class is not taste: a
    // struct with a base and virtual functions stops being an aggregate in
    // C++20, and `{ .speed = 320 }` no longer compiles against it. The usual
    // workaround -- a nested options struct and a constructor taking it -- turns
    // every later read into `get<TopDown>()->opts.speed`. Without inheritance
    // the fields ARE the configuration, hot:
    //
    //     if (auto move = player.get<rmp::behavior::TopDown>()) move->speed = 400;
    //
    // get<B>() is an rmp::Ref: empty when the object has no B, and the `if` is
    // how you ask.
    //
    // And the part that was not aimed for and turns out to be the best of it:
    // one of yours and one of ours are literally the same thing. There is no
    // extension API to learn because there is no extension.
    //
    // The optional hooks are detected at compile time with `requires`, so what
    // you do not declare costs nothing:
    //
    //     _update(Object &, float)            every frame, BEFORE the integrator
    //     _late_update(Object &, float)       every frame, AFTER it
    //
    // One of those two is required and the rest are optional.
    //     _ready(Object &)                    once, when it is added
    //     _draw(Object &)                     after the object is drawn
    //     _collision(Object &, Object &other) once per pair per frame
    //     _end(Object &)                      when removed, or the object dies
    //
    // _late_update is not decoration. Behaviors run before the velocity is
    // integrated, because that is what lets one WRITE velocity -- and a
    // behavior whose job is to correct where the object ENDED UP therefore has
    // nowhere to run. rmp::behavior::GridSnap is exactly that: it exists to
    // leave a puzzle piece square with the grid, and rounding the position the
    // object had before it moved would round the wrong number.
    template <class B> B &add(B value = {});
    template <class B> [[nodiscard]] Ref<B> get() const;
    template <class B> [[nodiscard]] bool has() const {
        return static_cast<bool>(get<B>());
    }
    template <class B> void remove();

    // ---- callbacks, for when a subclass is more than you want --------------
    //
    // The other half of the underscore rule: `_name` is yours and we call it,
    // `on_name` is ours and you hand us a function. They coexist without
    // ambiguity, which is the whole reason the rule exists -- an object can
    // override _collision AND carry an on_collision, and both run.
    //
    //     coin.on_collision([&](rmp::Object &self, rmp::Object &other) {
    //         if (&other == &player) { score += 10; self.destroy(); }
    //     });
    //
    // Setting one twice replaces it. There is no list, because a list of
    // handlers is a signal system, and that is a bigger idea than this needs.
    //
    // on_drag is handed how far the pointer moved since the last frame while
    // it holds this object, in WORLD units -- through the scene's camera, so
    // adding it to the position keeps the object under the finger at any zoom:
    //
    //     piece.on_drag([](rmp::Object &self, Vector2 moved) {
    //         self.position.x += moved.x;
    //         self.position.y += moved.y;
    //     });
    template <class F> void on_click(F &&fn) {
        _click_handler.set(static_cast<F &&>(fn));
    }
    template <class F> void on_drag(F &&fn) { _drag_handler.set(static_cast<F &&>(fn)); }
    template <class F> void on_collision(F &&fn) {
        _collision_handler.set(static_cast<F &&>(fn));
    }

    // ---- identity and life ------------------------------------------------
    // handle() is the plain one and handle<Goblin>() is the typed one, which is
    // what a behavior holding a target wants: `if (!target) return;` and then
    // `target->hp` without a cast.
    template <class T = Object> [[nodiscard]] Handle<T> handle() const {
        return Handle<T>(_index, _generation);
    }
    // The scene that spawned it. An object no scene spawned -- a member, a
    // local, a test's -- has none: asking is said once, and answers
    // Scene::current().
    [[nodiscard]] Scene &scene() const;

    // The axis-aligned box this object occupies right now, from the sprite or
    // the shape. Empty when it has neither, which is what an invisible logic
    // object is. With no `collider` and no `shape`, this box is what the object
    // collides as, and an empty one collides with nothing.
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
    [[nodiscard]] bool alive() const { return _alive; }

private:
    friend class Scene;
    friend struct Storage;

    // The private half the engine reaches, through Storage in
    // src/rmp/object_internal.h and Scene's spawn. Each says what it takes or
    // sets, so no other object ever names these fields. Defined in object.cpp.
    void attach(Scene &scene, unsigned index, unsigned generation, Vector2 at);
    [[nodiscard]] Ref<Scene> spawned_in() const { return _scene; }
    Vector2 take_force();
    [[nodiscard]] Vector2 previous_position() const;
    void remember_position();
    void notify_collision(Object &other);
    void notify_click();
    void notify_drag(Vector2 moved);
    [[nodiscard]] bool has_pointer_callback() const;
    [[nodiscard]] int behavior_slot() const;
    void set_behavior_slot(int slot);
    [[nodiscard]] bool entered_bounds() const;
    void set_entered_bounds(bool entered);

    Callback<Object &> _click_handler;
    Callback<Object &, Vector2> _drag_handler;
    Callback<Object &, Object &> _collision_handler;

    Ref<Scene> _scene; // empty until a scene spawns it
    unsigned _index = 0;
    unsigned _generation = 0;
    // Which record in the behavior engine is this object's, or -1 for "none".
    // The engine used to find it by scanning a list for the object's ADDRESS,
    // which made every lookup O(number of objects with behaviors) -- one per
    // behavior per pass, so a frame grew with the square of the object count --
    // and made a recycled address inherit a dead object's behaviors.
    int _behavior_slot = -1;
    bool _alive = true;
    bool _entered_bounds = false; // Edge::DESTROY fires only after this
    Vector2 _pending_force{}; // accumulated by apply_force, spent on integrate

    // Where the centre was before this frame's integration. The swept test
    // needs it: an object that crossed a thin wall never overlapped it on any
    // frame, so the only evidence it was ever there is the segment it covered.
    Vector2 _previous_position{};
};

// What Scene::raycast() and Scene::raycast_all() test: the segment from `from`
// to `to`, against the scene's objects. The map's tiles are not hit; ask
// Tilemap::solid_at() about those. Down here, below Object, and not with the
// block about raycasting above, because `ignore` is a handle and a handle
// needs Object complete.
struct RayQuery {
    Vector2 from{}; // where the ray starts, in world units
    Vector2 to{}; // where it ends; nothing beyond it is hit
    unsigned mask = 0xFFFFFFFFu; // the same layers as the collision pass
    // Normally whoever is shooting: `.ignore = self.handle()`. Empty ignores
    // nothing, and so does the handle of an object that is gone.
    Handle<Object> ignore{};

    // Only objects that are `solid`. A ground check wants the floor and not the
    // coin lying on it, and a mask cannot say that: layers are about who
    // collides with whom, and solid is about whether the contact resolves.
    // Without this the nearest hit under a character is whatever trigger
    // happens to be there, and the character walks through the floor.
    //
    // Last in the struct, because added before the others it would shift a
    // POSITIONAL initialiser -- `RayQuery{ a, b, mask, self.handle() }` -- onto
    // the wrong fields. A designated one does not mind: it names its fields, in
    // the order they are declared, and a new one in between is simply left out.
    bool solid_only = false;
};

// RayHit lives below Object too, for the same reason.
struct RayHit {
    Handle<Object> object; // what was hit; a handle, so it is safe to keep across frames
    Vector2 point{}; // where, in world coordinates
    Vector2 normal{}; // the surface normal there, pointing back at the ray
    float distance = 0; // from the ray's origin

    // `if (auto hit = raycast(a, b))` is the shape this is for.
    explicit operator bool() const { return static_cast<bool>(object); }
};

// ---------------------------------------------------------------------------
// The behavior templates, down here because they need Object to be complete.
// ---------------------------------------------------------------------------

template <class B> B &Object::add(B value) {
    static_assert(
        requires(B &b, Object &o, float d) { b._update(o, d); } ||
            requires(B &b, Object &o, float d) { b._late_update(o, d); },
        "a behavior needs `void _update(rmp::Object &self, float delta)`, or "
        "`_late_update` with the same signature when its whole job is to "
        "correct where the object ended up (rmp::behavior::GridSnap is the "
        "one in the catalogue). _ready, _draw, _collision and _end are "
        "optional and detected the same way.");
    static_assert(
        !std::is_polymorphic_v<B>,
        "a behavior must not have virtual functions: a struct with them "
        "stops being an aggregate in C++20, and then `add<B>({ .speed = 320 })` "
        "does not compile. Behaviors inherit from nothing on purpose.");
    // Owned from the first line: the engine takes the shared_ptr, and what
    // the caller gets back is a reference into it.
    std::shared_ptr<B> made = std::make_shared<B>(static_cast<B &&>(value));
    void *stored = rmp::detail::attach(*this, rmp::detail::type_id<B>(),
                                       rmp::detail::ops_for<B>(), std::move(made));
    return *static_cast<B *>(stored);
}

template <class B> Ref<B> Object::get() const {
    void *found = rmp::detail::find_behavior(*this, rmp::detail::type_id<B>());
    return found != nullptr ? Ref<B>(*static_cast<B *>(found)) : Ref<B>{};
}

template <class B> void Object::remove() {
    rmp::detail::detach(*this, rmp::detail::type_id<B>());
}

} // namespace rmp
