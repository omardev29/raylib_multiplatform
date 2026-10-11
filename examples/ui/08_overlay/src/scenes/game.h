#pragma once
#include <rmp/scene.h>

class GameScene : public rmp::Scene {
public:
    void _ready() override;
    void _draw() override;

private:
    bool _asking = false; // "Really quit?" is on screen
};
