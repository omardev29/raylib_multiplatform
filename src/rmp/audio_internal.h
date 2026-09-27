#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. The half of rmp::audio the rest of the framework drives,
// and the seams tests/audio_test.cpp uses.
//
// WHAT IS TESTED WITHOUT A DEVICE, AND WHAT IS NOT. There is no sound device
// on a CI runner, and faking one would test the fake: whether a WAV actually
// comes out of the speakers is checked by ear. What does NOT need a device is
// everything that decides what gets played and how loud -- the logical name
// expanding into files, the volume arithmetic, the clamping -- and the one
// property that matters most for a game with no sound at all: that the device
// is opened lazily, once, and never retried every frame after it fails. Those
// are pure functions or a state machine behind a seam, and they are tested.
// ---------------------------------------------------------------------------

#include <string>
#include <string_view>
#include <vector>

namespace rmp::audio::detail {

// What a logical name expands to, in search order: "coin" -> coin.wav,
// coin.ogg, coin.mp3, coin.qoa; "coin.ogg" -> just that. Only an audio
// extension counts (.wav .ogg .mp3 .qoa .flac .xm .mod, any case), and only
// in the FILE part: "v1.2/coin" and "ui.click" are names without one. Empty
// for an empty name.
std::vector<std::string> candidates(std::string_view name);

// The volume a sound effect is set to before MASTER, which raylib applies on
// its own through SetMasterVolume(). Both clamped to 0..1 first.
float effect_volume(float per_play, float sfx_bus);

// A volume as stored: clamped to 0..1, and a NaN replaced by `previous`
// rather than stored, so one bad slider value cannot silence the game.
float clamp_volume(float value, float previous);

// PlayOptions::pan as raylib 6 takes it: -1 left, 0 centre, 1 right, clamped,
// and a NaN is the centre.
float raylib_pan(float pan);

// How the device is opened. The real one calls InitAudioDevice() and asks
// IsAudioDeviceReady(); a test swaps in a function that returns false, which
// is exactly a machine with no sound. nullptr restores the real one.
using DeviceOpener = bool (*)();
void set_device_opener(DeviceOpener opener);

// Open the device if it has not been tried yet. True when it is usable. Tried
// ONCE: a machine with no device must not pay for a failing open every time a
// footstep plays. Also called by rmp::assets::load_sound, which used to need
// the game to have called InitAudioDevice() first.
bool ensure_device();

// How many times ensure_device() actually tried to open it. For the test that
// proves it is once.
int open_attempts();

// Whether the framework opened the device, and so is the one to close it.
bool opened_by_us();

// Seconds into the current track; 0 with none. For the test that proves
// asking for the same track does not restart it.
float music_time();

// Every frame, from rmp::app: feed the music stream. Nothing when there is no
// device or no music.
void update();

// On the way out, in two halves. shutdown() BEFORE rmp::detail::release_all(),
// in begin_stop(): the voices, the music and the cached effects go first,
// because a sound alias shares its buffer with the sound it was made from and
// must not outlive it. close_device() in end_stop(), AFTER the game's stop
// hook: every sound loaded through rmp::assets is unloaded by then, and so is
// any raw Sound the hook unloads itself -- a sound unloaded after its device
// has closed is a free on a torn-down mixer. And only if the framework opened
// the device: a game that opened it closes it.
void shutdown();
void close_device();

// Back to "nothing opened, nothing loaded, the volumes from the .toml". For
// tests only; it does not touch raylib.
void reset_for_tests();

} // namespace rmp::audio::detail
