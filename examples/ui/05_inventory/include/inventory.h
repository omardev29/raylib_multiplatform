#pragma once
#include <rmp/assets.h>

#include <array>
#include <string_view>

struct Item {
    std::string_view name;
    int count;
    bool equipped;
    std::string_view description; // the list's tooltip
};

inline constexpr int ITEM_COUNT = 12;
inline std::array<Item, ITEM_COUNT> items{ {
    { "Sword", 1, true, "Sharp at one end. Equipped." },
    { "Shield", 1, false, "Stops what the sword does not." },
    { "Potion", 12, false, "Twelve second chances." },
    { "Rope", 3, false, "Thirty feet, more or less." },
    { "Torch", 8, false, "Light for a cave, or for a fuse." },
    { "Map", 1, false, "Of somewhere else, unfortunately." },
    { "Key", 2, false, "Two keys, no doors yet." },
    { "Bread", 5, false, "Restores a little, tastes of a lot." },
    { "Coin", 240, false, "Spend it, or count it." },
    { "Gem", 4, false, "Worth a shop each." },
    { "Bow", 1, false, "Useless without the arrows." },
    { "Arrow", 60, false, "Useless without the bow." },
} };
inline int selected = 0;

// An rmp::Texture and not a Texture2D: the handle owns the texture, and it is
// released in on_exit() before the window closes. Copying it into a raw
// Texture2D would have destroyed the only owner on the same line.
inline rmp::Texture icon;

// One panel per file under src/panels/.
void inventory_grid();
void item_list();
