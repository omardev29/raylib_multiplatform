#pragma once
// Your first scene. One scene per file, one object per file — the same layout
// Godot teaches, and the reason src/ is globbed recursively by all four build
// systems: adding src/scenes/game.cpp touches no CMake, no Gradle and no Xcode
// project.
//
// This one is a start menu and nothing else, on purpose: it is the first thing
// anybody reads. Your game grows from here -- more scenes beside it, the things
// in them under src/objects/.

#include <rmp/assets.h>
#include <rmp/scene.h>

class MainMenuScene : public rmp::Scene {
public:
    void _ready() override;
    void _draw() override;

private:
    // No Unload anywhere, and no destructor: rmp::Texture releases itself when
    // the scene goes.
    rmp::Texture _rabbit;
};
