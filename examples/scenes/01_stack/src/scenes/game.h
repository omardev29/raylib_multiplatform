#pragma once
#include <rmp/scene.h>

class GameScene : public rmp::Scene {
public:
    // Constructor arguments cross a transition the way arguments normally
    // cross into an object: Scene::change<GameScene>(3) forwards to here. No
    // std::any, no dictionary, no globals.
    explicit GameScene(int level) : _level(level) {}

    void _ready() override;
    void _update(float delta) override;
    void _draw() override;
    void _end() override;

private:
    int _level = 1;
    int _score = 0;
    float _elapsed = 0.0f;
};
