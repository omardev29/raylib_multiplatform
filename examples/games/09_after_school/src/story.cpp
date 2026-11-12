// The story so far, kept with rmp::save, and what follows from it: how the
// evening ends, and how late the light says it is.

#include "school.h"

#include <rmp/app.h>
#include <rmp/save.h>

#include <algorithm>
#include <string>

namespace game {

namespace {

constexpr std::string_view SLOT = "after_school";
constexpr float MINUTES_AN_HOUR = 60;
// The light at seven, over everything: the afternoon fades towards it.
constexpr Color NIGHTFALL = { 52, 28, 96, 120 };

} // namespace

Story &story() { return rmp::global<Story>(); }

void new_story() { story() = Story{}; }

bool has_saved_story() { return rmp::save::exists(SLOT); }

void load_story() {
    rmp::Value v;
    rmp::save::read(SLOT, v);
    Story &s = story();
    s.hour = std::clamp(v["hour"].as_int(FIRST_HOUR), FIRST_HOUR, FESTIVAL_HOUR);
    s.minutes = std::clamp(v["minutes"].as_float(0), 0.0f, MINUTES_AN_HOUR - 1);
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
    v["minutes"] = s.minutes;
    v["with"] = s.with;
    v["met_mika"] = s.met_mika;
    v["met_sora"] = s.met_sora;
    v["met_aki"] = s.met_aki;
    v["met_kai"] = s.met_kai;
    v["raced"] = s.raced;
    v["flyer"] = s.flyer;
    rmp::save::write(SLOT, v);
}

void forget_story() { rmp::save::remove(SLOT); }

std::string evening() {
    const Story &s = story();
    if (s.with == "mika") return "festival";
    if (s.with == "sora") return "concert";
    return "home";
}

Color dusk(const Story &at) {
    const float hours =
        static_cast<float>(at.hour - FIRST_HOUR) + at.minutes / MINUTES_AN_HOUR;
    const float gone = std::clamp(hours / (FESTIVAL_HOUR - FIRST_HOUR), 0.0f, 1.0f);
    // Slow at first, then quickly: the last hour is the one that darkens.
    return Fade(NIGHTFALL, gone * gone * NIGHTFALL.a / 255.0f);
}

} // namespace game
