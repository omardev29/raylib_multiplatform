#pragma once
// Your first scene. One scene per file, one object per file — the same layout
// Godot teaches, and the reason src/ is globbed recursively by all four build
// systems: adding src/scenes/game.cpp touches no CMake, no Gradle and no Xcode
// project.
//
// This one is a start menu and nothing else, on purpose. It is the first thing
// anybody reads, so every mechanic added here is a mechanic between a newcomer
// and the point. What the framework grows next gets an example under examples/.

#include <rmp/assets.h>
#include <rmp/scene.h>

class MainMenuScene : public rmp::Scene {
public:
    void _ready() override;
    void _draw() override;

private:
    // No Unload anywhere, and no destructor. rmp::Texture releases itself when
    // the scene goes, which is the whole of what phase 3 bought: this used to
    // be an Image, a Texture2D and two calls in an on_exit() that had to stay
    // paired with them.
    rmp::Texture rabbit;
};
