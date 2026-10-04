#pragma once
// ---------------------------------------------------------------------------
// What every file of the platformer shares: the collision layers, the run
// that outlives a level, the three scenes, and two helpers for the art.
// ---------------------------------------------------------------------------

#include <rmp/scene.h>

#include <raylib.h>

#include <string>
#include <vector>

namespace game {

constexpr float TILE = 18; // the grid of tiles.png and of every level

// Who notices whom. The player is on its own layer and everything that can
// touch it looks at that layer; nothing else needs to know about anything else.
namespace layer {
constexpr unsigned PLAYER = 1u << 0;
constexpr unsigned ENEMY = 1u << 1;
constexpr unsigned PICKUP = 1u << 2; // coins and keys
constexpr unsigned HAZARD = 1u << 3; // spikes
constexpr unsigned SOLID = 1u << 4; // moving platforms and doors
constexpr unsigned GOAL = 1u << 5;
constexpr unsigned SIGN = 1u << 6;
} // namespace layer

// THE RUN: what survives walking from one level into the next, and dying.
// A level is a scene, and a scene keeps nothing when it changes, so this is an
// rmp::global<Run>() -- and `gone` is what stops a coin collected in the
// Meadow from being there again when the player walks back to it.
struct Run {
    int lives = 3;
    int coins = 0;
    float seconds = 0;
    std::vector<std::string>
        gone; // the iids of coins, keys, enemies and doors that are no more
    std::vector<std::string> keys; // the iids of the doors the player holds a key for
};
Run &run();
void new_run();
bool contains(const std::vector<std::string> &list, const char *iid);

// The best run, as rmp::save keeps it: the most coins, and on a tie the
// faster time. Returns true when this one is a new best (and keeps it).
struct Best {
    int coins = -1; // -1: nobody has finished yet
    float seconds = 0;
};
Best best();
bool keep_if_best(int coins, float seconds);

// A tile of tiles.png by its index, drawn with its top-left corner at `at`.
void draw_tile(int index, Vector2 at, Color tint = WHITE, bool flip_x = false);
// The same tile `scale` times its size, for the HUD.
void draw_icon(int index, Vector2 at, float scale);
// A number in the sheet's own pixel digits, at `scale`, left to right from `at`.
void draw_number(int value, Vector2 at, float scale);
// "73.4" as tiles: the run clock.
void draw_seconds(float seconds, Vector2 at, float scale);

// ---------------------------------------------------------------------------
// The scenes
// ---------------------------------------------------------------------------

class TitleScene : public rmp::Scene {
public:
    void _ready() override;
    void _update(float delta) override;
    void _draw() override;

private:
    Best best_; // read once: a file, not something to ask sixty times a second
    float clock_ = 0;
};

// Where the player comes into a level from: nowhere (a new game, or a level
// started from its Player entity), or another level, at a world position --
// which is the same place in this one, because the levels share the world.
struct Entry {
    bool carried = false;
    Vector2 position{};
    Vector2 velocity{};
};

class LevelScene : public rmp::Scene {
public:
    explicit LevelScene(std::string level, Entry entry = {});
    void _ready() override;
    void _update(float delta) override;
    void _draw() override;

    // Called by the player when it is hit, and by this scene when it falls.
    void lose_life();
    void win();

private:
    void respawn();

    std::string level_;
    Entry entry_;
    Vector2 start_{}; // where a lost life puts the player back
    rmp::Handle<rmp::Object> player_;
    bool over_ = false;
};

// Pushed on top of the level when it ends: the level stays on screen, frozen,
// under it -- the scene stack's defaults, and no code here.
class EndScene : public rmp::Scene {
public:
    EndScene(bool won, bool record);
    void _draw() override;

private:
    bool won_;
    bool record_;
};

} // namespace game
