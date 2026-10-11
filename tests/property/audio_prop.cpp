// ---------------------------------------------------------------------------
// rmp::audio, held to what rmp/audio.h says of it for any name and any number
// a game can hand it:
//
//   - a logical name expands to the files rmp/audio.h describes, worked out
//     here a second way: nothing for an empty name, the name alone when its
//     FILE part ends in an audio extension (any case), and the four
//     extensions in order otherwise;
//   - a volume is stored clamped to 0..1 and a NaN keeps the one before; an
//     effect's volume and its pan are in range whatever they are given;
//   - the buses never open the device, and a machine with no device is tried
//     ONCE, says nothing through RMP_REPORT_ONCE (it is not a mistake in the
//     game) and leaves every call a no-op;
//   - with a device, every name that resolves to nothing is said once -- one
//     line per name, every byte of it -- and nothing else is said;
//   - and with the real device, silent, any PlayOptions at all plays without
//     undefined behaviour, in raylib too, and a name with a NUL inside plays
//     nothing: the C string a file is opened by would stop at the NUL and
//     open another file.
//
// covers: rmp::audio::play(std::string_view, const PlayOptions &)
// covers: rmp::audio::music(std::string_view, bool)
// covers: rmp::audio::stop_music()
// covers: rmp::audio::music_playing()
// covers: rmp::audio::set_volume(Bus, float)
// covers: rmp::audio::volume(Bus)
// covers: rmp::audio::available()
// ---------------------------------------------------------------------------

#include "check.h"
#include "gen.h"

#include "../../src/rmp/audio_internal.h"
#include "../../src/rmp/internal.h"

#include <rmp/assets.h>
#include <rmp/audio.h>
#include <rmp/config.h>

#include <doctest.h>
#include <rapidcheck.h>
#include <raylib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace rmp::audio {

// How RapidCheck prints PlayOptions in a counterexample. RapidCheck finds it
// by its name, in the type's namespace.
// NOLINTNEXTLINE(readability-identifier-naming): RapidCheck's name
inline void showValue(const PlayOptions &options, std::ostream &os) {
    os << "{ .volume = " << options.volume << ", .pitch = " << options.pitch
       << ", .pan = " << options.pan << " }";
}

} // namespace rmp::audio

namespace {

namespace fs = std::filesystem;
using rmp::audio::Bus;
using rmp::audio::PlayOptions;

// --- the model ----------------------------------------------------------------

// The extensions a name may already carry, as rmp/audio.h lists them.
bool known_extension(std::string ext) {
    for (char &c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::array<std::string_view, 7> KNOWN = { ".wav", ".ogg",  ".mp3",
                                                           ".qoa", ".flac", ".xm",
                                                           ".mod" };
    return std::ranges::find(KNOWN, ext) != KNOWN.end();
}

std::vector<std::string> model_candidates(const std::string &name) {
    if (name.empty()) return {};
    std::size_t start = 0;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '/' || name[i] == '\\') start = i + 1;
    }
    const std::string file = name.substr(start);
    std::size_t dot = std::string::npos;
    for (std::size_t i = 0; i < file.size(); ++i) {
        if (file[i] == '.') dot = i;
    }
    if (dot != std::string::npos && dot > 0 && known_extension(file.substr(dot))) {
        return { name };
    }
    return { name + ".wav", name + ".ogg", name + ".mp3", name + ".qoa" };
}

float model_clamp(float value, float previous) {
    if (std::isnan(value)) return previous;
    return std::clamp(value, 0.0f, 1.0f);
}

// What volume(bus) reads for a bus cast by hand: MASTER, as rmp::audio does.
int bus_index(Bus bus) {
    if (bus == Bus::MUSIC) return 1;
    if (bus == Bus::SFX) return 2;
    return 0;
}

// --- generators ---------------------------------------------------------------

// A name: anything at all, or one of a few that collide in the ways that
// matter -- the same bytes up to a NUL, an extension in another case, an
// extension in the path rather than the file.
rc::Gen<std::string> sound_name() {
    return rc::gen::weightedOneOf<std::string>({
        { 2, rmp::test::gen::hostile_string() },
        { 3,
          rc::gen::element<std::string>(
              std::string("a"), std::string("a\0b", 3), std::string("a\0c", 3),
              std::string("coin"), std::string("coin.ogg"), std::string("COIN.OGG"),
              std::string(".wav"), std::string("x.v1/coin"), std::string("ui.click"),
              std::string("\\"), std::string("/"), std::string("")) },
    });
}

rc::Gen<Bus> any_bus() {
    // The three, and numbers cast into the enum by hand.
    return rc::gen::map(rc::gen::inRange(0, 6),
                        [](int n) { return static_cast<Bus>(n); });
}

rc::Gen<PlayOptions> any_options() {
    return rc::gen::build<PlayOptions>(
        rc::gen::set(&PlayOptions::volume, rmp::test::gen::hostile_float()),
        rc::gen::set(&PlayOptions::pitch, rmp::test::gen::hostile_float()),
        rc::gen::set(&PlayOptions::pan, rmp::test::gen::hostile_float()));
}

// --- the device ------------------------------------------------------------------

struct {
    int count = 0;
} device_opens;
bool no_device() {
    ++device_opens.count;
    return false;
}
bool fake_device() {
    ++device_opens.count;
    return true;
}

// Back to "nothing opened, nothing loaded", with the opener a test chose.
void fresh(rmp::audio::detail::DeviceOpener opener) {
    rmp::audio::detail::reset_for_tests();
    rmp::audio::detail::set_device_opener(opener);
    device_opens.count = 0;
    rmp::detail::reset_reports_for_tests();
}

// A resources/ of its own for one test case: empty, or with the tone the
// real device plays. The name of the tone is "tone", "tone.wav" on disk.
struct Resources {
    fs::path folder;
    std::string previous;
    explicit Resources(bool with_tone) : previous(rmp::assets::detail::resources_root()) {
        std::error_code error;
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        folder =
            fs::temp_directory_path(error) / ("rmp-audio-prop-" + std::to_string(now));
        fs::create_directories(folder, error);
        if (with_tone) write_tone(folder / "tone.wav");
        const std::string root = (folder / "").string();
        rmp::assets::detail::set_resources_root(root.c_str());
    }
    ~Resources() {
        rmp::assets::detail::set_resources_root(previous.c_str());
        std::error_code error;
        fs::remove_all(folder, error);
    }
    Resources(const Resources &) = delete;
    Resources &operator=(const Resources &) = delete;

    // A tenth of a second of a 440 Hz sine, mono, 16 bits, at 44.1 kHz.
    static void write_tone(const fs::path &path) {
        constexpr std::uint32_t RATE = 44100;
        constexpr std::uint32_t FRAMES = RATE / 10;
        std::ofstream f(path, std::ios::binary);
        const auto u32 = [&f](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) f.put(static_cast<char>((v >> (8U * i)) & 0xFFU));
        };
        const auto u16 = [&f](std::uint32_t v) {
            f.put(static_cast<char>(v & 0xFFU));
            f.put(static_cast<char>((v >> 8U) & 0xFFU));
        };
        f.write("RIFF", 4);
        u32(36 + FRAMES * 2);
        f.write("WAVEfmt ", 8);
        u32(16);
        u16(1); // PCM
        u16(1); // mono
        u32(RATE);
        u32(RATE * 2);
        u16(2);
        u16(16);
        f.write("data", 4);
        u32(FRAMES * 2);
        for (std::uint32_t i = 0; i < FRAMES; ++i) {
            const double s = 0.5 * std::sin(2 * std::numbers::pi * 440.0 * i / RATE);
            u16(static_cast<std::uint32_t>(static_cast<std::int16_t>(s * 32767)) &
                0xFFFFU);
        }
    }
};

// What raylib's log says while a case runs: nothing, the lines are not what
// is checked and a thousand of them would bury the summary.
struct QuietLog {
    QuietLog() { SetTraceLogLevel(LOG_NONE); }
    ~QuietLog() { SetTraceLogLevel(LOG_INFO); }
    QuietLog(const QuietLog &) = delete;
    QuietLog &operator=(const QuietLog &) = delete;
};

} // namespace

TEST_SUITE("property: audio") {
    TEST_CASE("a logical name expands to the files rmp/audio.h describes") {
        rmp::test::check(
            "candidates() is the name alone or the four extensions, in order", [] {
                const std::string name = *sound_name();
                RC_ASSERT(rmp::audio::detail::candidates(name) == model_candidates(name));
            });
    }

    TEST_CASE("a volume, a pitch and a pan are in range whatever they are given") {
        rmp::test::check(
            "clamp_volume, effect_volume, raylib_pitch and raylib_pan stay in range", [] {
                const float value = *rmp::test::gen::hostile_float();
                const float previous = *rc::gen::inRange(0, 101) / 100.0f;
                const float stored = rmp::audio::detail::clamp_volume(value, previous);
                RC_ASSERT(stored == model_clamp(value, previous));

                const float bus = *rmp::test::gen::hostile_float();
                const float effect = rmp::audio::detail::effect_volume(value, bus);
                RC_ASSERT(effect >= 0.0f);
                RC_ASSERT(effect <= 1.0f);
                RC_ASSERT(effect == model_clamp(value, 1.0f) * model_clamp(bus, 1.0f));

                const float pitch = rmp::audio::detail::raylib_pitch(value);
                RC_ASSERT(pitch >= rmp::audio::detail::MIN_PITCH);
                RC_ASSERT(pitch <= rmp::audio::detail::MAX_PITCH);
                if (!(value > 0) || !std::isfinite(value)) RC_ASSERT(pitch == 1.0f);
                if (value >= rmp::audio::detail::MIN_PITCH &&
                    value <= rmp::audio::detail::MAX_PITCH) {
                    RC_ASSERT(pitch == value);
                }

                const float pan = rmp::audio::detail::raylib_pan(value);
                RC_ASSERT(pan >= -1.0f);
                RC_ASSERT(pan <= 1.0f);
                RC_ASSERT(pan ==
                          (std::isnan(value) ? 0.0f : std::clamp(value, -1.0f, 1.0f)));
            });
    }

    TEST_CASE("the buses keep what they are given, clamped, and never open the device") {
        rmp::test::check("set_volume stores each bus clamped, a NaN changes nothing", [] {
            fresh(no_device);
            std::array<float, 3> model = { RMP_AUDIO_MASTER, RMP_AUDIO_MUSIC,
                                           RMP_AUDIO_SFX };
            const int steps = *rc::gen::inRange(1, 16);
            for (int i = 0; i < steps; ++i) {
                const Bus bus = *any_bus();
                const float value = *rmp::test::gen::hostile_float();
                rmp::audio::set_volume(bus, value);
                float &slot = model[static_cast<std::size_t>(bus_index(bus))];
                slot = model_clamp(value, slot);
                RC_ASSERT(rmp::audio::volume(Bus::MASTER) == model[0]);
                RC_ASSERT(rmp::audio::volume(Bus::MUSIC) == model[1]);
                RC_ASSERT(rmp::audio::volume(Bus::SFX) == model[2]);
                RC_ASSERT(rmp::audio::volume(bus) == slot);
            }
            RC_ASSERT(device_opens.count == 0);
            RC_ASSERT(rmp::audio::detail::open_attempts() == 0);
            RC_ASSERT(!rmp::audio::available());
            RC_ASSERT(rmp::detail::report_count() == 0);
            rmp::audio::detail::reset_for_tests();
        });
    }

    TEST_CASE("a machine with no device is tried once, and says nothing") {
        const QuietLog quiet;
        rmp::test::check(
            "with no device every call is a no-op, the device is tried once, nothing "
            "reported",
            [] {
                fresh(no_device);
                bool asked = false; // anything that needs the device
                const int steps = *rc::gen::inRange(1, 24);
                for (int i = 0; i < steps; ++i) {
                    const std::string name = *sound_name();
                    switch (*rc::gen::inRange(0, 5)) {
                        case 0:
                            rmp::audio::play(name, *any_options());
                            asked = asked || !name.empty();
                            break;
                        case 1:
                            rmp::audio::music(name, *rc::gen::arbitrary<bool>());
                            asked = asked || !name.empty();
                            break;
                        case 2:
                            rmp::audio::stop_music();
                            break;
                        case 3:
                            rmp::audio::set_volume(*any_bus(),
                                                   *rmp::test::gen::hostile_float());
                            break;
                        default:
                            RC_ASSERT(!rmp::audio::music_playing());
                            break;
                    }
                    RC_ASSERT(!rmp::audio::available());
                    RC_ASSERT(!rmp::audio::music_playing());
                    RC_ASSERT(device_opens.count == (asked ? 1 : 0));
                    RC_ASSERT(rmp::audio::detail::open_attempts() == device_opens.count);
                }
                // A machine without sound is not a mistake in the game.
                RC_ASSERT(rmp::detail::report_count() == 0);
                rmp::audio::detail::reset_for_tests();
            });
    }

    TEST_CASE("every name that resolves to nothing is said once, every byte of it") {
        const QuietLog quiet;
        const Resources empty(false);
        rmp::test::check(
            "a missing sound and a missing song are said once per name, and nothing else "
            "is",
            [] {
                fresh(fake_device);
                std::set<std::string> sounds;
                std::set<std::string> songs;
                const int steps = *rc::gen::inRange(1, 24);
                for (int i = 0; i < steps; ++i) {
                    const std::string name = *sound_name();
                    switch (*rc::gen::inRange(0, 3)) {
                        case 0:
                            rmp::audio::play(name, *any_options());
                            if (!name.empty()) sounds.insert(name);
                            break;
                        case 1:
                            rmp::audio::music(name, *rc::gen::arbitrary<bool>());
                            if (!name.empty()) songs.insert(name);
                            break;
                        default:
                            rmp::audio::stop_music();
                            break;
                    }
                    RC_ASSERT(!rmp::audio::music_playing());
                    RC_ASSERT(rmp::detail::report_count() ==
                              static_cast<int>(sounds.size() + songs.size()));
                }
                RC_ASSERT(device_opens.count <= 1);
                rmp::audio::detail::reset_for_tests();
                rmp::detail::reset_reports_for_tests();
            });
    }

    TEST_CASE("with the real device, silent, any options play and a NUL plays nothing") {
        const QuietLog quiet;
        const Resources tone(true);
        rmp::audio::detail::reset_for_tests(); // the real opener
        if (!rmp::audio::detail::ensure_device()) {
            // tests/audio_device_test.cpp's suite is what requires a device
            // where one must be (RMP_REQUIRE_AUDIO_DEVICE); this one goes
            // where the device goes.
            MESSAGE("no sound device here, not even a null one: skipped");
            rmp::audio::detail::reset_for_tests();
            return;
        }
        SetMasterVolume(0.0f); // nothing reaches the speakers
        rmp::test::check(
            "play, music and the buses take any name and any number on a real device",
            [] {
                rmp::detail::reset_reports_for_tests();
                std::set<std::string> missing_sounds;
                std::set<std::string> missing_songs;
                const auto plays = [](const std::string &name) {
                    return name == "tone" || name == "tone.wav";
                };
                const int steps = *rc::gen::inRange(1, 12);
                for (int i = 0; i < steps; ++i) {
                    const std::string name = *rc::gen::weightedOneOf<std::string>({
                        { 4,
                          rc::gen::element<std::string>(
                              std::string("tone"), std::string("tone.wav"),
                              std::string("tone.wav\0x", 10), std::string("tone\0", 5),
                              std::string("tone.wav\0.wav", 13),
                              std::string("missing")) },
                        { 1, sound_name() },
                    });
                    switch (*rc::gen::inRange(0, 4)) {
                        case 0:
                            rmp::audio::play(name, *any_options());
                            if (!name.empty() && !plays(name))
                                missing_sounds.insert(name);
                            break;
                        case 1:
                            rmp::audio::music(name, *rc::gen::arbitrary<bool>());
                            if (!name.empty() && !plays(name)) missing_songs.insert(name);
                            if (plays(name)) RC_ASSERT(rmp::audio::music_playing());
                            break;
                        case 2:
                            rmp::audio::stop_music();
                            RC_ASSERT(!rmp::audio::music_playing());
                            break;
                        default:
                            rmp::audio::set_volume(*any_bus(),
                                                   *rmp::test::gen::hostile_float());
                            SetMasterVolume(0.0f);
                            break;
                    }
                    rmp::audio::detail::update();
                    RC_ASSERT(rmp::audio::available());
                    for (const Bus bus : { Bus::MASTER, Bus::MUSIC, Bus::SFX }) {
                        const float v = rmp::audio::volume(bus);
                        RC_ASSERT(v >= 0.0f);
                        RC_ASSERT(v <= 1.0f);
                    }
                }
                RC_ASSERT(rmp::detail::report_count() ==
                          static_cast<int>(missing_sounds.size() + missing_songs.size()));
                // The effects and the track go; the device stays for the next case.
                rmp::audio::detail::shutdown();
                rmp::detail::release_all();
            });
        rmp::audio::detail::shutdown();
        rmp::detail::release_all();
        rmp::audio::detail::close_device();
        rmp::audio::detail::reset_for_tests();
        rmp::detail::reset_reports_for_tests();
    }
}
