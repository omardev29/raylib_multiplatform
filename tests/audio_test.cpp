// ---------------------------------------------------------------------------
// rmp::audio, without a sound device.
//
// There is none on a CI runner, and faking one would test the fake: whether a
// WAV actually comes out of the speakers is checked by ear. What is tested
// here is everything that does NOT need a device, which is also everything
// that decides what gets played and how loud:
//
//   - a logical name expanding into the files it is looked for as;
//   - the volume arithmetic and its clamping;
//   - and the property that matters most to a game with no sound at all, or
//     to one running on a machine without a device: that the device is opened
//     lazily, ONCE, and never retried every frame after it fails.
//
// The device is never opened for real in this file. The opener is swapped for
// one that says "no device", which is exactly a CI runner, or for one that
// says "yes" without touching raylib.
// ---------------------------------------------------------------------------

#include <doctest.h>

#include "../src/rmp/audio_internal.h"
#include "../src/rmp/internal.h"

#include <rmp/assets.h>
#include <rmp/audio.h>

#include <raylib.h>

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

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

struct Fixture {
    Fixture() {
        rmp::audio::detail::reset_for_tests();
        device_opens.count = 0;
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;
    ~Fixture() { rmp::audio::detail::reset_for_tests(); }
};

using Names = std::vector<std::string>;

// What raylib's log said while a test listened, one formatted line each.
struct {
    std::vector<std::string> lines;
} heard;

void hear(int /*level*/, const char *text, va_list args) {
    std::array<char, 512> line{};
    std::vsnprintf(line.data(), line.size(), text, args);
    heard.lines.emplace_back(line.data());
}

} // namespace

TEST_SUITE("audio") {
    // -----------------------------------------------------------------------
    // Logical names
    // -----------------------------------------------------------------------

    TEST_CASE("a bare name is looked for with each extension, in order") {
        const Names got = rmp::audio::detail::candidates("coin");
        // No .flac: raylib is built without its decoder, and a file that is
        // found and then cannot be decoded is worse than one not found.
        const Names want{ "coin.wav", "coin.ogg", "coin.mp3", "coin.qoa" };
        CHECK(got == want);
    }

    TEST_CASE("a name that has an audio extension is looked for exactly as given") {
        CHECK(rmp::audio::detail::candidates("coin.ogg") == Names{ "coin.ogg" });
        CHECK(rmp::audio::detail::candidates("music/level1.mp3") ==
              Names{ "music/level1.mp3" });
        // Any case, and the formats music() streams, and FLAC -- taken as
        // asked so that it can say it does not decode.
        for (const char *name : { "COIN.OGG", "Hit.Wav", "theme.xm", "theme.MOD",
                                  "theme.flac", "voice.qoa" }) {
            CAPTURE(name);
            CHECK(rmp::audio::detail::candidates(name) == Names{ name });
        }
    }

    TEST_CASE("a dot that is not an audio extension is part of the name") {
        // "ui.click" is a sound called ui.click, found as ui.click.wav. Taken
        // as an extension, it was looked for as a file called "ui.click" and
        // nothing else, while the report claimed .wav, .ogg... were tried.
        const Names got = rmp::audio::detail::candidates("ui.click");
        const Names want{ "ui.click.wav", "ui.click.ogg", "ui.click.mp3",
                          "ui.click.qoa" };
        CHECK(got == want);
        CHECK(rmp::audio::detail::candidates("jump.v2").size() == 4);
        CHECK(rmp::audio::detail::candidates("readme.txt").size() == 4);
    }

    TEST_CASE("the extension is looked for in the FILE part, not the path") {
        // A dot anywhere would read this as a file called "2/coin".
        const Names got = rmp::audio::detail::candidates("levels/v1.2/coin");
        REQUIRE(got.size() == 4);
        CHECK(got.front() == "levels/v1.2/coin.wav");
        const Names windows = rmp::audio::detail::candidates("levels\\v1.2\\coin");
        REQUIRE(windows.size() == 4);
        CHECK(windows.front() == "levels\\v1.2\\coin.wav");
    }

    TEST_CASE("a leading or trailing dot is not an extension") {
        // ".wav" is a hidden file's NAME; "coin." is a typo. Neither names a
        // format, so both get the search rather than being taken literally.
        CHECK(rmp::audio::detail::candidates(".wav").size() == 4);
        CHECK(rmp::audio::detail::candidates("coin.").size() == 4);
    }

    TEST_CASE("an empty name expands to nothing") {
        CHECK(rmp::audio::detail::candidates("").empty());
    }

    // -----------------------------------------------------------------------
    // Volumes
    // -----------------------------------------------------------------------

    TEST_CASE("a volume is clamped to 0..1, and a NaN keeps what was there") {
        using rmp::audio::detail::clamp_volume;
        CHECK(clamp_volume(0.5f, 1.0f) == doctest::Approx(0.5));
        CHECK(clamp_volume(-3.0f, 1.0f) == doctest::Approx(0.0));
        CHECK(clamp_volume(7.0f, 0.2f) == doctest::Approx(1.0));
        CHECK(clamp_volume(std::nanf(""), 0.3f) == doctest::Approx(0.3));
        CHECK(clamp_volume(0.0f, 1.0f) == doctest::Approx(0.0));
        CHECK(clamp_volume(1.0f, 0.0f) == doctest::Approx(1.0));
    }

    TEST_CASE("pan is raylib 6's: -1 left, 0 centre, 1 right, clamped, NaN centred") {
        using rmp::audio::detail::raylib_pan;
        CHECK(raylib_pan(0.0f) == 0.0f);
        CHECK(raylib_pan(-1.0f) == -1.0f);
        CHECK(raylib_pan(1.0f) == 1.0f);
        CHECK(raylib_pan(-0.25f) == doctest::Approx(-0.25));
        CHECK(raylib_pan(-7.0f) == -1.0f);
        CHECK(raylib_pan(7.0f) == 1.0f);
        CHECK(raylib_pan(std::nanf("")) == 0.0f);
        // The default is the centre -- raylib 5's 0.5 would be right of it.
        CHECK(rmp::audio::PlayOptions{}.pan == 0.0f);
    }

    TEST_CASE("looking candidates up is not a failed load") {
        // rmp::audio asks about coin.wav, coin.ogg, coin.mp3 and coin.qoa in
        // turn. Counted as failed loads, a game whose coin is an .ogg booted
        // with assets_failed=3 -- red in the CI boot gate for a correct game.
        const int failed = rmp::assets::failed_loads();
        const int requested = rmp::assets::requested_loads();
        for (const char *name : { "nope.wav", "nope.ogg", "nope.mp3", "nope.qoa" }) {
            CHECK_FALSE(rmp::assets::detail::resource_exists(name));
        }
        CHECK(rmp::assets::failed_loads() == failed);
        CHECK(rmp::assets::requested_loads() == requested);
    }

    TEST_CASE("an effect plays at its own volume times the SFX bus") {
        using rmp::audio::detail::effect_volume;
        CHECK(effect_volume(0.5f, 0.5f) == doctest::Approx(0.25));
        CHECK(effect_volume(1.0f, 0.8f) == doctest::Approx(0.8));
        CHECK(effect_volume(2.0f, 0.5f) == doctest::Approx(0.5)); // each clamped first
        CHECK(effect_volume(0.7f, -1.0f) == doctest::Approx(0.0));
        CHECK(effect_volume(std::nanf(""), 0.6f) == doctest::Approx(0.6));
    }

    TEST_CASE_FIXTURE(Fixture, "the buses start at the values in the .toml") {
        CHECK(rmp::audio::volume(rmp::audio::Bus::MASTER) ==
              doctest::Approx(RMP_AUDIO_MASTER));
        CHECK(rmp::audio::volume(rmp::audio::Bus::MUSIC) ==
              doctest::Approx(RMP_AUDIO_MUSIC));
        CHECK(rmp::audio::volume(rmp::audio::Bus::SFX) == doctest::Approx(RMP_AUDIO_SFX));
    }

    TEST_CASE_FIXTURE(
        Fixture, "set_volume clamps, ignores a NaN, and leaves the other buses alone") {
        rmp::audio::set_volume(rmp::audio::Bus::MUSIC, 0.25f);
        CHECK(rmp::audio::volume(rmp::audio::Bus::MUSIC) == doctest::Approx(0.25));
        CHECK(rmp::audio::volume(rmp::audio::Bus::SFX) == doctest::Approx(RMP_AUDIO_SFX));
        CHECK(rmp::audio::volume(rmp::audio::Bus::MASTER) ==
              doctest::Approx(RMP_AUDIO_MASTER));

        rmp::audio::set_volume(rmp::audio::Bus::MUSIC, 9.0f);
        CHECK(rmp::audio::volume(rmp::audio::Bus::MUSIC) == doctest::Approx(1.0));
        rmp::audio::set_volume(rmp::audio::Bus::MUSIC, -9.0f);
        CHECK(rmp::audio::volume(rmp::audio::Bus::MUSIC) == doctest::Approx(0.0));
        rmp::audio::set_volume(rmp::audio::Bus::MUSIC, 0.4f);
        rmp::audio::set_volume(rmp::audio::Bus::MUSIC, std::nanf(""));
        CHECK(rmp::audio::volume(rmp::audio::Bus::MUSIC) == doctest::Approx(0.4));
    }

    // -----------------------------------------------------------------------
    // The device: lazy, and once
    // -----------------------------------------------------------------------

    TEST_CASE_FIXTURE(Fixture, "a settings screen never opens the device") {
        // Volumes, queries, available(): a game that has made no sound must
        // not open a device to show and move its sliders.
        rmp::audio::detail::set_device_opener(no_device);
        rmp::audio::set_volume(rmp::audio::Bus::MASTER, 0.5f);
        rmp::audio::set_volume(rmp::audio::Bus::SFX, 0.5f);
        (void)rmp::audio::volume(rmp::audio::Bus::MUSIC);
        CHECK_FALSE(rmp::audio::available());
        CHECK_FALSE(rmp::audio::music_playing());
        rmp::audio::stop_music();
        CHECK(device_opens.count == 0);
        CHECK(rmp::audio::detail::open_attempts() == 0);
    }

    TEST_CASE_FIXTURE(Fixture, "an empty name never opens the device either") {
        rmp::audio::detail::set_device_opener(no_device);
        rmp::audio::play("");
        rmp::audio::music("");
        CHECK(device_opens.count == 0);
    }

    TEST_CASE_FIXTURE(Fixture,
                      "with no device, it is tried ONCE and every call is a no-op") {
        // A CI runner, a server, a laptop with its sound off. A footstep every
        // frame must not pay for a failing InitAudioDevice() every frame.
        rmp::audio::detail::set_device_opener(no_device);
        for (int i = 0; i < 100; ++i) {
            rmp::audio::play("step");
            rmp::audio::play("coin", { .volume = 0.5f, .pitch = 2.0f });
            rmp::audio::music("level1");
        }
        CHECK(device_opens.count == 1);
        CHECK(rmp::audio::detail::open_attempts() == 1);
        CHECK_FALSE(rmp::audio::available());
        CHECK_FALSE(rmp::audio::music_playing());
        CHECK_FALSE(rmp::audio::detail::ensure_device());
        CHECK(device_opens.count == 1);
    }

    TEST_CASE_FIXTURE(Fixture,
                      "no device is not a mistake: [dev] strict does not stop the game") {
        // Strict aborts a debug build on the first diagnostic, because those
        // are mistakes in the game. A runner with no sound card is not one,
        // and a strict build must still boot there.
        static int stops = 0;
        stops = 0;
        // Forget what earlier tests reported: a report that already fired is
        // silent, and this test would pass against the very bug it is for.
        rmp::detail::reset_reports_for_tests();
        const bool was_strict = rmp::detail::strict();
        const rmp::detail::StrictHandler previous =
            rmp::detail::set_strict_handler([] { ++stops; });
        rmp::detail::set_strict(true);
        rmp::audio::detail::set_device_opener(no_device);
        rmp::audio::play("step");
        rmp::audio::music("level1");
        rmp::detail::set_strict(was_strict);
        rmp::detail::set_strict_handler(previous);
        CHECK(device_opens.count == 1);
        CHECK(stops == 0);
        CHECK(rmp::detail::report_count() == 0);
        rmp::detail::reset_reports_for_tests();
    }

    TEST_CASE_FIXTURE(Fixture, "a device that opens is opened once and stays open") {
        rmp::audio::detail::set_device_opener(fake_device);
        CHECK(rmp::audio::detail::ensure_device());
        CHECK(rmp::audio::detail::ensure_device());
        CHECK(rmp::audio::detail::ensure_device());
        CHECK(device_opens.count == 1);
        CHECK(rmp::audio::available());
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a sound that is not there plays nothing and does not crash") {
        // With a device (faked) the play goes as far as the name, finds no
        // file, and stops -- before anything touches the mixer.
        rmp::detail::reset_reports_for_tests();
        rmp::audio::detail::set_device_opener(fake_device);
        rmp::audio::play("definitely_not_a_sound_anywhere");
        rmp::audio::play("definitely_not_a_sound_anywhere");
        // THAT is a mistake in the game, and it is said -- once per name.
        CHECK(rmp::detail::report_count() == 1);
        rmp::audio::play("another_missing_sound");
        CHECK(rmp::detail::report_count() == 2);
        rmp::audio::music("definitely_not_music_anywhere");
        CHECK(rmp::detail::report_count() == 3);
        CHECK_FALSE(rmp::audio::music_playing());
        CHECK(device_opens.count == 1);
        rmp::detail::reset_reports_for_tests();
    }

    TEST_CASE_FIXTURE(Fixture,
                      "a sound that is not there is reported with the files looked for") {
        // The line said "tried the name as given, then .wav, .ogg, .mp3, .qoa"
        // for every name, and a bare name is never tried as given: "coin" is
        // looked for as coin.wav and the rest, and "coin.ogg" as coin.ogg
        // alone. It names the files now, so it cannot say anything else.
        rmp::detail::reset_reports_for_tests();
        rmp::audio::detail::set_device_opener(fake_device);
        heard.lines.clear();
        SetTraceLogCallback(hear);
        rmp::audio::play("definitely_not_a_sound_anywhere");
        rmp::audio::play("definitely_not_a_sound_anywhere.ogg");
        SetTraceLogCallback(nullptr);
        REQUIRE(heard.lines.size() == 2);
        CHECK(heard.lines[0].find("looked for definitely_not_a_sound_anywhere.wav, "
                                  "definitely_not_a_sound_anywhere.ogg, "
                                  "definitely_not_a_sound_anywhere.mp3, "
                                  "definitely_not_a_sound_anywhere.qoa") !=
              std::string::npos);
        CHECK(heard.lines[1].find("looked for definitely_not_a_sound_anywhere.ogg)") !=
              std::string::npos);
        for (const std::string &line : heard.lines) {
            CAPTURE(line);
            CHECK(line.find("as given") == std::string::npos);
        }
        rmp::detail::reset_reports_for_tests();
    }

    TEST_CASE_FIXTURE(Fixture, "closing the device lets a later game open it again") {
        rmp::audio::detail::set_device_opener(no_device);
        CHECK_FALSE(rmp::audio::detail::ensure_device());
        rmp::audio::detail::shutdown();
        rmp::audio::detail::close_device();
        CHECK_FALSE(rmp::audio::detail::ensure_device());
        CHECK(device_opens.count == 2); // one per open, not one per call
    }
}
