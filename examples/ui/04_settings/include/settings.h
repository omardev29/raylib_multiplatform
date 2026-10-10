#pragma once
// Plain data. This is the whole model, and it is the point of the example: the
// controls take a field of this, by reference, and write to it. Delete the UI
// and your settings still exist; they were never ours.

#include <array>
#include <string>
#include <string_view>

struct Settings {
    bool fullscreen = false;
    bool vsync = true;
    bool subtitles = false;
    float master = 0.8f;
    float music = 0.5f;
    float sensitivity = 0.35f;
    int quality = 1;
    int language = 0;
    std::string player; // "Player" until the player says otherwise: see on_ready()
};

// What the two dropdowns offer. An array the dropdown reads where it is, so
// nobody has to tell it how long the list is.
inline constexpr std::array<std::string_view, 4> QUALITY{ "Low", "Medium", "High",
                                                          "Ultra" };
inline constexpr std::array<std::string_view, 3> LANGUAGE{ "English", "Espanol",
                                                           "Francais" };
