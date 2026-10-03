#pragma once
// ---------------------------------------------------------------------------
// Everything that moves, hurts or can be picked up. Each one is placed in
// LDtk and built from its rmp::MapObject by a factory in src/scenes/level.cpp;
// what it does when the player touches it is its own _collision().
// ---------------------------------------------------------------------------

#include <rmp/object.h>

#include <raylib.h>

#include <string>
#include <vector>

namespace game {

class Player : public rmp::Object {
public:
    void _ready() override;
    void _update(float delta) override;

    // A life, a knock away from `from_x`, and a moment of grace.
    void hurt(float from_x);
    // Off an enemy's head.
    void bounce();
    // Coming down onto the top of `other`: a stomp rather than a bump.
    [[nodiscard]] bool falling_onto(const rmp::Object &other) const;
    [[nodiscard]] bool hurting() const { return knocked_ > 0; }

private:
    float grace_ = 0; // seconds of not being hurt again
    float knocked_ = 0; // seconds without control after a hit
    bool was_on_ground_ = true;
};

// What walks and flies. The same rules for both when the player touches one:
// from above it is stomped, from anywhere else it hurts.
class Enemy : public rmp::Object {
public:
    std::string iid;
    bool stompable = true;
    void _collision(rmp::Object &other) override;
    void _update(float delta) override;

protected:
    virtual void patrol(float delta) = 0;
    virtual const char *squashed_tag() const { return nullptr; }

private:
    float dying_ = -1; // seconds left of the squashed frame, -1 = alive
};

// Back and forth along its floor, between the x of its first and last patrol
// Points. The floor is where LDtk put it; the Points say where to turn.
class Walker : public Enemy {
public:
    float speed = 40;
    float left = 0;
    float right = 0;
    void _ready() override;

protected:
    void patrol(float delta) override;
    const char *squashed_tag() const override { return "squash"; }

private:
    int direction_ = -1;
};

// Along the whole route of its Points, there and back again.
class Bat : public Enemy {
public:
    float speed = 50;
    std::vector<Vector2> route;
    void _ready() override;

protected:
    void patrol(float delta) override;

private:
    std::size_t next_ = 1;
    int step_ = 1;
};

// From where LDtk put it to its `to` Point and back, carrying whoever stands
// on it. Solid and immovable: the player lands on it like on the floor.
class Platform : public rmp::Object {
public:
    void _ready() override;
    Vector2 from{};
    Vector2 to{};
    float speed = 40;
    rmp::Handle<rmp::Object> rider; // the only one who can stand on it
    void _update(float delta) override;
    void _draw() override;

private:
    float travelled_ = 0;
    int direction_ = 1;
    float waiting_ = 0; // at either end, long enough to get on or off
};

class Coin : public rmp::Object {
public:
    void _ready() override;
    std::string iid;
    int value = 1;
    void _collision(rmp::Object &other) override;
};

// A key opens the door its `opens` field names -- an EntityRef in LDtk, an
// iid here, compared against the door's own.
class Key : public rmp::Object {
public:
    void _ready() override;
    std::string iid;
    std::string opens;
    void _update(float delta) override;
    void _draw() override;
    void _collision(rmp::Object &other) override;

private:
    float clock_ = 0;
};

class Door : public rmp::Object {
public:
    void _ready() override;
    std::string iid;
    void _draw() override;
    void _collision(rmp::Object &other) override;
};

class Spikes : public rmp::Object {
public:
    void _ready() override;
    void _draw() override;
    void _collision(rmp::Object &other) override;
};

// A wooden sign that says something while the player stands near it.
class Sign : public rmp::Object {
public:
    void _ready() override;
    std::string text;
    int tile = 86;
    void _update(float delta) override;
    void _draw() override;
    void _collision(rmp::Object &other) override;

private:
    float near_ = 0;
};

class Goal : public rmp::Object {
public:
    void _ready() override;
    void _draw() override;
    void _collision(rmp::Object &other) override;
};

} // namespace game
