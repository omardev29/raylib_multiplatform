#pragma once
// ---------------------------------------------------------------------------
// What every file of the dungeon shares: the collision layers, the run that
// outlives a level, the scenes, and the look.
// ---------------------------------------------------------------------------

#include <rmp/scene.h>

#include <raylib.h>

#include <string>
#include <string_view>

namespace game {

inline constexpr float TILE = 16; // the grid of tiles.png and of every level

// How much of the world is on screen: twelve and a half rows of tiles at the
// [window] size, each pixel of the art several of the screen's. A window of
// another shape shows more of the dungeon (Stretch::EXPAND), never less.
inline constexpr float VIEW_HEIGHT = 200;
inline constexpr float ZOOM = RMP_WINDOW_HEIGHT / VIEW_HEIGHT;

// The tiles of tiles.png the game draws by itself, by index (12 to a row).
namespace tiles {
inline constexpr int GEM = 102;
inline constexpr int RUNE = 101;
inline constexpr int POTION = 115;
inline constexpr int CHEST = 89;
inline constexpr int CHEST_OPEN = 91;
inline constexpr int LEVER = 80;
inline constexpr int LEVER_PULLED = 79;
inline constexpr int GATE = 45;
inline constexpr int ALTAR = 64;
inline constexpr int SPIKES = 41;
inline constexpr int BLADE = 103;
inline constexpr int PILLAR = 65;
inline constexpr int BRAZIER = 29;
inline constexpr int STAIRS = 56;
inline constexpr int PORTAL = 60;
inline constexpr int SWORD = 104;
inline constexpr int BOLT = 125;
} // namespace tiles

// Who notices whom.
namespace layer {
inline constexpr unsigned HERO = 1u << 0;
inline constexpr unsigned MONSTER = 1u << 1;
inline constexpr unsigned PICKUP = 1u << 2; // gems, potions, runes
inline constexpr unsigned TRAP = 1u << 3; // spikes, blades, bolts
inline constexpr unsigned PLACE = 1u << 4; // chests, levers, altars, stairs
inline constexpr unsigned WALL = 1u << 5; // the gate
inline constexpr unsigned SWING = 1u << 6; // the hero's sword
} // namespace layer

// THE RUN: what survives going down the stairs into the next level.
struct Run {
    int hp = 6;
    int max_hp = 6;
    int gems = 0;
    float seconds = 0;
};
Run &run();
void new_run();

// The best run, as rmp::save keeps it: the most gems.
int best_gems();
bool keep_if_best(int gems);

// The look of every screen: Kenney's Ancient frames, his pixel font, sounds
// on hover and click. Set once, by the title.
void set_look();

// A tile of tiles.png, centred on `at`, in the world.
void draw_tile(int index, Vector2 at, Color tint = WHITE, float scale = 1, float rotation = 0);
// A glow of `radius` around `at`, added to what is under it.
void draw_glow(Vector2 at, float radius, Color color);

// ---------------------------------------------------------------------------
// The scenes
// ---------------------------------------------------------------------------

class TitleScene : public rmp::Scene {
public:
    void _ready() override;
    void _update(float delta) override;
    void _draw() override;

private:
    float _clock = 0;
    int _best = -1;
};

class LevelScene : public rmp::Scene {
public:
    explicit LevelScene(std::string level);
    void _ready() override;
    void _update(float delta) override;
    void _draw() override;

    void hero_died();
    void go_down(const std::string &level);
    void win();
    // A line across the top of the screen for a moment: a room's name, a gate.
    void announce(std::string_view text);

private:
    void build();

    std::string _level;
    rmp::Handle<rmp::Object> _hero;
    std::string _banner;
    float _banner_left = 0;
    bool _over = false;
};

class PauseScene : public rmp::Scene {
public:
    void _update(float delta) override;
    void _draw() override;
};

class EndScene : public rmp::Scene {
public:
    EndScene(bool won, bool record);
    void _draw() override;

private:
    bool _won;
    bool _record;
};

} // namespace game
