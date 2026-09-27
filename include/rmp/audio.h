#pragma once
// ---------------------------------------------------------------------------
// rmp/audio.h — sound effects and music, by name.
//
//     rmp::audio::play("coin");                          // coin.wav, coin.ogg...
//     rmp::audio::play("hit", { .volume = 0.7f, .pitch = 1.2f });
//     rmp::audio::music("level1");                       // starts, or replaces
//     rmp::audio::set_volume(rmp::audio::Bus::SFX, 0.5f);
//
// NO LIFE CYCLE IN SIGHT, the same way rmp::assets has none. The device opens
// on the first call that needs it -- exactly as rmp::ui opens Clay on its first
// begin() -- so a game with no sound never opens it. The music is streamed, and
// the UpdateMusicStream() it needs every frame is the app's job, not yours. And
// everything is released on the way out, before the device closes, in the
// order that does not crash.
//
// A MACHINE WITH NO SOUND IS NOT AN ERROR. A CI runner, a server, a headless
// box: the device fails to open, the framework says so once, and every call
// here becomes a silent no-op. The game runs -- under [dev] strict too, which
// aborts on mistakes in the game and this is not one.
//
// THE NAMES ARE LOGICAL. "coin" finds coin.wav, coin.ogg, coin.mp3 or
// coin.qoa in resources/ -- in that order, the first that exists -- and
// "coin.ogg" finds exactly that. So a sound can be re-exported in another
// format without touching the code, and the pack and the loose files are the
// same to it, as they are to every other asset.
//
// OVERLAPPING IS THE DEFAULT. Playing the same effect while it is still
// playing starts another voice (up to four) instead of restarting the first,
// so a machine gun sounds like one and not like a stuck record.
//
// On the web, browsers keep audio muted until the player has clicked, touched
// or pressed a key. That is the browser's rule and not something a game can
// get round; a game that starts its music on the title screen is heard from
// the first such input on. A gamepad button does not count -- browsers do not
// treat it as a gesture -- so a gamepad-only player hears nothing until then.
// ---------------------------------------------------------------------------

#include <rmp/config.h>

#include <string_view>

namespace rmp::audio {

// The three volumes a settings screen offers, and what they multiply.
// MASTER scales everything; MUSIC and SFX scale their own on top of it. The
// starting values come from [audio] in raylib_multiplatform.toml.
enum class Bus { MASTER, MUSIC, SFX };

// Declaration order is the order they are written in, as C++20 requires.
struct PlayOptions {
    float volume = 1.0f; // this play only; times the SFX bus, times MASTER
    float pitch = 1.0f; // 2 = an octave up, 0.5 = an octave down
    float pan = 0.0f; // -1 = left, 0 = centre, 1 = right (raylib 6's range)
};

// A sound effect. Loaded the first time it is asked for and kept, so the
// second play costs nothing; a name that does not exist says so once and
// plays nothing.
void play(std::string_view name, const PlayOptions &options = {});

// Start the music, looping, or replace whatever is playing. Asking for the
// track that is already playing is NOT a restart -- a scene that calls
// music("level1") in its _ready() does not jump the song back to the start
// every time the player dies and the scene is changed to itself.
void music(std::string_view name, bool loop = true);
void stop_music();

// Whether music is playing right now. False with no device.
[[nodiscard]] bool music_playing();

// 0..1, clamped; a NaN is ignored rather than stored. Takes effect at once,
// on the music already playing too. Never opens the device: a settings screen
// can show and move the sliders in a game that has made no sound yet.
void set_volume(Bus bus, float volume);
[[nodiscard]] float volume(Bus bus);

// Whether the device is open and working. False until the first sound, and
// false for good on a machine with none -- asking does not open it.
[[nodiscard]] bool available();

} // namespace rmp::audio
