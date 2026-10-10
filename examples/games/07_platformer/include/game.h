#pragma once
// ---------------------------------------------------------------------------
// What every file of the platformer shares: the collision layers, the run
// that outlives a level, the three scenes, and two helpers for the art.
// ---------------------------------------------------------------------------

#include <rmp/scene.h>

#include <raylib.h>

#include <string>
#include <string_view>
#include <vector>

namespace game {

constexpr float TILE = 18; // the grid of tiles.png and of every level

// How much of the world is on screen: twelve and a half rows of tiles, whatever
// the window's height. The camera zooms to fit it.
constexpr float VIEW_HEIGHT = 225;

// The tiles of tiles.png this game draws by hand, by index (20 to a row).
namespace tiles {
constexpr int KEY = 27;
constexpr int DOOR = 28;
constexpr int HEART = 44;
constexpr int HEART_EMPTY = 46;
constexpr int PLATFORM_LEFT = 48;
constexpr int PLATFORM_MIDDLE = 49;
constexpr int PLATFORM_RIGHT = 50;
constexpr int SPIKES = 68;
constexpr int SIGN = 86;
constexpr int SIGN_LEFT = 87;
constexpr int SIGN_RIGHT = 88;
constexpr int FLAG = 111; // and 112, the other half of its wave
constexpr int POLE = 131;
constexpr int COIN = 151;
constexpr int POINT = 157; // the decimal point
constexpr int TIMES = 158; // the x in "x 12"
constexpr int DIGIT_0 = 160; // to 169
} // namespace tiles

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
    // The iids of the coins, keys, enemies and doors that are no more.
    std::vector<std::string> gone;
    // The iids of the doors the player holds a key for.
    std::vector<std::string> keys;
};
Run &run();
void new_run();
bool contains(const std::vector<std::string> &list, std::string_view iid);

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
    Best _best; // read once: a file, not something to ask sixty times a second
    float _clock = 0;
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

    std::string _level;
    Entry _entry;
    Vector2 _start{}; // where a lost life puts the player back
    rmp::Handle<rmp::Object> _player;
    bool _over = false;
};

// Pushed on top of the level when it ends: the level stays on screen, frozen,
// under it -- the scene stack's defaults, and no code here.
class EndScene : public rmp::Scene {
public:
    EndScene(bool won, bool record);
    void _draw() override;

private:
    bool _won;
    bool _record;
};

} // namespace game
