#pragma once
#include <rmp/scene.h>

// What the options change, and what outlives the screen that changed it.
struct Options {
    bool hints = true;
    float text_speed = 0.5f;
    int difficulty = 1;
};
inline Options options;

class TitleScene : public rmp::Scene {
public:
    void _ready() override;
    void _draw() override;
};

class OptionsScene : public rmp::Scene {
public:
    void _update(float delta) override;
    void _draw() override;
};

class CreditsScene : public rmp::Scene {
public:
    void _update(float delta) override;
    void _draw() override;
};
