// ---------------------------------------------------------------------------
// rmp::audio against a REAL device, read back from the mixer.
//
// tests/audio_test.cpp covers everything that needs no device. What it cannot
// see is what the mixer does with our calls -- and that is where the first
// review of rmp::audio found its worst bugs: the pan was raylib 5's range,
// so every default play leaned three quarters to the right; play() changed
// the volume of the game's own rmp::Sound; a jingle played once could never
// be played again. None of those is visible without listening.
//
// So this listens. The master volume goes to zero -- nothing reaches the
// speakers -- and AttachAudioMixedProcessor() hands the test every mixed
// frame before miniaudio applies that master volume. Left and right energy
// are summed over a window and compared.
//
// Where there is no device at all the tests say so and return. miniaudio
// keeps its null backend, so on most CI runners there IS a device, silent,
// and these run there too.
// ---------------------------------------------------------------------------

#include <doctest.h>

#include "../src/rmp/audio_internal.h"
#include "../src/rmp/internal.h"

#include <rmp/assets.h>
#include <rmp/audio.h>

#include <raylib.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <numbers>
#include <random>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace {

struct Energy {
    double left = 0;
    double right = 0;
    [[nodiscard]] double total() const { return left + right; }
};

// What the mixer has played, written on the audio thread under the lock.
struct {
    std::mutex lock;
    Energy energy;
} mixed;

// On the audio thread: raylib mixes in 32-bit float, two channels.
void capture(void *buffer, unsigned int frames) {
    const auto *samples = static_cast<const float *>(buffer);
    double l = 0;
    double r = 0;
    for (unsigned int i = 0; i < frames; i++) {
        const float left = samples[2 * static_cast<std::size_t>(i)];
        const float right = samples[2 * static_cast<std::size_t>(i) + 1];
        l += static_cast<double>(left) * left;
        r += static_cast<double>(right) * right;
    }
    const std::scoped_lock hold(mixed.lock);
    mixed.energy.left += l;
    mixed.energy.right += r;
}

Energy take() {
    const std::scoped_lock hold(mixed.lock);
    const Energy e = mixed.energy;
    mixed.energy = Energy{};
    return e;
}

void wait(double seconds) {
    // The music stream has to be fed while time passes, as the app does.
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        rmp::audio::detail::update();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// A mono 16-bit WAV of a sine, written by hand so the test owns its input.
void write_tone(const fs::path &path, double seconds, double hz) {
    const int rate = 44100;
    const auto frames = static_cast<std::uint32_t>(seconds * rate);
    std::ofstream f(path, std::ios::binary);
    REQUIRE(f.good());
    const auto u32 = [&f](std::uint32_t v) {
        f.write(reinterpret_cast<const char *>(&v), 4);
    };
    const auto u16 = [&f](std::uint16_t v) {
        f.write(reinterpret_cast<const char *>(&v), 2);
    };
    f.write("RIFF", 4);
    u32(36 + frames * 2);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1); // PCM
    u16(1); // mono
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    f.write("data", 4);
    u32(frames * 2);
    for (std::uint32_t i = 0; i < frames; i++) {
        const double s = 0.5 * std::sin(2 * std::numbers::pi * hz * i / rate);
        u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(s * 32767)));
    }
}

// Where there is no device at all these tests skip -- but the CI lint job
// sets RMP_REQUIRE_AUDIO_DEVICE=1, and there a skip is a failure: miniaudio's
// null backend is in the image, and a suite that quietly stops running is a
// gate that passes by not looking.
void skipped() {
    const char *must = std::getenv("RMP_REQUIRE_AUDIO_DEVICE");
    if (must != nullptr && std::string(must) == "1") {
        FAIL_CHECK(
            "no sound device, and RMP_REQUIRE_AUDIO_DEVICE=1 says there must be one");
    } else {
        MESSAGE("no sound device here, not even a null one: skipped");
    }
}

// The real device, silent, with a folder of tones as resources/. `ok` is
// false where there is no device, and every test then returns.
struct Device {
    fs::path dir;
    std::string saved_root;
    bool ok = false;

    Device() {
        std::random_device rd;
        dir = fs::temp_directory_path() / ("rmp_audio_device_" + std::to_string(rd()));
        fs::create_directories(dir);
        write_tone(dir / "short.wav", 0.08, 440);
        write_tone(dir / "jingle.wav", 0.2, 660);
        write_tone(dir / "long.wav", 1.0, 440);
        saved_root = rmp::assets::detail::resources_root();
        rmp::assets::detail::set_resources_root((dir.string() + "/").c_str());

        rmp::audio::detail::reset_for_tests(); // the real opener
        ok = rmp::audio::detail::ensure_device();
        if (!ok) {
            skipped();
            return;
        }
        SetMasterVolume(0.0f); // nothing reaches the speakers
        AttachAudioMixedProcessor(capture);
        warm_up();
        (void)take();
    }

    // A real backend can take longer than a play's 0.15 s to start its stream
    // the first time -- PulseAudio did, and the first case of the suite then
    // compared silence with silence and failed one run in a few. So the mixer
    // is woken first: one tone, waited for until it is heard (or two seconds,
    // and then the cases say what they hear).
    static void warm_up() {
        (void)take();
        rmp::audio::play("short", {});
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < until) {
            wait(0.02);
            const std::scoped_lock hold(mixed.lock);
            if (mixed.energy.total() > 0) break;
        }
        wait(0.15); // and let it finish, so it is not in the first measurement
    }
    ~Device() {
        if (ok) DetachAudioMixedProcessor(capture);
        rmp::audio::detail::shutdown();
        rmp::detail::release_all();
        rmp::audio::detail::close_device();
        rmp::audio::detail::reset_for_tests();
        rmp::assets::detail::set_resources_root(saved_root.c_str());
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;
};

// The energy of one play of `name`, over its whole length.
Energy energy_of_play(const char *name, const rmp::audio::PlayOptions &options) {
    (void)take();
    rmp::audio::play(name, options);
    wait(0.15);
    return take();
}

} // namespace

TEST_SUITE("audio: device") {
    TEST_CASE("pan: -1 is left, 1 is right, and the default is the centre") {
        const Device device;
        if (!device.ok) return;
        const Energy centre = energy_of_play("short", {});
        const Energy left = energy_of_play("short", { .pan = -1.0f });
        const Energy right = energy_of_play("short", { .pan = 1.0f });
        REQUIRE(centre.total() > 0); // or everything below compares silence

        // raylib 5's 0.5-is-centre, fed to raylib 6, gave right/left = 6.2.
        CHECK(centre.right / centre.left == doctest::Approx(1.0).epsilon(0.1));
        CHECK(left.left > 20 * left.right);
        CHECK(right.right > 20 * right.left);
    }

    TEST_CASE("play() leaves the game's own rmp::Sound of the same file alone") {
        const Device device;
        if (!device.ok) return;
        const rmp::Sound mine = rmp::assets::load_sound("short.wav");
        REQUIRE(mine.valid());
        const ::Sound &raw = mine;

        (void)take();
        PlaySound(raw);
        wait(0.15);
        const Energy before = take();
        REQUIRE(before.total() > 0);

        // A quiet, high, hard-left play through rmp::audio...
        (void)energy_of_play("short", { .volume = 0.05f, .pitch = 2.0f, .pan = -1.0f });

        // ...and the game's sound plays exactly as it did.
        (void)take();
        PlaySound(raw);
        wait(0.15);
        const Energy after = take();
        CHECK(after.total() == doctest::Approx(before.total()).epsilon(0.1));
        CHECK(after.right / after.left ==
              doctest::Approx(before.right / before.left).epsilon(0.1));
    }

    TEST_CASE("the SFX bus scales effects that are already ringing") {
        const Device device;
        if (!device.ok) return;
        rmp::audio::play("long", { .volume = 1.0f });
        wait(0.05);
        (void)take();
        wait(0.1);
        const Energy loud = take();
        rmp::audio::set_volume(rmp::audio::Bus::SFX, 0.25f);
        wait(0.03); // let the change reach the mixer
        (void)take();
        wait(0.1);
        const Energy quiet = take();
        REQUIRE(loud.total() > 0);
        // A quarter of the amplitude is a sixteenth of the energy.
        CHECK(quiet.total() / loud.total() < 0.12);
        CHECK(quiet.total() / loud.total() > 0.02);
    }

    TEST_CASE("a track played without looping plays again when asked for again") {
        const Device device;
        if (!device.ok) return;
        rmp::audio::music("jingle", false);
        CHECK(rmp::audio::music_playing());
        wait(0.6); // well past its end, stream buffers included
        CHECK_FALSE(rmp::audio::music_playing());
        rmp::audio::music("jingle", false); // the second game over
        CHECK(rmp::audio::music_playing());
        (void)take();
        wait(0.1);
        CHECK(take().total() > 0);
    }

    TEST_CASE("the same track while it plays is not a restart") {
        const Device device;
        if (!device.ok) return;
        rmp::audio::music("long", true);
        wait(0.2);
        const float before = rmp::audio::detail::music_time();
        REQUIRE(before > 0.1f);
        rmp::audio::music("long", true); // the scene re-entered
        CHECK(rmp::audio::music_playing());
        CHECK(rmp::audio::detail::music_time() >= before); // not back at 0
    }

    TEST_CASE("a device the game opened itself is the game's to close") {
        rmp::audio::detail::reset_for_tests();
        InitAudioDevice();
        if (!IsAudioDeviceReady()) {
            skipped();
            return;
        }
        CHECK(rmp::audio::detail::ensure_device());
        CHECK_FALSE(rmp::audio::detail::opened_by_us());
        rmp::audio::detail::shutdown();
        rmp::audio::detail::close_device();
        CHECK(IsAudioDeviceReady()); // still open: not ours to close
        CloseAudioDevice();
        rmp::audio::detail::reset_for_tests();
    }

    // A sound or a song that is THERE and does not decode is a failed load,
    // the way a missing one is: the CI boot gate reads assets_failed, and a
    // corrupt .wav left a silent game and a green boot. The files are
    // tests/fixtures/corrupt/'s; see the end of tests/assets_test.cpp.
    TEST_CASE("a sound that does not decode is a failed load") {
        const Device device;
        if (!device.ok) return;
        rmp::assets::detail::set_resources_root(RMP_TEST_FIXTURES "corrupt/");
        const int requested = rmp::assets::requested_loads();
        const int failed = rmp::assets::failed_loads();
        CHECK_FALSE(rmp::assets::load_sound("broken.wav").valid());
        CHECK(rmp::assets::requested_loads() == requested + 1);
        CHECK(rmp::assets::failed_loads() == failed + 1);
    }

    TEST_CASE("music that does not decode is a failed load, and is not asked for again") {
        const Device device;
        if (!device.ok) return;
        rmp::assets::detail::set_resources_root(RMP_TEST_FIXTURES "corrupt/");
        const int failed = rmp::assets::failed_loads();
        rmp::audio::music("broken.ogg");
        CHECK_FALSE(rmp::audio::music_playing());
        CHECK(rmp::assets::failed_loads() == failed + 1);
        rmp::audio::music("broken.ogg"); // remembered as missing: no second count
        CHECK(rmp::assets::failed_loads() == failed + 1);
    }

    TEST_CASE("a device the framework opened, the framework closes") {
        rmp::audio::detail::reset_for_tests();
        if (!rmp::audio::detail::ensure_device()) {
            skipped();
            return;
        }
        CHECK(rmp::audio::detail::opened_by_us());
        rmp::audio::detail::shutdown();
        rmp::audio::detail::close_device();
        CHECK_FALSE(IsAudioDeviceReady());
        rmp::audio::detail::reset_for_tests();
    }
}
