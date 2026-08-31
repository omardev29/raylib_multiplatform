// ---------------------------------------------------------------------------
// examples/scenes/01_stack.cpp
//
// The whole scene model in one file: a menu that goes to a game, and a game
// that puts a pause overlay on top of itself.
//
// Three things are worth watching for, because they are what the stack buys
// you and none of them is written here:
//
//   1. PauseScene contains NO POLICY. The defaults are a pause menu — what is
//      underneath freezes, stays visible, and stops receiving input — so
//      pushing it stops the world. It works the same with five enemies as with
//      ten thousand, because what is suspended is the SCENE.
//   2. Nothing in GameScene knows the pause menu exists. There is no
//      `if (paused) return;` at the top of _update, and no `player.freeze()`.
//   3. `Scene::change<T>()` from inside a _draw() is safe. The transition is
//      deferred to the end of the frame, so the scene that asked for it
//      survives the rest of the frame it asked from.
//
// A real project puts one scene per file under src/scenes/, the way Godot does
// — src/ is globbed recursively by all four build systems, so a new file costs
// no CMake, no Gradle and no Xcode edit. They are together here so the example
// reads top to bottom.
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <string>

// Scenes refer to each other in a circle — the pause menu goes back to the main
// menu, the main menu starts the game, the game pauses — so one of them has to
// be announced before it is defined. In a real project this is not a thing you
// think about: pause.cpp includes main_menu.h and that is the end of it. It is
// only visible here because the whole example is one file.
//
// The declaration is not enough on its own: Scene::change<T>() constructs a T,
// so it needs the complete type. That is why PauseScene::_draw() is DEFINED at
// the bottom of this file rather than inside the class — which is, again,
// exactly what a .h and a .cpp do for you.
class MainMenuScene;

// State that has to survive a scene change lives in exactly one place. A scene
// owns its state and loses it on the way out, which is right for the enemies
// and wrong for the score.
struct Progress {
    int best_score = 0;
    int runs = 0;
};

// --- the pause overlay -----------------------------------------------------
//
// Every default untouched. This is the whole file's point: `push<PauseScene>()`
// freezes the game, keeps it on screen behind the menu, and takes the input,
// and not one line below says so.
class PauseScene : public rmp::Scene {
public:
    void _draw() override; // defined at the bottom: it names MainMenuScene
};

// --- the game --------------------------------------------------------------

class GameScene : public rmp::Scene {
public:
    // Constructor arguments cross a transition the way arguments normally
    // cross into an object: Scene::change<GameScene>(3) forwards to here. No
    // std::any, no dictionary, no globals.
    explicit GameScene(int level) : level_(level) {}

    void _ready() override { rmp::global<Progress>().runs++; }

    void _update(float delta) override {
        // No pause check. When PauseScene is on top, this is not called at all.
        elapsed_ += delta;
        score_ = static_cast<int>(elapsed_ * 10.0f);
    }

    void _draw() override {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT });
        rmp::ui::text("Level " + std::to_string(level_));
        // Safe: rmp::ui interns every string it is given into a frame arena, so
        // a temporary like this one outlives the layout that reads it.
        rmp::ui::text("Score " + std::to_string(score_));
        if (rmp::ui::button("Pause")) rmp::Scene::push<PauseScene>();
        rmp::ui::end();
    }

    void _end() override {
        Progress &p = rmp::global<Progress>();
        if (score_ > p.best_score) p.best_score = score_;
    }

private:
    int level_ = 1;
    int score_ = 0;
    float elapsed_ = 0.0f;
};

// --- the menu --------------------------------------------------------------

class MainMenuScene : public rmp::Scene {
public:
    void _draw() override {
        rmp::ui::begin();
        rmp::ui::text("Scene stack example");

        // change<T>() from inside _draw(), which is the case the deferral
        // exists for: this scene is destroyed by that call, and it is the
        // scene the call is being made from.
        if (rmp::ui::button("Play")) rmp::Scene::change<GameScene>(1);

        const Progress &p = rmp::global<Progress>();
        if (p.runs > 0) {
            rmp::ui::text("Best " + std::to_string(p.best_score),
                          { .color = rmp::ui::ColorRole::MUTED });
        }

#if !defined(PLATFORM_IOS)
        // Inert on iPhone: Apple rejects apps that terminate themselves.
        if (rmp::ui::button("Quit")) rmp::app::quit();
#endif
        rmp::ui::end();
    }
};

// --- and the half of PauseScene that had to wait ---------------------------
//
// In a project with one scene per file this is not deferred at all: it sits in
// pause.cpp, which includes main_menu.h at the top like anything else.
void PauseScene::_draw() {
    // The veil goes through the UI rather than DrawRectangle, so it lands above
    // the game's HUD and below this menu instead of on top of both.
    rmp::ui::panel({ .box = { .grow_x = true, .grow_y = true },
                     .background = Color{ 0, 0, 0, 180 } },
                   [] {});

    rmp::ui::begin();
    rmp::ui::text("Paused");
    if (rmp::ui::button("Resume")) rmp::Scene::pop();
    if (rmp::ui::button("Give up")) rmp::Scene::change<MainMenuScene>();
    rmp::ui::end();
}

// Four lines in a real project, and one of them is this. RMP_GAME opens the
// window from [window] in raylib_multiplatform.toml, enters this scene, and
// writes the entry point for whichever of the fourteen targets you are
// building.
RMP_GAME(MainMenuScene);
