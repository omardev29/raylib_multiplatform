// The town: the map, you, whoever LDtk put in it, and the conversations.
//
// Every person is an entity of world.ldtk with a Talker: their `node` of
// school.yarn, their `look` (which .aseprite walks around, which portraits
// the Stage shows). When a conversation is loaded the game hands it what it
// needs: the story's variables, bound (the script's $hour IS story().hour),
// has_item(), and the two commands, <<walk who place>> -- which the dialogue
// waits on until they arrive -- and <<ending kind>>.

#include "school.h"

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/audio.h>
#include <rmp/behavior.h>
#include <rmp/dialogue.h>
#include <rmp/input.h>
#include <rmp/tilemap.h>
#include <rmp/ui.h>

#include <array>
#include <cmath>
#include <map>
#include <string>
#include <utility>

namespace game {

namespace {

constexpr float SPEED = 70; // yours, in world units a second
constexpr float STROLL = 40; // theirs, walking where the script sends them
constexpr float ARRIVED = 2; // close enough to a place
constexpr float MINUTES_A_SECOND = 3; // the afternoon's pace
constexpr float PROMPT_LIFT = -20; // the "Talk" bubble over a head
constexpr float KAI_SECONDS = 4; // Kai's race: say yes in time

// Where a <<walk>> goes, by the names LDtk gives them, and who is in town.
struct Town {
    std::map<std::string, Vector2, std::less<>> places;
    std::map<std::string, rmp::Handle<rmp::Object>, std::less<>> people;
};
Town &town() { return rmp::global<Town>(); }

// Someone the script walks somewhere: TopDown's animation tags, by hand.
struct Stroll {
    Vector2 to{};
    bool going = false;
    [[nodiscard]] bool arrived() const { return !going; }

    void _update(rmp::Object &self, float delta) {
        if (!going) return;
        const Vector2 d{ to.x - self.position.x, to.y - self.position.y };
        const float far = std::hypot(d.x, d.y);
        if (far <= ARRIVED) {
            going = false;
            self.sprite.play("idle_s");
            return;
        }
        const float step = std::min(far, STROLL * delta);
        self.position.x += d.x / far * step;
        self.position.y += d.y / far * step;
        const bool across = std::abs(d.x) > std::abs(d.y);
        self.sprite.play(across ? (d.x > 0 ? "walk_e" : "walk_w")
                                : (d.y > 0 ? "walk_s" : "walk_n"));
    }
};

// You stand facing where you last walked.
struct Standing {
    void _update(rmp::Object &self, float /*delta*/) {
        const auto walk = self.get<rmp::behavior::TopDown>();
        if (!walk || self.velocity.x != 0 || self.velocity.y != 0) return;
        const int sector = walk->sector();
        static constexpr std::array<std::string_view, 8> IDLE{
            "idle_e", "idle_e", "idle_n", "idle_w", "idle_w", "idle_w", "idle_s", "idle_e"
        };
        self.sprite.play(IDLE[static_cast<std::size_t>(sector)]);
    }
};

// What the story's conversations need from the game, given to each one when
// it is loaded.
void prepare(rmp::Dialogue &talk, TownScene &scene) {
    Story &s = story();
    talk.bind("$hour", s.hour);
    talk.bind("$with", s.with);
    talk.bind("$met_mika", s.met_mika);
    talk.bind("$met_sora", s.met_sora);
    talk.bind("$met_aki", s.met_aki);
    talk.bind("$met_kai", s.met_kai);
    talk.bind("$raced", s.raced);
    // Whether you carry the item: "flyer" is the only one in town.
    talk.on_function("has_item", [](std::string item) -> bool {
        return item == "flyer" && story().flyer;
    });
    // Walks a person to one of the map's places; the dialogue waits for them.
    talk.on_command("walk", [&talk](std::string who, std::string where) {
        const auto person = town().people.find(who);
        const auto place = town().places.find(where);
        if (person == town().people.end() || place == town().places.end()) return;
        if (const auto o = person->second.get()) {
            auto stroll = o->get<Stroll>();
            stroll->to = place->second;
            stroll->going = true;
            const rmp::Handle<rmp::Object> h = person->second;
            talk.wait_until([h] {
                const auto p = h.get();
                return !p || p->get<Stroll>()->arrived();
            });
        }
    });
    // The evening's end: festival, concert or home.
    talk.on_command("ending",
                    [&scene](std::string kind) { scene.ending(std::move(kind)); });
}

} // namespace

void TownScene::_ready() {
    background = Color{ 109, 190, 106, 255 };
    rmp::input::action("pause", KEY_ESCAPE, KEY_P, GAMEPAD_BUTTON_MIDDLE_RIGHT);
    rmp::input::action("interact", KEY_E, KEY_SPACE, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    build();
}

void TownScene::build() {
    town() = Town{};
    map = rmp::assets::load_map("world.ldtk", "Town");
    auto &you = spawn({ .shape = rmp::rect({ 10, 8 }) });
    you.sprite.sheet = rmp::assets::load_sheet("you.aseprite");
    you.sprite.play("idle_n");
    you.solid = true;
    you.layer = 5;
    you.add<rmp::behavior::TopDown>(
        { .speed = SPEED,
          .eight_way = true,
          .idle = "",
          .walk = "walk",
          .suffixes = { "e", "e", "n", "w", "w", "w", "s", "e" } });
    you.add<Standing>();
    _you = you.handle();

    map.on_object("Player", [this](rmp::Scene &, const rmp::MapObject &o) {
        if (const auto y = _you.get()) y->position = o.position;
    });
    map.on_object("Place", [](rmp::Scene &, const rmp::MapObject &o) {
        town().places[std::string(o.property_string("name"))] = o.position;
    });
    map.on_object("Person", [this](rmp::Scene &scene, const rmp::MapObject &o) {
        auto &person =
            scene.spawn({ .position = o.position, .shape = rmp::rect({ 10, 8 }) });
        const std::string look(o.property_string("look", "mika"));
        person.sprite.sheet = rmp::assets::load_sheet(look + ".aseprite");
        person.sprite.play("idle_s");
        person.solid = true;
        person.layer = 4;
        person.add<Stroll>();
        rmp::behavior::Talker talker = rmp::behavior::Talker::from(o);
        talker.player = _you;
        talker.on_load = [this](rmp::Dialogue &d) { prepare(d, *this); };
        const rmp::Handle<rmp::Object> me = person.handle();
        talker.on_talk = [this, me](rmp::Dialogue &) { talking(me); };
        talker.on_done = [this] { done_talking(); };
        person.add<rmp::behavior::Talker>(std::move(talker));
        town().people[std::string(o.property_string("name", "Mika"))] = me;
    });
    map.on_object("Home", [this](rmp::Scene &scene, const rmp::MapObject &o) {
        auto &home = scene.spawn({ .position = o.position });
        rmp::behavior::Talker talker = rmp::behavior::Talker::from(o);
        talker.player = _you;
        talker.action = ""; // walking onto it is enough
        talker.on_load = [this](rmp::Dialogue &d) { prepare(d, *this); };
        const rmp::Handle<rmp::Object> me = home.handle();
        talker.on_talk = [this, me](rmp::Dialogue &) { talking(me); };
        talker.on_done = [this] { done_talking(); };
        home.add<rmp::behavior::Talker>(std::move(talker));
    });
    map.on_object("Flyer", [](rmp::Scene &scene, const rmp::MapObject &o) {
        if (story().flyer) return;
        auto &flyer = scene.spawn({ .position = o.position, .shape = rmp::circle(6) });
        flyer.sprite.texture = rmp::assets::load_texture("tiles.png");
        flyer.sprite.source = Rectangle{ 11 * TILE, 13 * TILE, TILE, TILE }; // a poster
        flyer.add<rmp::behavior::Collectible>(
            { .sound = "switch", .on_collect = [](rmp::Object &, rmp::Object &) {
                 story().flyer = true;
                 rmp::ui::notify("You found a festival flyer!");
             } });
    });
    map.spawn_objects(*this);

    camera.follow = _you;
    camera.limits = map.bounds();
    camera.smoothing = 6;
    camera.zoom = ZOOM;
}

void TownScene::talking(rmp::Handle<rmp::Object> speaker) {
    _speaker = speaker;
    _stage.ours = {};
    // Kai's race: a choice with a clock on it.
    const auto o = speaker.get();
    const auto t =
        o ? o->get<rmp::behavior::Talker>() : rmp::Ref<rmp::behavior::Talker>{};
    _stage.flow.choice_seconds = t && t->node == "Kai" ? KAI_SECONDS : 0;
    _stage.flow.default_choice = 1;
    if (const auto y = _you.get()) y->enable<rmp::behavior::TopDown>(false);
}

void TownScene::done_talking() {
    _speaker = {};
    if (const auto y = _you.get()) y->enable<rmp::behavior::TopDown>(true);
    save_story();
    if (!_ending.empty()) {
        forget_story();
        rmp::Scene::change<EndScene>(_ending);
    }
}

void TownScene::ending(std::string kind) { _ending = std::move(kind); }

void TownScene::_update(float delta) {
    if (rmp::input::just_pressed("pause") && !_speaker) {
        rmp::Scene::push<PauseScene>();
        return;
    }
    // The afternoon goes on while you walk, not while you talk.
    if (!_speaker) {
        Story &s = story();
        s.minutes += delta * MINUTES_A_SECOND;
        while (s.minutes >= 60) {
            s.minutes -= 60;
            ++s.hour;
        }
    }
}

void TownScene::_draw() {
    const Story &s = story();
    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT, .padding = 14 });
    rmp::ui::panel([&] {
        rmp::ui::text(
            rmp::Text("{hour}:{minutes} pm",
                      { { "hour", s.hour - 12 },
                        { "minutes", TextFormat("%02d", static_cast<int>(s.minutes)) } }),
            { .wrap = false });
        rmp::ui::text(s.flyer ? rmp::Text("A festival flyer")
                              : rmp::Text("The festival is at seven"),
                      { .color = rmp::ui::ColorRole::MUTED, .wrap = false });
    });
    rmp::ui::end();

    if (const auto speaker = _speaker.get()) {
        auto talker = speaker->get<rmp::behavior::Talker>();
        rmp::ui::begin({ .placement = rmp::ui::Align::BOTTOM_CENTER, .grow = true });
        if (talker) _stage.show(talker->conversation());
        rmp::ui::end();
        return;
    }
    // "Talk" over whoever is close enough.
    for (const auto &[name, handle] : town().people) {
        const auto o = handle.get();
        const auto t =
            o ? o->get<rmp::behavior::Talker>() : rmp::Ref<rmp::behavior::Talker>{};
        if (!t || !t->in_range()) continue;
        rmp::ui::begin();
        rmp::ui::at({ o->position.x, o->position.y + PROMPT_LIFT }, [] {
            rmp::ui::panel([] { rmp::ui::text("E: Talk", { .wrap = false }); });
        });
        rmp::ui::end();
    }
}

} // namespace game
