#pragma once
#include <rmp/scene.h>

// Every default untouched. This is the whole example's point:
// push<PauseScene>() freezes the game, keeps it on screen behind the menu, and
// takes the input -- and not one line in this class says so.
class PauseScene : public rmp::Scene {
public:
    void _draw() override;
};
