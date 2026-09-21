#pragma once
// Plain data. This is the whole model, and it is the point of the example: the
// controls take a pointer to a field of this and write to it. Delete the UI
// and your settings still exist; they were never ours.
struct Settings {
    bool fullscreen = false;
    bool vsync = true;
    bool subtitles = false;
    float master = 0.8f;
    float music = 0.5f;
    float sensitivity = 0.35f;
    int quality = 1;
    int language = 0;
    char player[24] = "Player";
};

inline const char *const kQuality[] = { "Low", "Medium", "High", "Ultra" };
inline const char *const kLanguage[] = { "English", "Espanol", "Francais" };
