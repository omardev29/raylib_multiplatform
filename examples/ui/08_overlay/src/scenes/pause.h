#pragma once
#include <rmp/scene.h>

// Every default untouched: pushed, it freezes the game, keeps it on screen
// behind the overlay, and takes the input.
class PauseScene : public rmp::Scene {
public:
    void _draw() override;
};
