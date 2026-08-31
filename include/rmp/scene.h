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

// <utility> for std::forward and nothing else. Measured on this machine:
// <utility> adds 50 ms to a translation unit, <memory> adds 605. A header every
// scene file includes cannot carry the second one, which is why the three
// navigation functions below hand over a raw pointer.
#include <utility>

namespace rmp {

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
        detail_change(new T(std::forward<A>(args)...));
    }

    // Put one on top. What was there is suspended, not ended.
    template <class T, class... A> static void push(A &&...args) {
        detail_push(new T(std::forward<A>(args)...));
    }

    // Swap the top one only. What is underneath is untouched and stays
    // suspended — this is level 3 becoming level 4 without disturbing the
    // pause menu that put you there.
    template <class T, class... A> static void replace(A &&...args) {
        detail_replace(new T(std::forward<A>(args)...));
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
    static void detail_change(Scene *next);
    static void detail_push(Scene *next);
    static void detail_replace(Scene *next);
};

} // namespace rmp
