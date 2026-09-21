#pragma once
// Where the game currently is. Note that this is a plain variable of YOURS:
// the UI is immediate mode, so it holds no state and there is nothing to keep
// in sync. What you draw each frame is decided by your own data.
enum class Screen { MENU, OPTIONS, CONFIRM, PLAYING };

struct MenuState {
    Screen screen = Screen::MENU;
    int score = 0;
    bool music = true;
};
inline MenuState g_state;

// One function per screen, one file per screen under src/screens/.
void main_menu();
void options_menu();
void confirm_quit();
void hud();
