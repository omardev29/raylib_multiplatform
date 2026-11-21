#pragma once
// One scene per screen, one file per scene under src/scenes/. Going from one
// to the next is rmp::Scene::change<T>(): the scene on screen ends, the next
// one starts, at the end of the frame the button was pressed in.
#include <rmp/scene.h>

// What outlives a screen: the score of the run, and the music switch the
// options change. Plain data of YOURS -- the UI is immediate mode, so it holds
// no state, and there is nothing to keep in sync.
struct Progress {
    int score = 0;
    bool music = true;
};
inline Progress game;

class MainMenuScene : public rmp::Scene {
public:
    void _draw() override;
};

class OptionsScene : public rmp::Scene {
public:
    void _draw() override;
};

class ConfirmScene : public rmp::Scene {
public:
    void _draw() override;
};

// Not everything is a centred menu: the game being played, with its HUD.
class PlayingScene : public rmp::Scene {
public:
    void _update(float delta) override;
    void _draw() override;
};
