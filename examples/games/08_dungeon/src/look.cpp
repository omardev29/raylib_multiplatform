// The run, the best run, the look of every screen, and the two ways the game
// draws a tile of its own.

#include "dungeon.h"

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/save.h>
#include <rmp/ui.h>

#include <cmath>

namespace game {

namespace {

constexpr int COLUMNS = 12; // tiles.png, packed

// Kenney's Ancient frames are 48 pixels square; the border is 6 of them, and
// the bottom 8 with its shadow. Drawn three design units to a pixel of the art.
constexpr float FRAME = 6;
constexpr float FRAME_BOTTOM = 8;
constexpr float FRAME_SCALE = 3;

const Color PARCHMENT = { 248, 232, 200, 255 };
const Color INK = { 58, 34, 26, 255 };
const Color EMBER = { 255, 196, 92, 255 };

struct Art {
    rmp::Texture tiles = rmp::assets::load_texture("tiles.png");
};

rmp::ui::Picture frame(std::string_view file, float inset = 0) {
    return rmp::ui::NineSlice{ .texture = rmp::assets::load_texture(file),
                               .left = FRAME - inset,
                               .top = FRAME - inset,
                               .right = FRAME - inset,
                               .bottom = FRAME_BOTTOM - inset,
                               .scale = FRAME_SCALE };
}

} // namespace

Run &run() { return rmp::global<Run>(); }

void new_run() { rmp::global<Run>() = Run{}; }

int best_gems() {
    rmp::Value saved;
    rmp::save::read("dungeon", saved);
    return saved["gems"].as_int(-1);
}

bool keep_if_best(int gems) {
    if (gems <= best_gems()) return false;
    rmp::Value v;
    v["gems"] = gems;
    rmp::save::write("dungeon", v);
    return true;
}

void set_look() {
    rmp::ui::set_font(rmp::assets::load_font("font.ttf", 16));
    rmp::ui::Theme look = rmp::ui::theme_dark();
    look.text = PARCHMENT;
    look.text_muted = Color{ 214, 180, 140, 255 };
    look.primary = EMBER;
    look.sounds = { .hover = "hover", .click = "click", .volume = 0.6f };
    look.styles.panel = {
        .skin = rmp::ui::Skin{ .normal = frame("panel.png") },
        .shadow =
            rmp::ui::Shadow{ .offset = { 0, 8 }, .blur = 14, .color = { 0, 0, 0, 140 } },
        .padding_x = 28,
        .padding_y = 24,
        .gap = 14,
        .text_shadow =
            rmp::ui::TextShadow{ .offset = { 0, 2 }, .color = { 30, 14, 10, 200 } },
        .motion = rmp::ui::Motion{ .appear = 0.22f,
                                   .appear_offset = 14,
                                   .appear_scale = 0.97f },
    };
    look.styles.button = {
        .skin = rmp::ui::Skin{ .normal = frame("button.png"),
                               .press = frame("button_down.png") },
        .shadow =
            rmp::ui::Shadow{ .offset = { 0, 4 }, .blur = 6, .color = { 0, 0, 0, 120 } },
        .padding_x = 26,
        .padding_y = 12,
        .text = INK,
        .motion = rmp::ui::Motion{ .hover_scale = 1.05f, .press_scale = 0.95f },
    };
    look.styles.progress = {
        .skin = rmp::ui::Skin{ .normal = frame("inlay.png", 2) },
    };
    rmp::ui::set_theme(look);
}

void draw_tile(int index, Vector2 at, Color tint, float scale, float rotation) {
    const Texture2D &tiles = rmp::global<Art>().tiles;
    const Rectangle source{ static_cast<float>(index % COLUMNS) * TILE,
                            std::floor(static_cast<float>(index) / COLUMNS) * TILE, TILE,
                            TILE };
    const float size = TILE * scale;
    DrawTexturePro(tiles, source, Rectangle{ at.x, at.y, size, size },
                   Vector2{ size / 2, size / 2 }, rotation, tint);
}

void draw_glow(Vector2 at, float radius, Color color) {
    // Rings from the edge in, each adding a little light: a soft falloff that
    // needs nothing but circles.
    constexpr int RINGS = 14;
    const Color step{ color.r, color.g, color.b,
                      static_cast<unsigned char>(color.a / RINGS) };
    BeginBlendMode(BLEND_ADDITIVE);
    for (int i = 0; i < RINGS; ++i) {
        DrawCircleV(at, radius * static_cast<float>(RINGS - i) / RINGS, step);
    }
    EndBlendMode();
}

} // namespace game
