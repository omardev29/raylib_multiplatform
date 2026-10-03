// ===========================================================================
// examples/games/07_platformer, PLAYED -- through its real loop, by a bot.
//
// "A game in examples/games/ is playable from start to finish -- win, lose,
// play again -- or it does not belong there." Booting it for thirty frames
// proves the title screen draws. This proves the rest: the keys go in
// through rmp::input's seam and the UI's (they are two -- CLAUDE.md says why),
// the scenes, factories, collisions and the map do the rest, and the bot
// reads only what a player can see: where things are.
//
//   1. A whole game at 60 Hz: Play from the keyboard, the Meadow, the Desert's
//      moving platform, its key and door, the Snowfield's lift, the flag; a
//      best run kept on disk; Play again; three falls into the first pit;
//      Game over; Menu, and the title showing the best run.
//   2. A jump from the moving platform at 60 Hz and at 240 Hz. The first
//      review found the platform carrying its rider back down every frame a
//      jump moved it less than three pixels -- every jump, above ~127 FPS,
//      and the desktop build runs uncapped. The playthrough never jumps from
//      that platform, so it is asked for on its own.
//   3. A whole game to the flag again at 240 Hz, because a game that can be
//      won at one frame rate and not another is not finished either.
//
// The route through the Desert (up two platforms to the key and back) is
// written for the level as it is. When world.ldtk is redesigned in LDtk the
// route may need updating, and the failure says where the player got stuck.
// tests/ldtk_test.cpp checks the level's structure on its own, so a broken
// level and a stale route are told apart.
//
// Built with the examples (CMakeLists.txt, target platformer_play) and run by
// tools/examples_build.sh: `just test examples` here, the examples job in CI.
// It prints PLAY PASS or PLAY FAIL and the reason.
// ===========================================================================

#include <rmp/app.h>
#include <rmp/behavior.h>
#include <rmp/input.h>
#include <rmp/save.h>
#include <rmp/scene.h>

#include "game.h"
#include "object_internal.h"
#include "objects.h"
#include "scene_internal.h"
#include "ui/internal.h"

#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace {

// ---- the devices --------------------------------------------------------------

rmp::input::detail::DeviceState g_dev;
// The seams' signatures, which write through `out`.
void sample(rmp::input::detail::DeviceState *out) { *out = g_dev; }

bool g_enter_was = false;
void navigate(rmp::ui::detail::NavState *out) {
    out->x = 0;
    out->y = g_dev.keys[KEY_DOWN] ? 1 : (g_dev.keys[KEY_UP] ? -1 : 0);
    out->activate = g_dev.keys[KEY_ENTER] && !g_enter_was;
    g_enter_was = g_dev.keys[KEY_ENTER];
}

void release_all() { g_dev = rmp::input::detail::DeviceState{}; }

// ---- what the bot can see --------------------------------------------------------

template <class T> T *top() { return dynamic_cast<T *>(&rmp::Scene::current()); }

template <class T> T *first(rmp::Scene &s) {
    for (rmp::Object *o : rmp::objects::detail::live_objects(s)) {
        if (auto *t = dynamic_cast<T *>(o)) return t;
    }
    return nullptr;
}

const game::Platform *platform_under(rmp::Scene &s, float x, float y) {
    for (rmp::Object *o : rmp::objects::detail::live_objects(s)) {
        const auto *p = dynamic_cast<game::Platform *>(o);
        if (p == nullptr) continue;
        const Rectangle r = p->world_collider();
        if (x >= r.x && x <= r.x + r.width && y >= r.y - 3 && y <= r.y + r.height)
            return p;
    }
    return nullptr;
}

template <class T> bool any_in(rmp::Scene &s, Rectangle area) {
    return std::ranges::any_of(
        rmp::objects::detail::live_objects(s), [&area](rmp::Object *o) {
            return dynamic_cast<T *>(o) != nullptr && o->collision_layer != 0 &&
                CheckCollisionRecs(area, o->world_collider());
        });
}

// Ground to land on within a jump ahead: a solid cell with air above it, no
// higher than a jump, or a platform.
bool landable_ahead(rmp::Scene &s, float from, float feet, int dir) {
    for (int dx = 0; dx <= 80; dx += 3) {
        const float x = from + static_cast<float>(dir * dx);
        for (int above = -54; above <= 90; above += 18) {
            const float row = std::floor((feet + static_cast<float>(above)) / 18) * 18;
            if (row >= feet - 54 && s.map.solid_at({ x, row + 9 }) &&
                !s.map.solid_at({ x, row - 9 })) {
                return true;
            }
        }
        if (platform_under(s, x, feet + 2) != nullptr) return true;
    }
    return false;
}

// ---- the player's hands -------------------------------------------------------------

struct Hands {
    int dir = 0;
    bool jump = false;
};
bool g_jump_was = false;
int g_route = 0; // where in the Desert's detour to the key

Hands steer(game::LevelScene &scene, game::Player &p) {
    Hands h{ 1, false };
    const bool ground = p.get<rmp::behavior::Platformer>()->on_ground();
    const Rectangle box = p.world_collider();
    const float feet = box.y + box.height;
    const Rectangle bounds = scene.map.bounds();
    const auto press_jump = [&h] { h.jump = !g_jump_was; };

    // THE DESERT'S KEY, which is the one thing that is not straight ahead:
    // up to the platform at row 10, left and up to the one at row 7 where the
    // key floats, then down and on to the door. Level coordinates.
    if (std::string(scene.map.level()) == "Desert" && game::run().keys.empty()) {
        const float x = p.position.x - bounds.x;
        if (g_route == 0 && ground && feet > 230 && x > 560) g_route = 1;
        if (g_route == 1) {
            if (x < 586) {
                h.dir = 1;
            } else if (x > 598) {
                h.dir = -1;
            } else if (ground) {
                press_jump();
                g_route = 2;
            }
        } else if (g_route == 2) {
            if (ground && !h.jump)
                g_route = std::fabs(feet - 180) < 3 ? 3 : (feet > 230 ? 1 : 2);
        } else if (g_route == 3) {
            h.dir = -1;
            if (x <= 621 && ground) {
                press_jump();
                g_route = 4;
            }
        } else if (g_route == 4) {
            h.dir = -1;
            if (ground && !h.jump)
                g_route = std::fabs(feet - 180) < 3 ? 3 : (feet > 230 ? 1 : 4);
        }
        if (g_route > 0) return h;
    }

    const float front = box.x + box.width + 1;
    const bool beyond = front > bounds.x + bounds.width;
    const bool on_platform = platform_under(scene, p.position.x, feet + 1) != nullptr;
    const bool ground_ahead = scene.map.solid_in({ front, feet + 1, 4, 4 }) ||
        platform_under(scene, front, feet + 1) != nullptr ||
        (beyond && scene.map.neighbour_at({ front, feet - 2 })[0] != '\0');
    const Rectangle body{ front, box.y + 2, 4, box.height - 4 };
    const bool wall_ahead = scene.map.solid_in(body) || any_in<game::Door>(scene, body);
    const bool enemy_ahead =
        any_in<game::Enemy>(scene, { front, box.y - 10, 34, box.height + 10 });
    const bool spikes_ahead = any_in<game::Spikes>(scene, { front, feet - 10, 26, 12 });

    if (on_platform && ground) {
        // Ride, and get off only where there is ground right there.
        h.dir = scene.map.solid_in({ front, feet - 80, 64, 120 }) ? 1 : 0;
    } else if (ground) {
        if (!ground_ahead) {
            if (landable_ahead(scene, front + 18, feet, 1)) {
                press_jump();
            } else {
                h.dir = 0; // wait for a platform
            }
        } else if (wall_ahead || enemy_ahead || spikes_ahead) {
            press_jump();
        }
    }
    return h;
}

void apply(const Hands &h) {
    release_all();
    if (h.dir > 0) g_dev.keys[KEY_RIGHT] = true;
    if (h.dir < 0) g_dev.keys[KEY_LEFT] = true;
    g_dev.keys[KEY_SPACE] = h.jump;
    g_jump_was = h.jump;
}

// Down `downs` times, then Enter: a player without a mouse. The first button
// of a screen has the focus already.
void press_button(int t, int downs) {
    release_all();
    if (t < 0) return;
    const int step = t / 4;
    const bool on = t % 4 == 0;
    if (step < downs)
        g_dev.keys[KEY_DOWN] = on;
    else if (step == downs)
        g_dev.keys[KEY_ENTER] = on;
}

// ---- the missions ------------------------------------------------------------------------

enum class Phase {
    TITLE,
    PLAY_TO_WIN,
    WON,
    PLAY_TO_LOSE,
    LOST,
    JUMP_SETUP,
    JUMP_MEASURE,
    FAST_SETUP,
    FAST_PLAY,
    DONE,
};

Phase g_phase = Phase::TITLE;
int g_t = 0; // frames in this phase
float g_dt = 1.0f / 60;
int g_failures = 0;
std::string g_level_seen;
int g_jump_rate = 0; // which of the two frame rates the jump check is at
float g_platform_top = 0;
float g_highest_feet = 0;

void fail(const std::string &why) {
    std::printf("PLAY FAIL %s\n", why.c_str());
    g_failures++;
}

void next(Phase p) {
    g_phase = p;
    g_t = 0;
    g_route = 0;
    g_jump_was = false;
    release_all();
}

std::string where() {
    auto *level = top<game::LevelScene>();
    if (level == nullptr) return "(not in a level)";
    auto *p = first<game::Player>(*level);
    char text[160];
    std::snprintf(text, sizeof text, "in %s at (%.0f, %.0f), lives %d, coins %d, %.1f s",
                  level->map.level(), p != nullptr ? p->position.x : -1.0f,
                  p != nullptr ? p->position.y : -1.0f, game::run().lives,
                  game::run().coins, game::run().seconds);
    return text;
}

// Drives a level towards the flag (or, `fall`, straight into the first pit).
void play(bool fall) {
    auto *level = top<game::LevelScene>();
    if (level == nullptr) return;
    auto *p = first<game::Player>(*level);
    if (p == nullptr) return;
    const std::string name = level->map.level();
    if (name != g_level_seen) {
        std::printf("PLAY  %s\n", where().c_str());
        g_level_seen = name;
        g_route = 0;
    }
    apply(fall ? Hands{ 1, false } : steer(*level, *p));
}

void step() {
    g_t++;
    switch (g_phase) {
        case Phase::TITLE:
            press_button(g_t - 30, 0); // Play
            if (top<game::LevelScene>() != nullptr) next(Phase::PLAY_TO_WIN);
            if (g_t > 200) {
                fail("Enter on the title did not start a game");
                next(Phase::DONE);
            }
            break;
        case Phase::PLAY_TO_WIN:
        case Phase::FAST_PLAY:
            play(false);
            if (top<game::EndScene>() != nullptr) {
                release_all();
                const bool fast = g_phase == Phase::FAST_PLAY;
                std::printf("PLAY  the run ended %s: lives %d, coins %d, %.1f s\n",
                            fast ? "at 240 Hz" : "at 60 Hz", game::run().lives,
                            game::run().coins, game::run().seconds);
                if (game::run().lives <= 0)
                    fail("the bot lost every life on the way to the flag");
                next(fast ? Phase::DONE : Phase::WON);
            } else if (static_cast<float>(g_t) * g_dt > 180) {
                fail("no flag after three minutes of play, " + where());
                next(Phase::DONE);
            }
            break;
        case Phase::WON: {
            if (g_t == 1) {
                const game::Best b = game::best();
                if (b.coins != game::run().coins)
                    fail("the best run was not kept on disk");
            }
            press_button(g_t - 30, 0); // Play again
            auto *level = top<game::LevelScene>();
            if (level != nullptr && g_t > 30) {
                if (game::run().lives != 3 || game::run().coins != 0)
                    fail("Play again kept the old run");
                g_level_seen.clear();
                next(Phase::PLAY_TO_LOSE);
            } else if (g_t > 200) {
                fail("Play again did not start a game");
                next(Phase::DONE);
            }
            break;
        }
        case Phase::PLAY_TO_LOSE:
            play(true);
            if (top<game::EndScene>() != nullptr) {
                if (game::run().lives != 0) fail("the game ended with lives left");
                next(Phase::LOST);
            } else if (g_t > 60 * 60) {
                fail("three falls into the first pit did not end the game, " + where());
                next(Phase::DONE);
            }
            break;
        case Phase::LOST:
            press_button(g_t - 30, 1); // Menu
            if (top<game::TitleScene>() != nullptr) {
                std::printf("PLAY  game over, and back at the title\n");
                next(Phase::JUMP_SETUP);
            } else if (g_t > 200) {
                fail("Menu did not go back to the title");
                next(Phase::DONE);
            }
            break;
        case Phase::JUMP_SETUP:
            if (g_t == 2) {
                // Straight onto the Desert's platform, at its start, where it
                // waits: the platform's left cell is (9, 11), so its middle is
                // x = 189 in the level, 909 in the world, and its top y = 198.
                g_dt = g_jump_rate == 0 ? 1.0f / 60 : 1.0f / 240;
                game::new_run();
                rmp::Scene::change<game::LevelScene>(
                    "Desert", game::Entry{ true, { 909, 186 }, { 0, 0 } });
            }
            if (g_t > 2 && top<game::LevelScene>() != nullptr) next(Phase::JUMP_MEASURE);
            break;
        case Phase::JUMP_MEASURE: {
            auto *level = top<game::LevelScene>();
            game::Player *p = level != nullptr ? first<game::Player>(*level) : nullptr;
            const game::Platform *platform =
                level != nullptr ? first<game::Platform>(*level) : nullptr;
            if (p == nullptr || platform == nullptr) {
                fail("the jump check found no player or no platform");
                next(Phase::DONE);
                break;
            }
            const Rectangle box = p->world_collider();
            const float feet = box.y + box.height;
            const int settle = static_cast<int>(0.15f / g_dt);
            release_all();
            if (g_t == settle) {
                g_platform_top = platform->world_collider().y;
                g_highest_feet = feet;
                g_dev.keys[KEY_SPACE] = true;
            }
            if (g_t > settle) g_highest_feet = std::min(g_highest_feet, feet);
            if (static_cast<float>(g_t - settle) * g_dt > 0.8f) {
                const float rise = g_platform_top - g_highest_feet;
                const int hz = static_cast<int>(std::lround(1.0f / g_dt));
                std::printf(
                    "PLAY  a jump from the moving platform at %d Hz rose %.1f px\n", hz,
                    rise);
                if (rise < 40) {
                    fail("a jump from the moving platform at " + std::to_string(hz) +
                         " Hz rose " + std::to_string(rise) + " px");
                }
                if (++g_jump_rate < 2) {
                    next(Phase::JUMP_SETUP);
                } else {
                    next(Phase::FAST_SETUP);
                }
            }
            break;
        }
        case Phase::FAST_SETUP:
            if (g_t == 2) {
                g_dt = 1.0f / 240;
                game::new_run();
                g_level_seen.clear();
                rmp::Scene::change<game::LevelScene>("");
            }
            if (g_t > 2 && top<game::LevelScene>() != nullptr) next(Phase::FAST_PLAY);
            break;
        case Phase::DONE:
            release_all();
            break;
    }
}

// A frame of the game. While a level is being played it is the logic only --
// input, the scenes, what they queued -- which is what makes 240 Hz affordable
// on the software renderer; menus are drawn, because their buttons are.
void frame_of_game(float dt) {
    if (top<game::LevelScene>() != nullptr && g_phase != Phase::TITLE) {
        rmp::input::detail::begin_frame();
        rmp::scenes::detail::update(dt);
        rmp::scenes::detail::apply_pending();
    } else {
        rmp::app::detail::frame(dt);
    }
}

void ready() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    // The best run from an earlier run of this program would make "kept on
    // disk" pass without anything being kept.
    rmp::save::remove("platformer");
    rmp::input::detail::set_sample_provider(sample);
    rmp::app::detail::start(std::make_unique<game::TitleScene>());
}

void frame(float /*real time is not the game's time here*/) {
    rmp::ui::detail::set_nav_provider(navigate);
    step();
    if (g_phase == Phase::DONE) {
        if (g_failures == 0) {
            std::printf("PLAY PASS\n");
        } else {
            std::printf("PLAY FAIL (%d)\n", g_failures);
        }
        rmp::save::remove("platformer");
        rmp::app::quit();
        return;
    }
    frame_of_game(g_dt);
}

void stop() { rmp::app::detail::stop(); }

} // namespace

RMP_ENTRY_POINT(ready, frame, stop)
