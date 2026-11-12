#pragma once
// ---------------------------------------------------------------------------
// What every file of the afternoon shares: the story so far, the people, the
// scenes and the look.
// ---------------------------------------------------------------------------

#include <rmp/dialogue.h>
#include <rmp/scene.h>
#include <rmp/ui.h>

#include <string>

namespace game {

inline constexpr float TILE = 16; // the grid of maps/tiles.png and of the town

// Three pixels of the screen for each one of the art's, at the [window] size.
// A whole number: at 2.14 some of a tile's pixels were drawn wider than others.
inline constexpr float ZOOM = 3;

inline constexpr int FIRST_HOUR = 15; // the last bell
inline constexpr int FESTIVAL_HOUR = 19; // seven: the lanterns, and the evening's end

// THE STORY SO FAR: what every conversation reads and writes -- the
// $variables of dialogue/school.yarn are these, bound -- and what Continue
// restores.
struct Story {
    int hour = FIRST_HOUR; // of the afternoon
    float minutes = 0;
    std::string with; // "", "mika" or "sora": who you go with
    bool met_mika = false;
    bool met_sora = false;
    bool met_aki = false;
    bool met_kai = false;
    bool raced = false;
    bool flyer = false; // picked up by the east trees
};
Story &story();
void new_story();
bool has_saved_story();
void load_story();
void save_story();
void forget_story();
// How the evening ends, from who you go with: "festival", "concert" or "home".
std::string evening();

// The look of every screen: Kenney's frames and font, a voice for each
// person, sounds on hover and click. Set once, by the title.
void set_look();
// The conversations: one Stage, Persona style, its portraits and voices.
rmp::ui::DialogueStage make_stage();
// The dusk that comes over the town as the afternoon goes: none at three.
Color dusk(const Story &at);
// The town into a scene's map, with a factory for its trees and street lamps
// and nobody else in it: the town scene registers the people before it calls
// map.spawn_objects(), the title and the ending call it as it is.
void load_town(rmp::Scene &scene);

// Anyone who walks, you included: the sheet's tag for where they go, or last
// went, and drawn in front of whatever stands further up the screen.
struct Character {
    Vector2 facing{ 0, 1 }; // south, towards the screen

    void _update(rmp::Object &self, float delta);
};
// Turns `who` to look at `at`.
void face(rmp::Object &who, Vector2 at);

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
};

class TownScene : public rmp::Scene {
public:
    void _ready() override;
    void _update(float delta) override;
    void _draw() override;

    // A conversation began with whoever stands at `speaker`, or ended.
    void talking(rmp::Handle<rmp::Object> speaker);
    void done_talking();
    // An ending the script asked for: shown once its last line has been read.
    void ending(std::string kind);
    // What the story's conversations need from the game.
    void prepare(rmp::Dialogue &talk);

private:
    void build();
    void pass_time(float delta);
    void draw_clock() const;
    void draw_prompts() const;

    rmp::Handle<rmp::Object> _you;
    rmp::Handle<rmp::Object> _speaker; // whose Talker holds the conversation
    rmp::Dialogue _seven; // what the clock says at seven, to nobody's Talker
    rmp::ui::DialogueStage _stage = make_stage();
    std::string _ending;
};

class PauseScene : public rmp::Scene {
public:
    void _update(float delta) override;
    void _draw() override;
};

class EndScene : public rmp::Scene {
public:
    explicit EndScene(std::string kind);
    void _ready() override;
    void _update(float delta) override;
    void _draw() override;

private:
    std::string _kind;
    float _clock = 0;
};

} // namespace game
