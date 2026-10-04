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

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
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
constexpr std::array<const char *, 4> EXTENSIONS = { ".wav", ".ogg", ".mp3", ".qoa" };

// What counts as "this name already says its format". Only audio: "ui.click"
// is a name with a dot in it, not a file in a format called ".click". FLAC is
// here so that "theme.flac" is taken as asked and then says it cannot be
// decoded, instead of turning into a search for theme.flac.wav; XM and MOD
// because music() streams them.
constexpr std::array<const char *, 7> KNOWN_EXTENSIONS = { ".wav", ".ogg",  ".mp3",
                                                           ".qoa", ".flac", ".xm",
                                                           ".mod" };

// Four voices per effect, all of them ALIASES. The loaded sound itself is
// never played: it is the resource table's, and a game that loaded the same
// file with rmp::assets::load_sound() holds that very ::Sound -- every volume,
// pitch and pan a play() set would land on the game's copy. An alias shares
// the samples and has its own settings, so a voice costs a few bytes rather
// than a second copy of the audio. Four because that is what a burst of the
// same effect needs before the oldest voice being cut is inaudible anyway.
constexpr int VOICES = 4;

struct Voice {
    ::Sound sound{};
    std::uint64_t started = 0; // the play it last started for; 0 = never
    float volume = 1.0f; // this play's own volume, before the SFX bus
};

struct Effect {
    std::string name; // as asked for, which is the cache key
    rmp::Sound base; // empty when the name resolved to nothing
    std::array<Voice, VOICES> voices{};
    int voice_count = 0;
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
    float master = RMP_AUDIO_MASTER;
    float music = RMP_AUDIO_MUSIC;
    float sfx = RMP_AUDIO_SFX;

    detail::DeviceOpener opener = nullptr;
    bool attempted = false;
    bool ready = false;
    // Whether WE called InitAudioDevice(). A game that opened the device
    // itself (older code, a raylib example pasted in) closes it itself, in
    // its stop hook; closing it for them would pull the mixer out from under
    // every raw Sound that hook still has to unload.
    bool opened_by_us = false;
    int attempts = 0;

    std::vector<Effect> effects;
    Track track;
    std::vector<std::string> missing_music; // asked for, and nothing playable
    std::uint64_t plays = 0;
};

State &state() {
    static State s;
    return s;
}

// The real opener. Records whether it was the one that opened the device.
bool open_for_real() {
    if (!IsAudioDeviceReady()) {
        InitAudioDevice();
        state().opened_by_us = IsAudioDeviceReady();
    }
    return IsAudioDeviceReady();
}

bool real_device() { return state().ready && state().opener == nullptr; }

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

// A voice that is not busy, making a new alias while there is room, and
// otherwise the one that has been playing LONGEST. By counting starts rather
// than rotating: a rotation that skips voices that happened to be free cuts
// a voice that started a moment ago while an older one plays on.
Voice &pick_voice(Effect &e) {
    for (int i = 0; i < e.voice_count; i++) {
        Voice &v = e.voices[static_cast<std::size_t>(i)];
        if (!IsSoundPlaying(v.sound)) return v;
    }
    if (e.voice_count < VOICES) {
        Voice &v = e.voices[static_cast<std::size_t>(e.voice_count++)];
        v.sound = LoadSoundAlias(e.base);
        return v;
    }
    return *std::ranges::min_element(e.voices, {}, &Voice::started);
}

float &bus_ref(Bus bus) {
    if (bus == Bus::MUSIC) return state().music;
    if (bus == Bus::SFX) return state().sfx;
    return state().master; // MASTER, and anything cast into the enum by hand
}

bool is_missing_music(std::string_view name) {
    const std::vector<std::string> &m = state().missing_music;
    return std::ranges::find(m, name) != m.end();
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
    // A leading dot is a hidden file's name, not an extension; and only an
    // audio extension is one here -- see KNOWN_EXTENSIONS.
    if (dot != std::string_view::npos && dot > 0) {
        std::string ext(file.substr(dot));
        for (char &c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char *known : KNOWN_EXTENSIONS) {
            if (ext == known) {
                out.emplace_back(name);
                return out;
            }
        }
    }
    out.reserve(EXTENSIONS.size());
    for (const char *ext : EXTENSIONS) out.push_back(std::string(name) + ext);
    return out;
}

float clamp_volume(float value, float previous) {
    if (std::isnan(value)) return previous;
    return value < 0 ? 0.0f : (value > 1 ? 1.0f : value);
}

float effect_volume(float per_play, float sfx_bus) {
    return clamp_volume(per_play, 1.0f) * clamp_volume(sfx_bus, 1.0f);
}

float raylib_pan(float pan) {
    // raylib 6 pans from -1 (left) through 0 (centre) to 1 (right), and so
    // does PlayOptions. raylib 5 used 0..1 with 0.5 in the middle; passing a
    // 0..1 value to raylib 6 puts "centre" three quarters to the right.
    if (std::isnan(pan)) return 0.0f;
    return pan < -1 ? -1.0f : (pan > 1 ? 1.0f : pan);
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

bool opened_by_us() { return state().opened_by_us; }

float music_time() {
    const State &s = state();
    return (s.ready && s.track.loaded) ? GetMusicTimePlayed(s.track.music) : 0.0f;
}

void update() {
    State &s = state();
    if (!s.ready || !s.track.loaded) return;
    UpdateMusicStream(s.track.music);
}

void shutdown() {
    State &s = state();
    if (s.ready) {
        for (Effect &e : s.effects) {
            for (int i = 0; i < e.voice_count; i++) {
                UnloadSoundAlias(e.voices[static_cast<std::size_t>(i)].sound);
            }
            e.voice_count = 0;
        }
    }
    // The cached rmp::Sound handles go with the vector: their slots in the
    // resource table are released here, and the table unloads the samples.
    s.effects.clear();
    s.missing_music.clear();
    unload_track(s.track);
}

void close_device() {
    State &s = state();
    if (s.ready && s.opener == nullptr && s.opened_by_us && IsAudioDeviceReady()) {
        CloseAudioDevice();
    }
    s.ready = false;
    s.attempted = false;
    s.opened_by_us = false;
}

void reset_for_tests() {
    State &s = state();
    // Nothing here calls raylib: a test that swapped the opener never had a
    // real device, and the vectors are empty because nothing could load.
    s.effects.clear();
    s.missing_music.clear();
    s.track = Track{};
    s.master = RMP_AUDIO_MASTER;
    s.music = RMP_AUDIO_MUSIC;
    s.sfx = RMP_AUDIO_SFX;
    s.opener = nullptr;
    s.attempted = false;
    s.ready = false;
    s.opened_by_us = false;
    s.attempts = 0;
    s.plays = 0;
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

    Voice &voice = pick_voice(*e);
    voice.started = ++state().plays;
    voice.volume = detail::clamp_volume(options.volume, 1.0f);
    SetSoundVolume(voice.sound, detail::effect_volume(voice.volume, state().sfx));
    // A pitch of 0 or less is silence or a crash depending on the backend; a
    // NaN or an infinity is both. Any of them reads as "no change".
    SetSoundPitch(voice.sound,
                  options.pitch > 0 && std::isfinite(options.pitch) ? options.pitch
                                                                    : 1.0f);
    SetSoundPan(voice.sound, detail::raylib_pan(options.pan));
    PlaySound(voice.sound);
}

void music(std::string_view name, bool loop) {
    State &s = state();
    if (name.empty()) {
        stop_music();
        return;
    }
    // The same track again is not a restart while it plays: a scene that
    // asks for its music in _ready() does not jump the song back every time
    // it is re-entered. One that has finished -- a jingle played without
    // looping -- starts again, which is what asking for it means.
    if (s.track.loaded && s.track.name == name) {
        s.track.music.looping = loop;
        if (s.ready && !IsMusicStreamPlaying(s.track.music)) {
            StopMusicStream(s.track.music); // rewinds
            PlayMusicStream(s.track.music);
        }
        return;
    }
    if (!detail::ensure_device()) return;
    // A name that has already come to nothing is not looked for again: the
    // search, and the full read of a file that does not decode, would repeat
    // on every call. And whatever is playing keeps playing.
    if (is_missing_music(name)) return;

    const std::string file = resolve(name);
    if (file.empty()) {
        s.missing_music.emplace_back(name);
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
    if (!next.bytes.empty()) {
        const char *ext = GetFileExtension(file.c_str());
        next.music = LoadMusicStreamFromMemory(ext, next.bytes.data(),
                                               static_cast<int>(next.bytes.size()));
    }
    if (next.bytes.empty() || next.music.stream.buffer == nullptr) {
        // The bytes were there and did not stream: a failed load, the way a
        // missing file is one, so the CI boot gate sees a corrupt song.
        // load_data() has already counted an empty read itself.
        if (!next.bytes.empty()) rmp::assets::detail::loads.failed++;
        s.missing_music.emplace_back(name);
        RMP_REPORT_ONCE_KEYED(file.c_str(),
                              "AUDIO: \"%s\" is in resources/ but could not be streamed "
                              "(raylib here reads .wav, .ogg, .mp3 and .qoa)",
                              file.c_str());
        return;
    }
    // Only now, with the new track ready, does the old one go.
    unload_track(s.track);
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
    // Applied now to what is already playing, but never opening the device to
    // do it: a settings screen in a game that has made no sound yet stays
    // silent and costs nothing.
    if (!real_device()) return;
    State &s = state();
    if (bus == Bus::MASTER) SetMasterVolume(s.master);
    if (bus == Bus::MUSIC && s.track.loaded) SetMusicVolume(s.track.music, s.music);
    if (bus == Bus::SFX) {
        // Every voice keeps the volume its own play asked for, so a slider
        // moved while effects ring out scales them instead of flattening them.
        for (Effect &e : s.effects) {
            for (int i = 0; i < e.voice_count; i++) {
                const Voice &v = e.voices[static_cast<std::size_t>(i)];
                if (IsSoundPlaying(v.sound)) {
                    SetSoundVolume(v.sound, detail::effect_volume(v.volume, s.sfx));
                }
            }
        }
    }
}

float volume(Bus bus) { return bus_ref(bus); }

bool available() { return state().ready; }

} // namespace rmp::audio
