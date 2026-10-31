// ---------------------------------------------------------------------------
// The catalogue's third part, what moves on its own: Patrol, rmp::find_path()
// and Pathfind, Chase and StateMachine. The surface is include/rmp/behavior.h;
// the shared arithmetic is src/rmp/behaviors.cpp's, through
// behavior_internal.h.
//
// The map is read through its public questions -- solid_at(), solid_in(),
// bounds(), tile_size() -- so a path goes around exactly the cells a solid
// object collides with.
// ---------------------------------------------------------------------------

#include <rmp/behavior.h>

#include <rmp/tilemap.h>

#include "animation_internal.h"
#include "behavior_internal.h"
#include "internal.h"
#include "object_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Every hook below is one half of an interface: its signature is the
// contract's, and clang-tidy's advice to make one static or const would break
// it (src/rmp/behaviors.cpp says the same, at more length).
//
// NOLINTBEGIN(readability-convert-member-functions-to-static,readability-make-member-function-const,readability-named-parameter)
namespace rmp {

namespace {

using behavior::detail::finite;
using behavior::detail::length_of;
using behavior::detail::NEAR_ZERO;
using behavior::detail::normalised;
using behavior::detail::put;
using behavior::detail::say_nan;
using behavior::detail::seconds_of;

constexpr float DEGREES = 180.0f / std::numbers::pi_v<float>;

// The most cells a search walks. A map bigger than this -- 1024 by 1024 -- is
// not searched whole: find_path() says so once and finds nothing.
constexpr std::int64_t MOST_CELLS = std::int64_t{ 1 } << 20;

// Two goals this close are the same goal.
constexpr float SAME_PLACE = 0.5f;

Vector2 minus(Vector2 a, Vector2 b) { return Vector2{ a.x - b.x, a.y - b.y }; }

Vector2 times(Vector2 v, float k) { return Vector2{ v.x * k, v.y * k }; }

bool same_place(Vector2 a, Vector2 b) { return length_of(minus(a, b)) <= SAME_PLACE; }

bool sheet_has(const Object &self, std::string_view tag) {
    if (tag.empty() || !self.sprite.sheet.valid()) return false;
    return rmp::animation::detail::tag_index(self.sprite.sheet.raw(), tag) >= 0;
}

void play_if_there(Object &self, std::string_view tag) {
    if (sheet_has(self, tag) && self.sprite.playing() != tag) self.sprite.play(tag);
}

// The grid find_path() searches: the map's cells, and which of them a walker
// keeping `clearance` from every solid cell cannot stand in.
struct Grid {
    Rectangle bounds{};
    Vector2 cell{};
    int columns = 0;
    int rows = 0;
    std::vector<unsigned char> solid; // the map's own
    std::vector<unsigned char> blocked; // solid, or too near a solid one

    [[nodiscard]] bool inside(int c, int r) const {
        return c >= 0 && r >= 0 && c < columns && r < rows;
    }
    [[nodiscard]] std::size_t at(int c, int r) const {
        return static_cast<std::size_t>(r) * static_cast<std::size_t>(columns) +
            static_cast<std::size_t>(c);
    }
    [[nodiscard]] Vector2 centre(int c, int r) const {
        return Vector2{ bounds.x + (static_cast<float>(c) + 0.5f) * cell.x,
                        bounds.y + (static_cast<float>(r) + 0.5f) * cell.y };
    }
    // The cell a point is in, or false outside the grid.
    bool cell_of(Vector2 p, int &c, int &r) const {
        if (!finite(p)) return false;
        const float fc = std::floor((p.x - bounds.x) / cell.x);
        const float fr = std::floor((p.y - bounds.y) / cell.y);
        if (fc < 0 || fr < 0 || fc >= static_cast<float>(columns) ||
            fr >= static_cast<float>(rows)) {
            return false;
        }
        c = static_cast<int>(fc);
        r = static_cast<int>(fr);
        return true;
    }
};

bool build_grid(const Tilemap &map, float clearance, Grid &grid) {
    if (!map.valid()) return false;
    grid.bounds = map.bounds();
    grid.cell = map.tile_size();
    if (!(grid.cell.x > 0) || !(grid.cell.y > 0)) return false;
    const double columns = std::floor(grid.bounds.width / grid.cell.x);
    const double rows = std::floor(grid.bounds.height / grid.cell.y);
    if (!(columns >= 1) || !(rows >= 1)) return false;
    if (columns * rows > static_cast<double>(MOST_CELLS)) {
        RMP_REPORT_ONCE("PATH: the map has %.0f cells, and a path is searched over %lld at "
                        "most. Nothing was found.",
                        columns * rows, static_cast<long long>(MOST_CELLS));
        return false;
    }
    grid.columns = static_cast<int>(columns);
    grid.rows = static_cast<int>(rows);
    const std::size_t cells = static_cast<std::size_t>(grid.columns) *
        static_cast<std::size_t>(grid.rows);
    grid.solid.assign(cells, 0);
    for (int r = 0; r < grid.rows; ++r) {
        for (int c = 0; c < grid.columns; ++c) {
            grid.solid[grid.at(c, r)] = map.solid_at(grid.centre(c, r)) ? 1 : 0;
        }
    }
    grid.blocked = grid.solid;
    if (!(clearance > 0) || !std::isfinite(clearance)) return true;

    // A cell is blocked when a solid cell comes nearer its centre than the
    // clearance: a walker that wide, centred there, would touch it.
    const int reach_x = static_cast<int>(std::ceil(clearance / grid.cell.x));
    const int reach_y = static_cast<int>(std::ceil(clearance / grid.cell.y));
    for (int r = 0; r < grid.rows; ++r) {
        for (int c = 0; c < grid.columns; ++c) {
            if (grid.solid[grid.at(c, r)] != 0) continue;
            const Vector2 middle = grid.centre(c, r);
            bool near = false;
            for (int dr = -reach_y; dr <= reach_y && !near; ++dr) {
                for (int dc = -reach_x; dc <= reach_x && !near; ++dc) {
                    const int nc = c + dc;
                    const int nr = r + dr;
                    if (!grid.inside(nc, nr) || grid.solid[grid.at(nc, nr)] == 0) continue;
                    const float left = grid.bounds.x + static_cast<float>(nc) * grid.cell.x;
                    const float top = grid.bounds.y + static_cast<float>(nr) * grid.cell.y;
                    const float dx =
                        std::max({ left - middle.x, 0.0f, middle.x - (left + grid.cell.x) });
                    const float dy =
                        std::max({ top - middle.y, 0.0f, middle.y - (top + grid.cell.y) });
                    near = dx * dx + dy * dy < clearance * clearance;
                }
            }
            if (near) grid.blocked[grid.at(c, r)] = 1;
        }
    }
    return true;
}

// Whether nothing solid on the map stands between `a` and `b`: the segment,
// sampled every quarter of a cell.
bool clear_line(const Tilemap &map, Vector2 a, Vector2 b) {
    if (!map.valid()) return true;
    const Vector2 cell = map.tile_size();
    const float step = std::min(cell.x, cell.y) / 4;
    if (!(step > 0)) return true;
    const float d = length_of(minus(b, a));
    if (!std::isfinite(d)) return false;
    constexpr float MOST_SAMPLES = 4096;
    const float samples = std::min(std::ceil(d / step), MOST_SAMPLES);
    for (float i = 1; i < samples; ++i) {
        const float t = i / samples;
        if (map.solid_at(Vector2{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t })) return false;
    }
    return true;
}

} // namespace

std::vector<Vector2> find_path(const Tilemap &map, Vector2 from, Vector2 to,
                               const PathOptions &options) {
    Grid grid;
    if (!build_grid(map, options.clearance, grid)) return {};
    int sc = 0;
    int sr = 0;
    int gc = 0;
    int gr = 0;
    if (!grid.cell_of(from, sc, sr) || !grid.cell_of(to, gc, gr)) return {};
    if (grid.solid[grid.at(sc, sr)] != 0 || grid.solid[grid.at(gc, gr)] != 0) return {};
    // Standing near a wall is where a walker starts and ends; only the cells
    // between them have to keep the clearance.
    grid.blocked[grid.at(sc, sr)] = 0;
    grid.blocked[grid.at(gc, gr)] = 0;
    if (sc == gc && sr == gr) return { to };

    const std::size_t cells = grid.blocked.size();
    const float infinity = std::numeric_limits<float>::infinity();
    std::vector<float> cost(cells, infinity);
    std::vector<int> came(cells, -1);
    std::vector<unsigned char> done(cells, 0);
    const float wide = grid.cell.x;
    const float tall = grid.cell.y;
    const float slant = std::sqrt(wide * wide + tall * tall);
    const auto guess = [&](int c, int r) {
        const float dx = static_cast<float>(std::abs(c - gc)) * wide;
        const float dy = static_cast<float>(std::abs(r - gr)) * tall;
        if (!options.diagonal) return dx + dy;
        // Octile: the diagonal steps first, then the straight ones.
        const float across = std::min(dx / wide, dy / tall);
        return dx + dy + across * (slant - wide - tall);
    };

    using Entry = std::pair<float, int>; // (estimate, cell)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    const std::size_t start = grid.at(sc, sr);
    const std::size_t goal = grid.at(gc, gr);
    cost[start] = 0;
    open.emplace(guess(sc, sr), static_cast<int>(start));
    while (!open.empty()) {
        const auto [estimate, here_int] = open.top();
        (void)estimate;
        open.pop();
        const auto here = static_cast<std::size_t>(here_int);
        if (done[here] != 0) continue;
        done[here] = 1;
        if (here == goal) break;
        const int c = here_int % grid.columns;
        const int r = here_int / grid.columns;
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if (dc == 0 && dr == 0) continue;
                const bool diagonal = dc != 0 && dr != 0;
                if (diagonal && !options.diagonal) continue;
                const int nc = c + dc;
                const int nr = r + dr;
                if (!grid.inside(nc, nr) || grid.blocked[grid.at(nc, nr)] != 0) continue;
                // A diagonal step never cuts a corner: both cells beside it
                // must be open too.
                if (diagonal && (grid.blocked[grid.at(c + dc, r)] != 0 ||
                                 grid.blocked[grid.at(c, r + dr)] != 0)) {
                    continue;
                }
                const std::size_t next = grid.at(nc, nr);
                const float step = diagonal ? slant : (dc != 0 ? wide : tall);
                const float through = cost[here] + step;
                if (through < cost[next]) {
                    cost[next] = through;
                    came[next] = here_int;
                    open.emplace(through + guess(nc, nr), static_cast<int>(next));
                }
            }
        }
    }
    if (done[goal] == 0) return {};

    std::vector<Vector2> path;
    for (int at = static_cast<int>(goal); at != static_cast<int>(start);
         at = came[static_cast<std::size_t>(at)]) {
        path.push_back(grid.centre(at % grid.columns, at / grid.columns));
    }
    std::ranges::reverse(path);
    path.back() = to;
    return path;
}

namespace behavior {

// ---------------------------------------------------------------------------
// Patrol
// ---------------------------------------------------------------------------

Patrol Patrol::from(const MapObject &object) {
    Patrol made;
    const int points = object.property_count("path");
    if (points > 0) {
        made.waypoints.push_back(object.position);
        for (int i = 0; i < points; ++i) {
            made.waypoints.push_back(object.property_point("path", i, object.position));
        }
    }
    made.speed = object.property_float("speed", made.speed);
    made.wait = object.property_float("wait", made.wait);
    made.ping_pong = object.property_bool("ping_pong", made.ping_pong);
    return made;
}

void Patrol::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("Patrol", { { "speed", speed }, { "wait", wait } });
    bool moving = false;
    if (!waypoints.empty()) {
        const int count = static_cast<int>(waypoints.size());
        ours.index = std::clamp(ours.index, 0, count - 1);
        if (ours.waited > 0) {
            ours.waited -= seconds;
            self.velocity = Vector2{};
        } else {
            const Vector2 goal = waypoints[static_cast<std::size_t>(ours.index)];
            const Vector2 to = minus(goal, self.position);
            const float reach = std::max(1.0f, speed * seconds);
            if (length_of(to) <= reach) {
                put(self.position, goal, "Patrol");
                self.velocity = Vector2{};
                ours.waited = wait > 0 ? wait : 0;
                if (count > 1 && ping_pong) {
                    if (ours.index + ours.step < 0 || ours.index + ours.step >= count) {
                        ours.step = -ours.step;
                    }
                    ours.index += ours.step;
                } else if (count > 1) {
                    ours.index = (ours.index + 1) % count;
                }
            } else {
                put(self.velocity, times(normalised(to), speed), "Patrol");
                moving = true;
            }
        }
    } else {
        // Along the floor: turn at a wall, before a drop, or when something
        // stopped it -- a solid object the map does not know.
        if (!ours.started) ours.way = self.flip_x ? -1.0f : 1.0f;
        bool turn =
            ours.walked && seconds > 0 && std::fabs(self.position.x - ours.was_x) < NEAR_ZERO;
        const Ref<Scene> scene = Storage::spawned_in(self);
        const Rectangle box = self.world_collider();
        if (!turn && scene && scene->map.valid() && box.width > 0 && box.height > 2) {
            const Tilemap &map = scene->map;
            const float front = ours.way > 0 ? box.x + box.width + 1 : box.x - 1;
            const bool wall = map.solid_in(Rectangle{ ours.way > 0 ? front - 1 : front,
                                                      box.y + 1, 1, box.height - 2 });
            const bool grounded = map.solid_in(Rectangle{ box.x, box.y + box.height, box.width, 2 });
            const bool ledge = turn_at_ledges && grounded &&
                !map.solid_at(Vector2{ front, box.y + box.height + 2 });
            turn = wall || ledge;
        }
        if (turn) ours.way = -ours.way;
        ours.was_x = self.position.x;
        put(self.velocity.x, ours.way * speed, "Patrol");
        ours.walked = speed != 0;
        moving = speed != 0;
    }
    ours.started = true;
    if (self.velocity.x < -NEAR_ZERO) self.flip_x = true;
    if (self.velocity.x > NEAR_ZERO) self.flip_x = false;
    if (!self.has<Animator>()) play_if_there(self, moving ? walk : idle);
}

// ---------------------------------------------------------------------------
// Pathfind
// ---------------------------------------------------------------------------

void Pathfind::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("Pathfind",
            { { "speed", speed }, { "clearance", clearance }, { "repath_every", repath_every } });
    std::optional<Vector2> where = goal;
    if (target != Handle<Object>{}) {
        const Ref<Object> t = target.get();
        where = t ? std::optional<Vector2>(t->position) : std::nullopt;
    }
    if (!where || !finite(*where)) {
        // Nowhere to go: it leaves the velocity to whoever else moves it.
        ours.path.clear();
        ours.next = 0;
        ours.arrived = true;
        return;
    }
    ours.since += seconds;
    if (ours.arrived && same_place(*where, ours.aimed)) {
        self.velocity = Vector2{};
        return;
    }
    const bool spent = static_cast<std::size_t>(ours.next) >= ours.path.size();
    if (spent || (ours.since >= repath_every && !same_place(*where, ours.aimed))) {
        const Ref<Scene> scene = Storage::spawned_in(self);
        if (scene && scene->map.valid()) {
            ours.path = find_path(scene->map, self.position, *where,
                                  PathOptions{ .diagonal = diagonal, .clearance = clearance });
        } else {
            ours.path = { *where };
        }
        ours.next = 0;
        ours.since = 0;
        ours.aimed = *where;
        ours.arrived = ours.path.empty();
        if (ours.arrived) {
            self.velocity = Vector2{};
            return;
        }
    }
    const float reach = std::max(2.0f, speed * seconds);
    while (static_cast<std::size_t>(ours.next) < ours.path.size() &&
           length_of(minus(ours.path[static_cast<std::size_t>(ours.next)], self.position)) <=
               reach) {
        ++ours.next;
    }
    if (static_cast<std::size_t>(ours.next) >= ours.path.size()) {
        ours.arrived = true;
        self.velocity = Vector2{};
        return;
    }
    ours.arrived = false;
    const Vector2 to = minus(ours.path[static_cast<std::size_t>(ours.next)], self.position);
    put(self.velocity, times(normalised(to), speed), "Pathfind");
}

// ---------------------------------------------------------------------------
// Chase
// ---------------------------------------------------------------------------

namespace {

bool can_see(const Chase &chase, const Object &self, const Object &target, Vector2 facing) {
    const Vector2 to = minus(target.position, self.position);
    const float d = length_of(to);
    if (!std::isfinite(d)) return false;
    if (chase.sight > 0 && d > chase.sight) return false;
    if (chase.fov < 360 && d > NEAR_ZERO) {
        const float half = std::max(chase.fov, 0.0f) / 2 / DEGREES;
        const float along = (facing.x * to.x + facing.y * to.y) / d;
        if (along < std::cos(half)) return false;
    }
    if (chase.line_of_sight) {
        const Ref<Scene> scene = Storage::spawned_in(self);
        if (scene && !clear_line(scene->map, self.position, target.position)) return false;
    }
    return true;
}

// Steers the Pathfind on `self`, if it has one, or the velocity straight.
void head_for(Object &self, Vector2 where, float speed, const Handle<Object> &follow) {
    if (const Ref<Pathfind> path = self.get<Pathfind>()) {
        path->speed = speed;
        path->target = follow;
        path->goal = where;
        return;
    }
    const Vector2 to = minus(where, self.position);
    if (length_of(to) <= 1) {
        self.velocity = Vector2{};
        return;
    }
    put(self.velocity, times(normalised(to), speed), "Chase");
}

void stand(Object &self) {
    if (const Ref<Pathfind> path = self.get<Pathfind>()) {
        path->target = Handle<Object>{};
        path->goal.reset();
    }
    self.velocity = Vector2{};
}

} // namespace

Chase Chase::from(const MapObject &object) {
    Chase made;
    made.speed = object.property_float("speed", made.speed);
    made.sight = object.property_float("sight", made.sight);
    made.fov = object.property_float("fov", made.fov);
    return made;
}

void Chase::_ready(Object &self) {
    ours.home = self.position;
    ours.facing = Vector2{ self.flip_x ? -1.0f : 1.0f, 0 };
}

void Chase::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    say_nan("Chase", { { "speed", speed },
                       { "sight", sight },
                       { "fov", fov },
                       { "lose_after", lose_after },
                       { "stop_distance", stop_distance } });
    const Vector2 moving = normalised(self.velocity);
    if (moving.x != 0 || moving.y != 0) {
        ours.facing = moving;
    } else if (std::fabs(ours.facing.y) < NEAR_ZERO) {
        ours.facing = Vector2{ self.flip_x ? -1.0f : 1.0f, 0 };
    }

    const Ref<Object> seen = target.get();
    ours.sees = seen && seen->alive() && can_see(*this, self, *seen, ours.facing);
    if (ours.sees) {
        ours.last_seen = seen->position;
        ours.unseen = 0;
        if (!ours.chasing) {
            ours.chasing = true;
            ours.going_home = false;
            self.enable<Patrol>(false);
            on_spot(self, *seen);
            if (!self.alive()) return;
        }
    } else if (ours.chasing) {
        ours.unseen += seconds;
        if (ours.unseen >= lose_after) {
            ours.chasing = false;
            on_lose(self);
            if (!self.alive()) return;
            if (return_home) {
                ours.going_home = true;
            } else {
                stand(self);
                self.enable<Patrol>(true);
            }
        }
    }

    if (ours.chasing) {
        const Vector2 where = ours.sees ? seen->position : ours.last_seen;
        const float d = length_of(minus(where, self.position));
        if (d <= stop_distance || (!ours.sees && d <= 2)) {
            stand(self);
            return;
        }
        head_for(self, where, speed, ours.sees ? target : Handle<Object>{});
        return;
    }
    if (ours.going_home) {
        const float d = length_of(minus(ours.home, self.position));
        if (d <= std::max(2.0f, speed * seconds)) {
            stand(self);
            ours.going_home = false;
            self.enable<Patrol>(true);
            return;
        }
        head_for(self, ours.home, speed, Handle<Object>{});
    }
}

// ---------------------------------------------------------------------------
// StateMachine
// ---------------------------------------------------------------------------

namespace {

int state_index(const StateMachine &machine, std::string_view name) {
    for (std::size_t i = 0; i < machine.states.size(); ++i) {
        if (machine.states[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

void say_no_state(std::string_view name) {
    RMP_REPORT_ONCE_KEYED(name,
                          "BEHAVIOR: a StateMachine has no state called '%.*s'; it stays "
                          "in the one it is in.",
                          static_cast<int>(name.size()), name.data());
}

void enter(StateMachine &machine, Object &self, int index) {
    const auto at = static_cast<std::size_t>(index);
    if (machine.ours.current >= 0 &&
        static_cast<std::size_t>(machine.ours.current) < machine.states.size()) {
        machine.states[static_cast<std::size_t>(machine.ours.current)].on_exit(self);
    }
    machine.ours.current = index;
    machine.ours.seconds = 0;
    // Every behavior some state names runs in the states that name it, and
    // only there.
    const std::vector<int> &on = machine.states[at].enables;
    for (const State &state : machine.states) {
        for (const int kind : state.enables) {
            (void)rmp::detail::set_enabled(self, kind, std::ranges::find(on, kind) != on.end());
        }
    }
    machine.states[at].on_enter(self);
}

} // namespace

std::string_view StateMachine::state() const {
    if (ours.current < 0 || static_cast<std::size_t>(ours.current) >= states.size()) return {};
    return states[static_cast<std::size_t>(ours.current)].name;
}

void StateMachine::_update(Object &self, float delta) {
    const float seconds = seconds_of(delta);
    if (states.empty()) return;
    if (ours.current < 0 || static_cast<std::size_t>(ours.current) >= states.size()) {
        int first = initial.empty() ? 0 : state_index(*this, initial);
        if (first < 0) {
            say_no_state(initial);
            first = 0;
        }
        enter(*this, self, first);
        if (!self.alive()) return;
    }
    if (!ours.pending.empty()) {
        const std::string wanted = std::move(ours.pending);
        ours.pending.clear();
        const int index = state_index(*this, wanted);
        if (index < 0) {
            say_no_state(wanted);
        } else {
            enter(*this, self, index);
            if (!self.alive()) return;
        }
    }
    for (const Transition &move : transitions) {
        if (!move.from.empty() && move.from != state()) continue;
        if (move.to == state()) continue;
        if (!move.when(self)) continue;
        const int index = state_index(*this, move.to);
        if (index < 0) {
            say_no_state(move.to);
            continue;
        }
        enter(*this, self, index);
        if (!self.alive()) return;
        break;
    }
    ours.seconds += seconds;
    states[static_cast<std::size_t>(ours.current)].on_update(self, seconds);
}

} // namespace behavior

} // namespace rmp
// NOLINTEND(readability-convert-member-functions-to-static,readability-make-member-function-const,readability-named-parameter)
