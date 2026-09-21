#pragma once
#include <rmp/assets.h>

struct Item {
    const char *name;
    int count;
    bool equipped;
};

inline Item g_items[] = {
    { "Sword", 1, true }, { "Shield", 1, false }, { "Potion", 12, false },
    { "Rope", 3, false }, { "Torch", 8, false },  { "Map", 1, false },
    { "Key", 2, false },  { "Bread", 5, false },  { "Coin", 240, false },
    { "Gem", 4, false },  { "Bow", 1, false },    { "Arrow", 60, false },
};
inline constexpr int kItemCount = 12;
inline int g_selected = 0;

// An rmp::Texture and not a Texture2D: the handle owns the texture, and it is
// released in on_exit() before the window closes. Copying it into a raw
// Texture2D would have destroyed the only owner on the same line.
inline rmp::Texture g_icon;

// One panel per file under src/panels/.
void inventory_grid();
void item_list();
