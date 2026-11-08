// ---------------------------------------------------------------------------
// examples/ui/10_dialogue/src/main.cpp
//
// One Yarn conversation, shown three ways -- switch while it runs:
//
//   Box     a box along the bottom: a portrait, a name plate on the frame, a
//           voice for each speaker. The classic RPG.
//   Bubble  a bubble over whoever speaks, its tail pointing at them, while the
//           park goes on around it.
//   Stage   big portraits that slide in, the speaker in front, the rest
//           dimmed; a quake shakes it. A visual novel.
//
// And what the game hands the dialogue: $cups is the scene's own int, bound
// (talk.bind), time_of_day() a typed function, <<quake>> a command. The
// script waits a second by itself (<<wait 1>>).
//
// The art is Kenney's (CC0): tools/make_story_assets.py. The words are
// resources/talk.yarn. Run it: `rmp example 10_dialogue`.
// ---------------------------------------------------------------------------

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/dialogue.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <array>
#include <string>
#include <string_view>

namespace {

constexpr float FRAME = 12; // panel.png's border, in its pixels
constexpr float PLATE = 10; // plate.png's
constexpr std::array<std::string_view, 3> MOODS{ "happy", "sad", "think" };
const Color PARK = { 109, 190, 106, 255 };
const Color INK = { 62, 39, 35, 255 };
const Color CREAM = { 255, 246, 226, 255 };

rmp::ui::Picture frame(std::string_view file, float border) {
    return rmp::ui::NineSlice{ .texture = rmp::assets::load_texture(file),
                               .left = border,
                               .top = border,
                               .right = border,
                               .bottom = border,
                               .scale = 2 };
}

// Both of them, in every mood: one picture each, by speaker and #mood:.
std::vector<rmp::ui::Portrait> portraits() {
    const auto picture = [](const std::string &file) -> rmp::ui::Picture {
        return rmp::assets::load_texture(file);
    };
    std::vector<rmp::ui::Portrait> all;
    for (const std::string who : { "mika", "sora" }) {
        const std::string name = who == "mika" ? "Mika" : "Sora";
        const std::string file = "portraits/" + who + "_";
        all.push_back({ .speaker = name, .picture = picture(file + "neutral.png") });
        for (const std::string_view mood : MOODS) {
            all.push_back({ .speaker = name,
                            .mood = std::string(mood),
                            .picture = picture(file + std::string(mood) + ".png") });
        }
    }
    return all;
}

} // namespace

class ParkScene : public rmp::Scene {
public:
    void _ready() override {
        background = PARK;
        rmp::ui::set_font(rmp::assets::load_font("font.ttf", 16));
        rmp::ui::Theme look = rmp::ui::theme_light();
        look.text = INK;
        look.sounds = { .click = "click", .volume = 0.5f };
        look.styles.panel = { .skin =
                                  rmp::ui::Skin{ .normal = frame("panel.png", FRAME) },
                              .padding_x = 26,
                              .padding_y = 20,
                              .gap = 10 };
        look.styles.button = {
            .skin = rmp::ui::Skin{ .normal = frame("button.png", PLATE),
                                   .press = frame("button_down.png", PLATE) },
            .padding_x = 20,
            .padding_y = 10,
            .text = INK
        };
        rmp::ui::set_theme(look);

        // Two people in the park, for the bubble to stand over.
        for (const auto &[name, x] :
             { std::pair{ "Mika", 400.0f }, std::pair{ "Sora", 580.0f } }) {
            auto &person = spawn({ .position = { x, 180 } });
            person.sprite.texture = rmp::assets::load_texture(
                std::string("portraits/") + (name[0] == 'M' ? "mika" : "sora") +
                "_neutral.png");
            person.sprite.size = { 72, 96 };
            _people.push_back(
                { .name = name, .object = person.handle(), .offset = { 0, -52 } });
        }
        _people[0].pitch = 1.25f;
        _people[1].pitch = 0.85f;
        start();
    }

    void _draw() override {
        rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT, .padding = 12 });
        rmp::ui::panel([this] {
            rmp::ui::dropdown("Shown as", _style, { "Box", "Bubble", "Stage" });
            rmp::ui::text(rmp::Text("Cups of tea: {cups}", { { "cups", _cups } }),
                          { .wrap = false });
            if (rmp::ui::button("Again")) start();
        });
        rmp::ui::end();

        rmp::ui::begin({ .placement = rmp::ui::Align::BOTTOM_CENTER, .grow = true });
        if (_style == 0) _box.show(_talk);
        if (_style == 2) _stage.show(_talk);
        rmp::ui::end();
        if (_style == 1) {
            rmp::ui::begin();
            _bubble.show(_talk);
            rmp::ui::end();
        }
    }

private:
    void start() {
        _talk = rmp::assets::load_dialogue("talk.yarn");
        _talk.bind("$cups", _cups);
        // Morning, afternoon or evening: the game's clock, not the script's.
        _talk.on_function("time_of_day", []() -> std::string { return "afternoon"; });
        _talk.on_command("quake", [this] { _stage.shake(); });
        _talk.start();

        const rmp::ui::DialogueFlow flow{
            .letters_per_second = 40, .blip = "tap", .blip_every = 3, .blip_jitter = 0.05f
        };
        const rmp::ui::Style plate{ .skin = rmp::ui::Skin{ .normal = frame("plate.png",
                                                                           PLATE) },
                                    .padding_x = 18,
                                    .padding_y = 6,
                                    .text = CREAM };
        const std::vector<rmp::ui::Speaker> voices{ { .name = "Mika", .pitch = 1.25f },
                                                    { .name = "Sora", .pitch = 0.85f } };
        _box = { .portraits = portraits(),
                 .speakers = voices,
                 .flow = flow,
                 .width = 640,
                 .name_offset = { 18, -16 },
                 .name_style = plate };
        _bubble = { .speakers = _people, .flow = flow, .width = 240 };
        _stage = { .portraits = portraits(),
                   .speakers = voices,
                   .flow = flow,
                   .portrait_height = 250,
                   .name_style = plate };
    }

    rmp::Dialogue _talk;
    int _cups = 0;
    int _style = 0;
    std::vector<rmp::ui::BubbleSpeaker> _people;
    rmp::ui::DialogueBox _box;
    rmp::ui::DialogueBubble _bubble;
    rmp::ui::DialogueStage _stage;
};

RMP_GAME(ParkScene);
