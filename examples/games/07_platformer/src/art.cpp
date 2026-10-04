// The run, the best run, and drawing from tiles.png.

#include "game.h"

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/save.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace game {

namespace {

// A global and not a static texture: globals are destroyed while the window is
// still open, and a texture freed after the GPU context is gone is a crash.
struct Art {
    rmp::Texture tiles = rmp::assets::load_texture("tiles.png");
};

Rectangle source_of(int index) {
    const int columns = 20;
    const int column = index % columns;
    const int row = index / columns;
    return Rectangle{ static_cast<float>(column) * TILE, static_cast<float>(row) * TILE,
                      TILE, TILE };
}

void draw_scaled_tile(int index, Vector2 at, float scale) {
    const Texture2D &tiles = rmp::global<Art>().tiles;
    DrawTexturePro(tiles, source_of(index),
                   Rectangle{ at.x, at.y, TILE * scale, TILE * scale }, Vector2{}, 0,
                   WHITE);
}

} // namespace

Run &run() { return rmp::global<Run>(); }

void new_run() { rmp::global<Run>() = Run{}; }

bool contains(const std::vector<std::string> &list, const char *iid) {
    return std::find(list.begin(), list.end(), iid) != list.end();
}

Best best() {
    rmp::Value saved;
    rmp::save::read("platformer", &saved);
    return Best{ saved["coins"].as_int(-1), saved["seconds"].as_float(0) };
}

bool keep_if_best(int coins, float seconds) {
    const Best before = best();
    const bool better =
        coins > before.coins || (coins == before.coins && seconds < before.seconds);
    if (!better) return false;
    rmp::Value v;
    v["coins"] = coins;
    v["seconds"] = seconds;
    rmp::save::write("platformer", v);
    return true;
}

void draw_icon(int index, Vector2 at, float scale) { draw_scaled_tile(index, at, scale); }

void draw_tile(int index, Vector2 at, Color tint, bool flip_x) {
    const Texture2D &tiles = rmp::global<Art>().tiles;
    Rectangle source = source_of(index);
    if (flip_x) source.width = -source.width;
    DrawTextureRec(tiles, source, at, tint);
}

// The sheet's digits are tiles 160..169. Each glyph sits in the middle of its
// cell, so the advance is narrower than the cell.
void draw_number(int value, Vector2 at, float scale) {
    const std::string digits = std::to_string(std::max(0, value));
    for (std::size_t i = 0; i < digits.size(); i++) {
        draw_scaled_tile(160 + (digits[i] - '0'),
                         { at.x + (static_cast<float>(i) * 11 * scale), at.y }, scale);
    }
}

void draw_seconds(float seconds, Vector2 at, float scale) {
    const int tenths = static_cast<int>(std::floor(seconds * 10));
    draw_number(tenths / 10, at, scale);
    const float after =
        static_cast<float>(std::to_string(tenths / 10).size()) * 11 * scale;
    draw_scaled_tile(157, { at.x + after - (3 * scale), at.y }, scale); // the point
    draw_number(tenths % 10, { at.x + after + (5 * scale), at.y }, scale);
}

} // namespace game
