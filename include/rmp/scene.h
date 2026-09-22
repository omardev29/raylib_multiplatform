#pragma once
// ---------------------------------------------------------------------------
// rmp/scene.h — a scene, and the navigation between scenes.
//
// A scene is a self-contained context of state, update and presentation that
// can be entered, left, suspended and resumed. A main menu is a scene, a level
// is a scene, a pause overlay is a scene.
//
//     class MainMenuScene : public rmp::Scene {
//         void _draw() override {
//             rmp::ui::begin();
//             if (rmp::ui::button("Play")) rmp::Scene::change<GameScene>();
//             if (rmp::ui::button("Quit")) rmp::app::quit();
//             rmp::ui::end();
//         }
//     };
//
// THE UNDERSCORE IS THE ACCESS RULE, and it is worth knowing because it holds
// across the whole framework: `_name` is a method on YOUR type that WE call.
// You override it and never call it. (`on_name` is the other half — a function
// you hand us — and it turns up from phase 6 onwards.)
//
// The navigation functions are static because navigation is a service, not a
// property of any one scene: `rmp::Scene::change<GameScene>()` reads the same
// from inside a scene, from a callback, or from anywhere else. That the same
// class is both the base to inherit and the service to call is deliberate, and
// it is what makes both spellings in the design docs compile.
// ---------------------------------------------------------------------------

#include <raylib.h> // Color, and BLANK for the default background
#include <rmp/config.h>
#include <rmp/object.h> // rmp::Object and rmp::ObjectOptions, for spawn()
#include <rmp/tilemap.h> // rmp::Tilemap -- `map` is a field of every scene

// <utility> for std::forward and nothing else. Measured on this machine:
// <utility> adds 50 ms to a translation unit, <memory> adds 605. A header every
// scene file includes cannot carry the second one, which is why the three
// navigation functions below hand over a raw pointer.
#include <memory> // std::unique_ptr: what spawn() and the transitions hand over
#include <utility>

// <type_traits> for the two static_asserts that make spawn<T>() say something
// useful instead of erroring inside the template. Measured on this machine:
// 12 ms against an empty file's 28 ms baseline, which is to say free.
#include <type_traits>

namespace rmp {

// ---------------------------------------------------------------------------
// The camera. Owned by the scene, because the framing is a property of the
// context and not of any one object. Four things in this framework depend on
// it -- the default `bounds`, rmp::input::pointer() in world units, the hit
// test behind on_click, and rmp::behavior::Parallax -- and they all read it
// from here.
//
//     camera.follow = player.handle();
//     camera.limits = map.bounds();     // never shows the void outside the map
//
// It starts centred on the design resolution ([window] width x height), so a
// scene that never mentions it draws exactly where it always did: at that size
// the camera is the identity. `position` is the CENTRE of what is visible.
//
// The app opens it around the map and the objects and closes it before
// Scene::_draw(), which is why rmp::ui inside _draw() does not drift with the
// view. To draw in world units yourself: BeginMode2D(camera.raylib()).
//
// Two details that are written wrong by hand more often than not:
//   - `limits` win over `follow`: when the player nears the edge of the map
//     the camera stops and the player keeps going, instead of a black strip.
//   - Smoothing and shake are not here yet (phase 11): the camera is pinned to
//     what it follows. When they arrive, shake is a separate offset that decays
//     on its own and never touches `position`.
// ---------------------------------------------------------------------------
class Camera {
public:
    Vector2 position{ APP_WINDOW_WIDTH / 2.0f, APP_WINDOW_HEIGHT / 2.0f };
    float zoom = 1.0f; // 2 = everything twice as big
    float rotation = 0; // degrees, clockwise, like raylib's Camera2D

    // Follow this object: every frame, after it has moved, the camera centres
    // on it. Empty = the camera stays where you left it. A handle, so a target
    // that dies simply stops being followed.
    Handle<Object> follow;

    // Never show outside this rectangle, in world units. One axis at a time:
    // a zero width or a zero height means "unbounded on that axis", so a
    // runner pins y with `{ 0, 0, 0, APP_WINDOW_HEIGHT }` and follows x freely.
    // Empty = no limits. A limit narrower than the view centres the view on it.
    Rectangle limits{};

    // What is visible, in world units, ignoring rotation.
    [[nodiscard]] Rectangle view() const;
    [[nodiscard]] Vector2 to_screen(Vector2 world) const;
    [[nodiscard]] Vector2 to_world(Vector2 screen) const;

    // The raylib camera this is, for BeginMode2D() in your own drawing.
    [[nodiscard]] Camera2D raylib() const;

    // Called by the scene once per frame after the objects have moved: apply
    // `follow`, then `limits`. Public so a test can drive it; not for a game.
    void detail_settle();
};

class Scene {
public:
    Scene() = default;
    virtual ~Scene() = default;

    // Scenes live in the stack and are moved around by pointer. Copying one
    // would give two objects that both think they are in the stack.
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;

    // -----------------------------------------------------------------------
    // What you override. Every one of them has an empty implementation, so a
    // scene that only draws overrides only _draw().
    // -----------------------------------------------------------------------

    // Once, when the scene enters the stack. The window exists by now, so this
    // is where assets load.
    virtual void _ready() {}

    // Once, when it leaves for good. Not called for a suspend — see _suspend.
    //
    // It is `_end` and not `_exit` because exit() is a POSIX function, and a
    // member called exit() in a class the user writes is a name that shadows it
    // in a way nobody enjoys debugging.
    virtual void _end() {}

    // Something was pushed on top of this scene, and later came off again. The
    // scene is still alive and still owns everything it owned.
    virtual void _suspend() {}
    virtual void _resume() {}

    // Every frame, bottom of the stack upwards, skipping whatever the scene
    // above freezes. `delta` is seconds since the last frame.
    virtual void _update(float delta) { (void)delta; }

    // Every frame, in SCREEN SPACE. The app closes the camera before calling
    // this, so rmp::ui inside it cannot drift with the view — a pause menu
    // stays where it is while the player moves. To draw in world coordinates,
    // open the camera yourself for the few lines that need it.
    virtual void _draw() {}

    // -----------------------------------------------------------------------
    // What this scene lets through to whatever is underneath it.
    //
    // The defaults ARE a pause menu: push a scene with none of these touched
    // and the world below freezes, stays visible, and stops receiving input.
    // That is why a pause scene in this framework writes no policy at all.
    // -----------------------------------------------------------------------
    bool updates_below = false; // the scene below keeps running
    bool draws_below = true; // the scene below stays on screen
    bool input_below = false; // the scene below still sees input

    // The colour the frame is cleared with. BLANK means "the theme's", which is
    // what almost every scene wants. Only the LOWEST drawing scene's choice is
    // used: the ones above it are drawn over what is already there.
    Color background = BLANK;

    // The world's gravity, in units per second squared, pointing down the way
    // everyone expects. It belongs to the scene and not to each object because
    // direction and magnitude are a property of the world; each object then
    // says how much of it applies to it, through Object::gravity_scale, which
    // is 0 by default. Nothing falls until something asks to.
    Vector2 gravity{ 0, 980 };

    // The level, designed in Tiled. A field and not something you draw:
    //
    //     map = rmp::assets::load_map("level1.json");
    //
    // and the scene draws it underneath everything, collides against its solid
    // tiles, and `object.bounds` left empty comes to mean THE MAP'S bounds
    // rather than the view's. An empty map costs nothing, and a menu scene
    // simply never touches it.
    //
    // That is what turns Tiled from a parser into the place you design the
    // level: you put the enemies in the editor and they turn up in the game.
    Tilemap map;

    // The framing. See rmp::Camera above; a scene that never touches it draws
    // in screen units, exactly as before it existed.
    Camera camera;

    // -----------------------------------------------------------------------
    // Objects.
    //
    // spawn<T>() default-constructs T, applies the options, puts it in this
    // scene and calls its _ready(). T only has to be default-constructible --
    // there is no `using Object::Object;` and no forwarding constructor to
    // write, which is the usual toll for this pattern. Whatever your type needs
    // goes in its fields or in its _ready().
    //
    // It returns T&, and that reference is good for the whole frame you got it
    // in, because destruction is deferred to the end of the frame:
    //
    //     auto &bullet = spawn({ .position = muzzle });
    //     bullet.velocity = aim * 900;
    //
    // Kept across frames it can dangle. That is what rmp::Handle is for.
    // -----------------------------------------------------------------------
    template <class T = Object> T &spawn(const ObjectOptions &options = {}) {
        static_assert(std::is_base_of_v<Object, T>,
                      "spawn<T>() needs a type derived from rmp::Object");
        static_assert(
            std::is_default_constructible_v<T>,
            "spawn<T>() default-constructs T; give it a default constructor and "
            "put the rest in fields or in _ready()");
        // Owned from the first line: the scene's storage takes the unique_ptr,
        // and what the caller gets back is a reference into it.
        std::unique_ptr<T> made = std::make_unique<T>();
        T &ref = *made;
        detail_spawn(std::move(made), options);
        return ref;
    }

    // Same as object.destroy(). Deferred: it stops updating and drawing at
    // once, and the memory goes after the frame. Twice is harmless.
    void destroy(Object &object);

    // How many live objects this scene has. Objects destroyed earlier in this
    // frame are already not counted.
    [[nodiscard]] int object_count() const;

    // -----------------------------------------------------------------------
    // Raycasting. See the block above RayHit in rmp/object.h for what it is for
    // and why it walks the collision grid instead of the object list.
    //
    //     if (auto hit = raycast(muzzle, muzzle + aim * 400)) { ... }
    //     auto hit = raycast({ .from = muzzle, .to = target,
    //                          .mask = layer::kEnemy, .ignore = &self });
    // -----------------------------------------------------------------------
    [[nodiscard]] RayHit raycast(Vector2 from, Vector2 to) const;
    [[nodiscard]] RayHit raycast(const RayQuery &query) const;

    // Every hit along the ray, nearest first, up to `max`. Returns how many
    // were written. Everything the ray passes through, for a piercing shot or a
    // line of sight that has to know what is in the way.
    int raycast_all(const RayQuery &query, RayHit *out, int max) const;

    // -----------------------------------------------------------------------
    // Navigation. All of it is DEFERRED: these record what to do and return,
    // and the change happens at the end of the frame.
    //
    // That is not an optimisation. Half of these calls happen from inside the
    // very scene that is about to be destroyed — `if (button("Play"))
    // change<GameScene>()` sits in the _draw() of the menu that disappears.
    // Destroying it there is pulling the floor out from under the call stack,
    // and it is a bug that sometimes goes unnoticed on desktop and never does
    // on Android.
    //
    // Constructor arguments are forwarded, so data crosses a transition the way
    // data normally crosses into an object:
    //
    //     rmp::Scene::change<GameScene>(3, Difficulty::HARD);
    //
    // No std::any, no dictionary, no globals. If you want designated
    // initialisers, give the struct a name — change<GameScene>(GameScene::Start{
    // .level = 3 }) — because a braced list cannot deduce a type through a
    // variadic template. That is C++, not a decision of ours.
    // -----------------------------------------------------------------------

    // Clear the stack and go. Everything on it gets _end(), top down.
    template <class T, class... A> static void change(A &&...args) {
        detail_change(std::make_unique<T>(std::forward<A>(args)...), scene_type<T>());
    }

    // Put one on top. What was there is suspended, not ended.
    template <class T, class... A> static void push(A &&...args) {
        detail_push(std::make_unique<T>(std::forward<A>(args)...), scene_type<T>());
    }

    // Swap the top one only. What is underneath is untouched and stays
    // suspended — this is level 3 becoming level 4 without disturbing the
    // pause menu that put you there.
    template <class T, class... A> static void replace(A &&...args) {
        detail_replace(std::make_unique<T>(std::forward<A>(args)...), scene_type<T>());
    }

    // Take the top one off and resume what was under it. Popping the last
    // scene is refused with a warning rather than leaving the app with nothing
    // to draw: on desktop that is a black window, and on iOS there is no way
    // to get back. Call rmp::app::quit() if leaving is what you meant.
    static void pop();

    // The top of the stack. Never null while the app is running.
    static Scene &current();

    // How many scenes are on the stack.
    static int depth();

private:
    // The non-template halves, so that the templates above stay three lines
    // and the stack itself is compiled once, in src/rmp/scene.cpp.
    //
    // Each TAKES OWNERSHIP of `next`. The pointer is created by the template
    // above and handed over on the same line, so it is never something a caller
    // holds — and src/rmp/scene.cpp wraps it in a unique_ptr on arrival, where
    // <memory> costs nothing.
    // The type tag is what lets the same scene asked for twice in one frame --
    // two end conditions firing together, which Invaders did -- be pushed
    // once. An address of a static per T, no RTTI, like behavior_type().
    template <class T> static const void *scene_type() {
        static const char kTag = 0;
        return &kTag;
    }
    static void detail_change(std::unique_ptr<Scene> next, const void *type);
    static void detail_push(std::unique_ptr<Scene> next, const void *type);
    static void detail_replace(std::unique_ptr<Scene> next, const void *type);

    // The non-template half of spawn(). TAKES OWNERSHIP of `made`.
    void detail_spawn(std::unique_ptr<Object> owned, const ObjectOptions &options);
};

} // namespace rmp
