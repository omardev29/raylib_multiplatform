// ===========================================================================
// SoundName: the one place rmp::ui names rmp::audio.
//
// A name is written, `.click = "click"`, and its constructor is the link: the
// linker brings this file -- and with it rmp::audio::play(), the sound loader
// and its decoders -- into a game only when the game writes a sound's name. A
// UI with none plays nothing and carries none of it (tools/size_budget.txt).
// ===========================================================================

#include "internal.h"

#include <rmp/audio.h>

#include <string>
#include <string_view>

namespace rmp::ui {

void SoundName::keep(std::string_view name) {
    _name = std::string(name);
    if (detail::has_sound_player()) return;
    detail::set_sound_player([](std::string_view sound, float volume) {
        rmp::audio::play(sound, rmp::audio::PlayOptions{ .volume = volume });
    });
}

} // namespace rmp::ui
