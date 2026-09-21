#pragma once
#include <rmp/scene.h>

class GameScene : public rmp::Scene {
public:
    // Constructor arguments cross a transition the way arguments normally
    // cross into an object: Scene::change<GameScene>(3) forwards to here. No
    // std::any, no dictionary, no globals.
    explicit GameScene(int level) : level_(level) {}

    void _ready() override;
    void _update(float delta) override;
    void _draw() override;
    void _end() override;

private:
    int level_ = 1;
    int score_ = 0;
    float elapsed_ = 0.0f;
};
