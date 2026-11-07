// The look: Kenney's beige frames and brown plates, his Kenvector font, and a
// Stage whose people each have a face for every mood and a voice of their own.
// And the story so far, kept with rmp::save.

#include "school.h"

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/save.h>
#include <rmp/ui.h>

#include <array>
#include <string>
#include <string_view>

namespace game {

namespace {

constexpr float FRAME = 12; // panel.png's border, in its pixels
constexpr float PLATE = 10; // button.png's and plate.png's
constexpr float PORTRAIT_HEIGHT = 250;

const Color INK = { 62, 39, 35, 255 };
const Color CREAM = { 255, 246, 226, 255 };
const Color DUSK = { 255, 178, 92, 255 };

rmp::ui::Picture frame(std::string_view file, float border) {
    return rmp::ui::NineSlice{ .texture = rmp::assets::load_texture(file),
                               .left = border,
                               .top = border,
                               .right = border,
                               .bottom = border,
                               .scale = 2 };
}

// Who is in the script, their voice's pitch, and the colour of their plate.
struct Person {
    std::string_view name;
    std::string_view look;
    float pitch;
    Color plate;
};
constexpr std::array<Person, 4> PEOPLE{ {
    { "Mika", "mika", 1.25f, { 226, 92, 92, 255 } },
    { "Sora", "sora", 0.85f, { 92, 132, 226, 255 } },
    { "Aki", "aki", 1.1f, { 120, 180, 96, 255 } },
    { "Kai", "kai", 0.95f, { 230, 160, 60, 255 } },
} };
constexpr std::array<std::string_view, 3> MOODS{ "happy", "sad", "think" };

} // namespace

Story &story() { return rmp::global<Story>(); }

void new_story() { story() = Story{}; }

bool has_saved_story() { return rmp::save::exists("after_school"); }

void load_story() {
    rmp::Value v;
    rmp::save::read("after_school", v);
    Story &s = story();
    s.hour = v["hour"].as_int(15);
    s.with = std::string(v["with"].as_string(""));
    s.met_mika = v["met_mika"].as_bool();
    s.met_sora = v["met_sora"].as_bool();
    s.met_aki = v["met_aki"].as_bool();
    s.met_kai = v["met_kai"].as_bool();
    s.raced = v["raced"].as_bool();
    s.flyer = v["flyer"].as_bool();
}

void save_story() {
    const Story &s = story();
    rmp::Value v;
    v["hour"] = s.hour;
    v["with"] = s.with;
    v["met_mika"] = s.met_mika;
    v["met_sora"] = s.met_sora;
    v["met_aki"] = s.met_aki;
    v["met_kai"] = s.met_kai;
    v["raced"] = s.raced;
    v["flyer"] = s.flyer;
    rmp::save::write("after_school", v);
}

void forget_story() { rmp::save::remove("after_school"); }

void set_look() {
    rmp::ui::set_font(rmp::assets::load_font("font.ttf", 16));
    rmp::ui::Theme look = rmp::ui::theme_light();
    look.text = INK;
    look.primary = DUSK;
    look.sounds = { .hover = "tap", .click = "click", .volume = 0.5f };
    look.styles.panel = {
        .skin = rmp::ui::Skin{ .normal = frame("panel.png", FRAME) },
        .shadow =
            rmp::ui::Shadow{ .offset = { 0, 6 }, .blur = 12, .color = { 0, 0, 0, 110 } },
        .padding_x = 30,
        .padding_y = 24,
        .gap = 12,
        .motion = rmp::ui::Motion{ .appear = 0.25f, .appear_offset = 18 },
    };
    look.styles.button = {
        .skin = rmp::ui::Skin{ .normal = frame("button.png", PLATE),
                               .press = frame("button_down.png", PLATE) },
        .padding_x = 26,
        .padding_y = 12,
        .text = INK,
        .motion = rmp::ui::Motion{ .hover_scale = 1.04f, .press_scale = 0.96f },
    };
    rmp::ui::set_theme(look);
}

rmp::ui::DialogueStage make_stage() {
    rmp::ui::DialogueStage stage{
        .flow = { .letters_per_second = 50,
                  .blip = "tap",
                  .blip_every = 3,
                  .blip_jitter = 0.06f },
        .portrait_height = PORTRAIT_HEIGHT,
        .name_style = { .skin = rmp::ui::Skin{ .normal = frame("plate.png", PLATE) },
                        .padding_x = 22,
                        .padding_y = 8,
                        .text = CREAM },
    };
    for (const Person &p : PEOPLE) {
        const std::string face = "portraits/" + std::string(p.look) + "_";
        stage.portraits.push_back(
            { .speaker = std::string(p.name),
              .picture = rmp::assets::load_texture(face + "neutral.png") });
        for (const std::string_view mood : MOODS) {
            stage.portraits.push_back({ .speaker = std::string(p.name),
                                        .mood = std::string(mood),
                                        .picture = rmp::assets::load_texture(
                                            face + std::string(mood) + ".png") });
        }
        stage.speakers.push_back({ .name = std::string(p.name),
                                   .pitch = p.pitch,
                                   .name_style = { .tint = p.plate } });
    }
    return stage;
}

} // namespace game
