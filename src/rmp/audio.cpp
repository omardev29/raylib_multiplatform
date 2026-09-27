// ---------------------------------------------------------------------------
// rmp::audio: the device opened lazily, effects by logical name with a small
// pool of voices each, one streamed music track, and three volume buses.
//
// The surface is include/rmp/audio.h. What is here is the bookkeeping that
// keeps it one line at the call site: resolving "coin" into whichever file
// exists, loading it once, giving it up to four voices so overlapping plays
// overlap instead of restarting, feeding the music stream every frame, and
// releasing all of it on the way out before the device goes.
// ---------------------------------------------------------------------------

#include <rmp/assets.h>
#include <rmp/audio.h>

#include "audio_internal.h"
#include "internal.h"

#include <raylib.h>

#include <array>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace rmp::audio {

namespace {

// The order a logical name is tried in. WAV first because it is what
// generators and editors write by default and needs no decoder; then the
// compressed ones by how common they are in games; QOA last, raylib's own.
// No FLAC: raylib's config.h ships SUPPORT_FILEFORMAT_FLAC as 0, so a .flac
// would be found and then fail to decode, which is worse than not found.
constexpr std::array<const char *, 4> kExtensions = { ".wav", ".ogg", ".mp3", ".qoa" };

// One base sound plus up to three aliases: four voices. An alias shares the
// base's sample buffer, so the extra voices cost a few bytes each rather than
// a second copy of the audio. Four because that is what a burst of the same
// effect needs before the oldest voice being cut is inaudible anyway.
constexpr int kMaxAliases = 3;

struct Effect {
    std::string name; // as asked for, which is the cache key
    rmp::Sound base; // empty when the name resolved to nothing
    std::array<::Sound, kMaxAliases> aliases{};
    int alias_count = 0;
    int next_steal = 0; // round robin, when every voice is busy
};

// The music track. Streamed, so it keeps the bytes it streams from: raylib's
// decoders read the memory LoadMusicStreamFromMemory() was given for as long
// as the stream lives, and freeing it early is a read of freed memory that
// sounds like static for a frame and then crashes.
struct Track {
    std::string name;
    std::vector<unsigned char> bytes;
    ::Music music{};
    bool loaded = false;
};

struct State {
    float master = APP_AUDIO_MASTER;
    float music = APP_AUDIO_MUSIC;
    float sfx = APP_AUDIO_SFX;

    detail::DeviceOpener opener = nullptr;
    bool attempted = false;
    bool ready = false;
    int attempts = 0;

    std::vector<Effect> effects;
    Track track;
};

State &state() {
    static State s;
    return s;
}

bool open_for_real() {
    // A game that opened the device itself (older code, or a raylib example
    // pasted in) is not opened twice: raylib would warn and keep the first.
    if (!IsAudioDeviceReady()) InitAudioDevice();
    return IsAudioDeviceReady();
}

// The first name the logical name expands to that exists, or "".
std::string resolve(std::string_view name) {
    for (const std::string &candidate : detail::candidates(name)) {
        if (rmp::assets::detail::resource_exists(candidate.c_str())) return candidate;
    }
    return {};
}

void unload_track(Track &track) {
    if (track.loaded && state().ready) {
        StopMusicStream(track.music);
        UnloadMusicStream(track.music);
    }
    track = Track{};
}

Effect *find_effect(std::string_view name) {
    for (Effect &e : state().effects) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

// A voice that is not busy, making a new alias if there is room, and taking
// the oldest when there is not. By value, because raylib's sound functions all
// take a Sound by value: it is a handle to a buffer, not the buffer.
::Sound pick_voice(Effect &e) {
    const ::Sound base = e.base;
    if (!IsSoundPlaying(base)) return base;
    for (int i = 0; i < e.alias_count; i++) {
        const ::Sound alias = e.aliases[static_cast<std::size_t>(i)];
        if (!IsSoundPlaying(alias)) return alias;
    }
    if (e.alias_count < kMaxAliases) {
        const ::Sound alias = LoadSoundAlias(base);
        e.aliases[static_cast<std::size_t>(e.alias_count++)] = alias;
        return alias;
    }
    // Every voice busy: the oldest is cut. At four overlapping copies of the
    // same effect, which one stopped is not something anybody can hear.
    const int voice = e.next_steal;
    e.next_steal = (e.next_steal + 1) % (kMaxAliases + 1);
    return voice == 0 ? base : e.aliases[static_cast<std::size_t>(voice - 1)];
}

float &bus_ref(Bus bus) {
    if (bus == Bus::MUSIC) return state().music;
    if (bus == Bus::SFX) return state().sfx;
    return state().master; // MASTER, and anything cast into the enum by hand
}

} // namespace

// ---------------------------------------------------------------------------
// The pure half, tested without a device
// ---------------------------------------------------------------------------

namespace detail {

std::vector<std::string> candidates(std::string_view name) {
    std::vector<std::string> out;
    if (name.empty()) return out;

    // The extension is in the FILE part. Looking for a dot anywhere would read
    // "levels/v1.2/coin" as a file called "2/coin" with an extension.
    const std::size_t slash = name.find_last_of("/\\");
    const std::string_view file =
        slash == std::string_view::npos ? name : name.substr(slash + 1);
    const std::size_t dot = file.find_last_of('.');
    // A leading dot is a hidden file's name, not an extension.
    if (dot != std::string_view::npos && dot > 0 && dot + 1 < file.size()) {
        out.emplace_back(name);
        return out;
    }
    out.reserve(kExtensions.size());
    for (const char *ext : kExtensions) out.push_back(std::string(name) + ext);
    return out;
}

float clamp_volume(float value, float previous) {
    if (std::isnan(value)) return previous;
    return value < 0 ? 0.0f : (value > 1 ? 1.0f : value);
}

float effect_volume(float per_play, float sfx_bus) {
    return clamp_volume(per_play, 1.0f) * clamp_volume(sfx_bus, 1.0f);
}

void set_device_opener(DeviceOpener opener) { state().opener = opener; }

bool ensure_device() {
    State &s = state();
    if (s.ready) return true;
    if (s.attempted) return false; // once, and a failure is remembered
    s.attempted = true;
    s.attempts++;
    const DeviceOpener open = s.opener != nullptr ? s.opener : open_for_real;
    s.ready = open();
    if (!s.ready) {
        // A plain log and not RMP_REPORT_ONCE: that one is for mistakes in the
        // game, and [dev] strict makes it abort a debug build. A machine with
        // no sound is not a mistake, and a strict build on a CI runner must
        // still run. Once is guaranteed by `attempted`, above.
        TraceLog(LOG_WARNING,
                 "AUDIO: no sound device on this machine; sound is off and "
                 "every rmp::audio call is a silent no-op from here on");
        return false;
    }
    // The opener might be a test's; only the real device has a mixer to set.
    if (s.opener == nullptr) SetMasterVolume(s.master);
    return true;
}

int open_attempts() { return state().attempts; }

void update() {
    State &s = state();
    if (!s.ready || !s.track.loaded) return;
    UpdateMusicStream(s.track.music);
}

void shutdown() {
    State &s = state();
    if (s.ready) {
        for (Effect &e : s.effects) {
            for (int i = 0; i < e.alias_count; i++) {
                UnloadSoundAlias(e.aliases[static_cast<std::size_t>(i)]);
            }
            e.alias_count = 0;
        }
    }
    // The cached rmp::Sound handles go with the vector: their slots in the
    // resource table are released here, and the table unloads the samples.
    s.effects.clear();
    unload_track(s.track);
}

void close_device() {
    State &s = state();
    if (s.ready && s.opener == nullptr && IsAudioDeviceReady()) CloseAudioDevice();
    s.ready = false;
    s.attempted = false;
}

void reset_for_tests() {
    State &s = state();
    // Nothing here calls raylib: a test that swapped the opener never had a
    // real device, and the vectors are empty because nothing could load.
    s.effects.clear();
    s.track = Track{};
    s.master = APP_AUDIO_MASTER;
    s.music = APP_AUDIO_MUSIC;
    s.sfx = APP_AUDIO_SFX;
    s.opener = nullptr;
    s.attempted = false;
    s.ready = false;
    s.attempts = 0;
}

} // namespace detail

// ---------------------------------------------------------------------------
// The public half
// ---------------------------------------------------------------------------

void play(std::string_view name, const PlayOptions &options) {
    if (name.empty() || !detail::ensure_device()) return;

    Effect *e = find_effect(name);
    if (e == nullptr) {
        State &s = state();
        s.effects.push_back(Effect{});
        e = &s.effects.back();
        e->name = std::string(name);
        const std::string file = resolve(name);
        if (!file.empty()) e->base = rmp::assets::load_sound(file);
        // Cached as missing either way, so a footstep with no file does not
        // cost a directory search sixty times a second; said once, and the two
        // failures said differently, because they are fixed differently.
        if (file.empty()) {
            RMP_REPORT_ONCE_KEYED(e->name.c_str(),
                                  "AUDIO: no sound called \"%s\" in resources/ (tried "
                                  "the name as given, then .wav, .ogg, .mp3, .qoa)",
                                  e->name.c_str());
        } else if (!e->base.valid()) {
            RMP_REPORT_ONCE_KEYED(
                e->name.c_str(),
                "AUDIO: \"%s\" is in resources/ but could not be decoded "
                "(raylib here reads .wav, .ogg, .mp3 and .qoa)",
                file.c_str());
        }
    }
    if (!e->base.valid()) return;

    const ::Sound voice = pick_voice(*e);
    SetSoundVolume(voice, detail::effect_volume(options.volume, state().sfx));
    // A pitch of 0 or less is silence or a crash depending on the backend; a
    // NaN is both. Either reads as "no change".
    SetSoundPitch(voice, options.pitch > 0 ? options.pitch : 1.0f);
    SetSoundPan(voice, detail::clamp_volume(options.pan, 0.5f));
    PlaySound(voice);
}

void music(std::string_view name, bool loop) {
    State &s = state();
    if (name.empty()) {
        stop_music();
        return;
    }
    // The same track again is not a restart: a scene that asks for its music
    // in _ready() does not jump the song back every time it is re-entered.
    if (s.track.loaded && s.track.name == name) {
        s.track.music.looping = loop;
        return;
    }
    if (!detail::ensure_device()) return;

    unload_track(s.track);
    const std::string file = resolve(name);
    if (file.empty()) {
        RMP_REPORT_ONCE_KEYED(std::string(name).c_str(),
                              "AUDIO: no music called \"%s\" in resources/",
                              std::string(name).c_str());
        return;
    }

    // Through load_data, which serves the pack and loose files alike, and the
    // bytes stay with the track for as long as it streams from them.
    Track next;
    next.name = std::string(name);
    next.bytes = rmp::assets::load_data(file);
    if (next.bytes.empty()) return;
    const char *ext = GetFileExtension(file.c_str());
    next.music = LoadMusicStreamFromMemory(ext, next.bytes.data(),
                                           static_cast<int>(next.bytes.size()));
    if (next.music.stream.buffer == nullptr) {
        RMP_REPORT_ONCE_KEYED(file.c_str(),
                              "AUDIO: \"%s\" is in resources/ but could not be streamed "
                              "(raylib here reads .wav, .ogg, .mp3 and .qoa)",
                              file.c_str());
        return;
    }
    next.loaded = true;
    next.music.looping = loop;
    s.track = std::move(next);
    SetMusicVolume(s.track.music, s.music);
    PlayMusicStream(s.track.music);
}

void stop_music() { unload_track(state().track); }

bool music_playing() {
    const State &s = state();
    return s.ready && s.track.loaded && IsMusicStreamPlaying(s.track.music);
}

void set_volume(Bus bus, float volume) {
    float &slot = bus_ref(bus);
    slot = detail::clamp_volume(volume, slot);
    State &s = state();
    // Applied now to what is already playing, but never opening the device to
    // do it: a settings screen in a game that has made no sound yet stays
    // silent and costs nothing.
    if (!s.ready || s.opener != nullptr) return;
    if (bus == Bus::MASTER) SetMasterVolume(s.master);
    if (bus == Bus::MUSIC && s.track.loaded) SetMusicVolume(s.track.music, s.music);
}

float volume(Bus bus) { return bus_ref(bus); }

bool available() { return state().ready; }

} // namespace rmp::audio
