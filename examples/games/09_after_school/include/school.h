#pragma once
// ---------------------------------------------------------------------------
// What every file of the afternoon shares: the story so far, the scenes and
// the look.
// ---------------------------------------------------------------------------

#include <rmp/scene.h>
#include <rmp/ui.h>

#include <string>

namespace game {

inline constexpr float TILE = 16; // the grid of tiles.png and of the town

// How much of the town is on screen: rows of tiles at the [window] size.
inline constexpr float VIEW_HEIGHT = 210;
inline constexpr float ZOOM = RMP_WINDOW_HEIGHT / VIEW_HEIGHT;

// THE STORY SO FAR: what every conversation reads and writes -- the
// $variables of school.yarn are these, bound -- and what Continue restores.
struct Story {
    int hour = 15; // of the afternoon: the festival is at seven
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

// The look of every screen: Kenney's frames and font, a voice for each
// person, sounds on hover and click. Set once, by the title.
void set_look();
// The conversations: one Stage, Persona style, its portraits and voices.
rmp::ui::DialogueStage make_stage();

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

private:
    void build();

    rmp::Handle<rmp::Object> _you;
    rmp::Handle<rmp::Object> _speaker; // whose Talker holds the conversation
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
