// Sparks and rising numbers: small objects that fade and go, made of the
// catalogue's Tween and Lifespan.

#include "objects.h"

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/behavior.h>
#include <rmp/random.h>

#include <cmath>
#include <string>
#include <vector>

namespace game {

namespace {

constexpr float SPARK_SIZE = 2;
constexpr float SPARK_SPEED = 70;
constexpr float SPARK_LIFE = 0.45f;
constexpr float TEXT_RISE = 26;
constexpr float TEXT_LIFE = 0.8f;
constexpr float TEXT_SIZE = 8;

struct Letters {
    rmp::Font font = rmp::assets::load_font("font.ttf", 8);
};

// A word in the world, drawn in the game's font, fading with its object.
struct Words {
    std::string text;
    Color color = WHITE;
    float elapsed = 0;
    void _update(rmp::Object &self, float delta) {
        (void)self;
        elapsed += delta;
    }
    void _draw(rmp::Object &self) {
        const Font &font = rmp::global<Letters>().font;
        const float fade = 1 - (elapsed / TEXT_LIFE);
        const Vector2 size = MeasureTextEx(font, text.c_str(), TEXT_SIZE, 0);
        const Vector2 at{ self.position.x - (size.x / 2), self.position.y - (size.y / 2) };
        DrawTextEx(font, text.c_str(), Vector2{ at.x + 1, at.y + 1 }, TEXT_SIZE, 0,
                   Fade(BLACK, fade * 0.7f));
        DrawTextEx(font, text.c_str(), at, TEXT_SIZE, 0, Fade(color, fade));
    }
};

// How dark the dungeon is between its lights, of 255.
constexpr unsigned char DARK = 150;
const Color NIGHT = { 14, 6, 18, DARK };

// Every object with a Light, for the darkness to draw.
std::vector<rmp::Handle<rmp::Object>> lit;

struct Dark {
    void _update(rmp::Object &self, float delta) {
        (void)self;
        (void)delta;
    }
    void _draw(rmp::Object &self) {
        Rectangle view = self.scene().camera.view();
        DrawRectangleRec(Rectangle{ view.x - TILE, view.y - TILE, view.width + (2 * TILE),
                                    view.height + (2 * TILE) },
                         NIGHT);
        std::erase_if(lit, [](const rmp::Handle<rmp::Object> &h) { return !h; });
        for (const rmp::Handle<rmp::Object> &h : lit) {
            const auto object = h.get();
            if (!object->visible || !object->enabled<Light>()) continue;
            const Light &light = *object->get<Light>();
            const float breath = light.flicker
                ? 1 + (0.08f * std::sin(light.clock * 7.3f)) + (0.05f * std::sin(light.clock * 13.1f))
                : 1;
            draw_glow(object->position, light.radius * breath, light.color);
        }
    }
};

} // namespace

void Light::_ready(rmp::Object &self) { lit.push_back(self.handle()); }

void Light::_update(rmp::Object &self, float delta) {
    (void)self;
    clock += delta;
}

void add_darkness(rmp::Scene &scene) {
    lit.clear();
    auto &dark = scene.spawn({ .layer = 50 });
    dark.collision_layer = 0;
    dark.collision_mask = 0;
    dark.add<Dark>();
}

void sparks(rmp::Scene &scene, Vector2 at, Color color, int count) {
    for (int i = 0; i < count; ++i) {
        const Vector2 velocity{ rmp::random::range(-SPARK_SPEED, SPARK_SPEED),
                                rmp::random::range(-SPARK_SPEED, SPARK_SPEED) };
        auto &spark = scene.spawn({ .position = at, .size = { SPARK_SIZE, SPARK_SIZE },
                                    .velocity = velocity, .layer = 8 });
        spark.shape.color = color;
        spark.collision_layer = 0;
        spark.collision_mask = 0;
        spark.add<rmp::behavior::Tween>({ .property = rmp::behavior::Property::ALPHA,
                                          .from = 255,
                                          .to = 0,
                                          .seconds = SPARK_LIFE,
                                          .ease = rmp::behavior::Ease::IN });
        spark.add<rmp::behavior::Lifespan>({ .seconds = SPARK_LIFE });
    }
}

void float_text(rmp::Scene &scene, Vector2 at, std::string_view text, Color color) {
    auto &words = scene.spawn({ .position = at, .velocity = { 0, -TEXT_RISE / TEXT_LIFE },
                                .layer = 9 });
    words.collision_layer = 0;
    words.collision_mask = 0;
    words.add<Words>({ .text = std::string(text), .color = color });
    words.add<rmp::behavior::Lifespan>({ .seconds = TEXT_LIFE });
}

} // namespace game
