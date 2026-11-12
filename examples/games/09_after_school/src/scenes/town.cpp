// The town: the map, you, whoever LDtk put in it, and the conversations.
//
// Everything here is an entity of maps/world.ldtk. A Person is a Talker: their
// `node` of dialogue/school.yarn, their `look` (which sheet walks around, which
// portraits the Stage shows). A Place is where a <<walk who place>> of the
// script goes, and Home is the way out of the afternoon. The trees and the
// street lamps are src/scenery.cpp's, in every scene that shows the town.

#include "school.h"

#include <rmp/app.h>
#include <rmp/assets.h>
#include <rmp/behavior.h>
#include <rmp/dialogue.h>
#include <rmp/input.h>
#include <rmp/tilemap.h>
#include <rmp/ui.h>

#include <cmath>
#include <map>
#include <string>
#include <utility>

namespace game {

namespace {

constexpr float SPEED = 72; // yours, in world units a second
constexpr float STROLL = 44; // theirs, walking where the script sends them
constexpr float ARRIVED = 6; // close enough to a place
constexpr float MINUTES_A_SECOND = 3; // the afternoon's pace
constexpr float MINUTES_AN_HOUR = 60;
constexpr float KAI_SECONDS = 4; // Kai's race: say yes in time
constexpr float CAMERA_DRIFT = 8; // how quickly the view catches up with you
constexpr Vector2 FEET{ 10, 6 }; // what of a person stops at a wall
constexpr Vector2 OVER_HEAD{ 0, -14 }; // where a person's prompt floats
constexpr int ON_THE_GROUND = -1; // drawn under everyone

const Color INK = { 62, 39, 35, 235 };
const Color CREAM = { 255, 246, 226, 255 };

constexpr Rectangle POSTER{ 11 * TILE, 13 * TILE, TILE, TILE }; // in maps/tiles.png

// Where a <<walk>> goes, by the names LDtk gives them, and who is in town.
struct Town {
    std::map<std::string, Vector2, std::less<>> places;
    std::map<std::string, rmp::Handle<rmp::Object>, std::less<>> people;
};
Town &town() { return rmp::global<Town>(); }

bool close_to(Vector2 a, Vector2 b) {
    return std::hypot(a.x - b.x, a.y - b.y) <= ARRIVED;
}

// Someone with a sheet of their own, standing on their feet.
rmp::Object &person(rmp::Scene &scene, Vector2 position, std::string_view look) {
    auto &someone = scene.spawn({ .position = position });
    someone.sprite.sheet =
        rmp::assets::load_sheet("sprites/" + std::string(look) + ".aseprite");
    someone.collider = rmp::rect(FEET);
    someone.solid = true;
    return someone;
}

// Sends `who` to one of the map's places; the dialogue waits until they are there.
void walk(rmp::Dialogue &talk, std::string_view who, std::string_view where) {
    const auto walker = town().people.find(who);
    const auto place = town().places.find(where);
    if (walker == town().people.end() || place == town().places.end()) return;
    const rmp::Handle<rmp::Object> them = walker->second;
    const Vector2 to = place->second;
    if (const auto o = them.get()) {
        if (auto path = o->get<rmp::behavior::Pathfind>()) path->goal = to;
    }
    talk.wait_until([them, to, asked = 0]() mutable {
        const auto o = them.get();
        if (!o || close_to(o->position, to)) return true;
        // With no way there the script goes on, rather than wait for ever.
        const auto path = o->get<rmp::behavior::Pathfind>();
        return ++asked > 2 && (!path || path->arrived());
    });
}

} // namespace

void TownScene::prepare(rmp::Dialogue &talk) {
    Story &s = story();
    talk.bind("$hour", s.hour);
    talk.bind("$with", s.with);
    talk.bind("$met_mika", s.met_mika);
    talk.bind("$met_sora", s.met_sora);
    talk.bind("$met_aki", s.met_aki);
    talk.bind("$met_kai", s.met_kai);
    talk.bind("$raced", s.raced);
    // Whether you carry the item: "flyer" is the only one in town.
    talk.on_function("has_item", [](const std::string &item) -> bool {
        return item == "flyer" && story().flyer;
    });
    // The view goes with whoever the script walks somewhere, until the
    // conversation is over.
    talk.on_command("walk",
                    [this, &talk](const std::string &who, const std::string &where) {
                        walk(talk, who, where);
                        if (const auto walker = town().people.find(who);
                            walker != town().people.end()) {
                            camera.follow = walker->second;
                        }
                    });
    // The evening's end: festival, concert or home.
    talk.on_command("ending", [this](std::string kind) { ending(std::move(kind)); });
}

void TownScene::_ready() {
    background = Color{ 60, 200, 168, 255 };
    rmp::input::action("pause", KEY_ESCAPE, KEY_P, GAMEPAD_BUTTON_MIDDLE_RIGHT);
    rmp::input::action("interact", KEY_E, KEY_SPACE, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    build();
}

void TownScene::build() {
    town() = Town{};
    load_town(*this);
    _seven = rmp::assets::load_dialogue("dialogue/school.yarn");
    prepare(_seven);

    auto &you = person(*this, {}, "you");
    you.add<rmp::behavior::TopDown>({ .speed = SPEED, .idle = "", .walk = "" });
    you.add<Character>({ .facing = { 0, -1 } });
    _you = you.handle();

    map.on_object("Player", [this](rmp::Scene &, const rmp::MapObject &o) {
        if (const auto y = _you.get()) y->position = o.position;
    });
    map.on_object("Place", [](rmp::Scene &, const rmp::MapObject &o) {
        town().places[std::string(o.property_string("name"))] = o.position;
    });
    map.on_object("Person", [this](rmp::Scene &scene, const rmp::MapObject &o) {
        auto &them = person(scene, o.position, o.property_string("look", "mika"));
        // Nobody is pushed out of their conversation.
        them.immovable = true;
        them.add<rmp::behavior::Pathfind>({ .speed = STROLL, .clearance = FEET.x / 2 });
        them.add<Character>();
        rmp::behavior::Talker talker = rmp::behavior::Talker::from(o);
        talker.player = _you;
        talker.on_load = [this](rmp::Dialogue &d) { prepare(d); };
        const rmp::Handle<rmp::Object> me = them.handle();
        talker.on_talk = [this, me](rmp::Dialogue &) { talking(me); };
        talker.on_done = [this] { done_talking(); };
        them.add<rmp::behavior::Talker>(std::move(talker));
        town().people[std::string(o.property_string("name", "Mika"))] = me;
    });
    map.on_object("Home", [this](rmp::Scene &scene, const rmp::MapObject &o) {
        auto &home = scene.spawn({ .position = o.position });
        home.visible = false;
        rmp::behavior::Talker talker = rmp::behavior::Talker::from(o);
        talker.player = _you;
        talker.action = ""; // walking onto it is enough
        talker.on_load = [this](rmp::Dialogue &d) { prepare(d); };
        const rmp::Handle<rmp::Object> me = home.handle();
        talker.on_talk = [this, me](rmp::Dialogue &) { talking(me); };
        talker.on_done = [this] { done_talking(); };
        home.add<rmp::behavior::Talker>(std::move(talker));
    });
    map.on_object("Flyer", [](rmp::Scene &scene, const rmp::MapObject &o) {
        if (story().flyer) return;
        auto &flyer = scene.spawn({ .position = o.position, .shape = rmp::circle(6) });
        flyer.sprite.texture = rmp::assets::load_texture("maps/tiles.png");
        flyer.sprite.source = POSTER;
        flyer.layer = ON_THE_GROUND;
        flyer.add<rmp::behavior::Collectible>(
            { .sound = "audio/switch", .on_collect = [](rmp::Object &, rmp::Object &) {
                 story().flyer = true;
                 rmp::ui::notify("You found a festival flyer!");
             } });
    });
    map.spawn_objects(*this);

    camera.follow = _you;
    camera.limits = map.bounds();
    camera.smoothing = CAMERA_DRIFT;
    camera.zoom = ZOOM;
}

void TownScene::talking(rmp::Handle<rmp::Object> speaker) {
    _speaker = speaker;
    _stage.ours = {};
    const auto them = speaker.get();
    const auto talker =
        them ? them->get<rmp::behavior::Talker>() : rmp::Ref<rmp::behavior::Talker>{};
    // Kai's race: a choice with a clock on it.
    _stage.flow.choice_seconds = talker && talker->node == "Kai" ? KAI_SECONDS : 0;
    _stage.flow.default_choice = 1;
    const auto you = _you.get();
    if (!you) return;
    // You stop where you are, and the two of you look at each other.
    you->enable<rmp::behavior::TopDown>(false);
    you->velocity = {};
    if (them) {
        face(*you, them->position);
        face(*them, you->position);
    }
}

void TownScene::done_talking() {
    _speaker = {};
    camera.follow = _you;
    if (const auto you = _you.get()) you->enable<rmp::behavior::TopDown>(true);
    save_story();
    if (_ending.empty()) return;
    forget_story();
    rmp::Scene::change<EndScene>(std::exchange(_ending, {}));
}

void TownScene::ending(std::string kind) { _ending = std::move(kind); }

// The afternoon goes on while you walk, not while you talk; at seven it is over.
void TownScene::pass_time(float delta) {
    Story &s = story();
    if (s.hour >= FESTIVAL_HOUR) {
        s.hour = FESTIVAL_HOUR;
        s.minutes = 0;
        if (_seven.start("Seven")) talking({});
        return;
    }
    s.minutes += delta * MINUTES_A_SECOND;
    while (s.minutes >= MINUTES_AN_HOUR && s.hour < FESTIVAL_HOUR) {
        s.minutes -= MINUTES_AN_HOUR;
        ++s.hour;
    }
}

void TownScene::_update(float delta) {
    const bool at_seven = _seven.running();
    if (_speaker || at_seven) return;
    // Seven's last line was read: the evening it asked for.
    if (!_ending.empty()) {
        done_talking();
        return;
    }
    if (rmp::input::just_pressed("pause")) {
        rmp::Scene::push<PauseScene>();
        return;
    }
    pass_time(delta);
}

void TownScene::draw_clock() const {
    const Story &s = story();
    rmp::ui::begin({ .placement = rmp::ui::Align::TOP_LEFT, .padding = 10 });
    rmp::ui::panel({ .box = { .gap = 2, .padding = 12 } }, [&] {
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
}

// "Talk" over whoever is close enough to talk to.
void TownScene::draw_prompts() const {
    for (const auto &[name, handle] : town().people) {
        const auto them = handle.get();
        const auto talker =
            them ? them->get<rmp::behavior::Talker>() : rmp::Ref<rmp::behavior::Talker>{};
        if (!talker || !talker->in_range()) continue;
        rmp::ui::begin();
        rmp::ui::at(them->position, { .offset = OVER_HEAD, .id = name }, [] {
            rmp::ui::Label prompt{ .text = "Talk",
                                   .wrap = false,
                                   .style = { .background = INK,
                                              .radius = 6,
                                              .padding_x = 8,
                                              .padding_y = 3,
                                              .text = CREAM } };
            prompt.show();
        });
        rmp::ui::end();
    }
}

void TownScene::_draw() {
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), dusk(story()));
    draw_clock();
    const auto them = _speaker.get();
    auto talker =
        them ? them->get<rmp::behavior::Talker>() : rmp::Ref<rmp::behavior::Talker>{};
    if (!talker && !_seven.running()) {
        draw_prompts();
        return;
    }
    rmp::ui::begin({ .placement = rmp::ui::Align::BOTTOM_CENTER, .grow = true });
    _stage.show(talker ? talker->conversation() : _seven);
    rmp::ui::end();
}

} // namespace game
