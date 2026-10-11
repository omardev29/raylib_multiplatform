// ===========================================================================
// tests/e2e/driver.cpp -- an example, PLAYED from a script, frame by frame,
// and its frames compared with the goldens, pixel for pixel.
//
// Linked into e2e_<area>_<example> (CMakeLists.txt) next to the example's own
// sources -- its main.cpp and its RMP_GAME or RMP_ENTRY_POINT included -- so
// what runs is the game as it ships, under raylib's software renderer
// (PLATFORM=Memory). Compiled once for all of them: which script, which
// goldens and which folder are the target's own file (target.h). Nothing in the game knows: this file installs itself
// with a static initializer, before main(), through the seams a headless test
// already had and the clock's (src/rmp/internal.h):
//
//   the clock   every frame is exactly 1/60 s and rmp::random starts from
//               seed 1, so a run is the same run every time -- the game's
//               and the UI's, which read one clock;
//   the hands   rmp::input's devices AND the UI's navigation and pointer,
//               which are two seams (CLAUDE.md says why), fed from what the
//               script holds down -- the keyboard, the first gamepad's
//               buttons and the mouse; the input for frame N+1 is set at the
//               end of frame N, which is when a real keyboard changes;
//   the disk    rmp::save writes to a fresh folder under the build tree, so
//               a best score from the last run cannot change this one;
//   the sound   no device, the same on a laptop with speakers and in a CI
//               container without any;
//   the eyes    at the end of every frame, after EndDrawing() -- valid here,
//               where the framebuffer is memory (tests/smoke_test.h) -- the
//               script's shots and expectations for that frame, and on
//               every frame whether the example put a control where nobody
//               can reach it (rmp::ui::detail::controls_out_of_reach()).
//
// What it prints: `E2E SHOT <name> <w>x<h> hash=<fnv>` for every shot (two
// runs, or the image and a laptop, compare by those), `E2E FAIL <line>:
// <reason>` for every check that failed, and `E2E PASS` at `end` when none
// did; the exit status is not 0 when one did, or when the game stopped
// before `end`. A mistake of the game's that the framework reported during
// the run (RMP_REPORT_ONCE: a widget outside begin()/end(), an action nobody
// defined) fails it too, at `end`, a line each, in the log's own words: the
// run that said it is not the run the script describes, whatever its
// pictures look like -- and a game made from it says the same thing in a
// player's log.
//
// THREE MODES, from the environment, which tools/examples_build.sh sets:
//
//   RMP_E2E_COMPARE=1  a shot that differs from its golden fails. Only inside
//                      the pinned build image, where the goldens were drawn:
//                      the comparison is exact, and exact means one compiler,
//                      one libm, one renderer.
//   (neither)          the same comparison, REPORTED (`E2E DIFF`) and not
//                      enforced -- a laptop is not where goldens come from.
//   RMP_E2E_UPDATE=1   records every shot as its golden, and refuses outside
//                      the image. `rmp test e2e-update`.
//
// On a difference the expected picture, the actual one and the difference go
// to e2e::target::OUT (build/examples-check/e2e/<area>/<example>/), which CI
// uploads.
// ===========================================================================

#include "compare.h"
#include "script.h"
#include "target.h"

#include "audio_internal.h"
#include "internal.h"
#include "save_internal.h"
#include "ui/internal.h"

#include <rmp/app.h>
#include <rmp/input.h>
#include <rmp/object.h> // rmp::Callback
#include <rmp/random.h>
#include <rmp/scene.h>

#include <raylib.h>
#include <smoke_test.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr float STEP = 1.0f / 60;
constexpr std::uint64_t SEED = 1;
// What says this is the pinned build image: tools/versions_check.sh reads the
// same file.
constexpr std::string_view IMAGE_MANIFEST = "/etc/raylib-build-image.json";

enum class Mode { REPORT, COMPARE, UPDATE };

// The script, and how far the game has got through it.
struct {
    e2e::Script script;
    std::size_t next_input = 0; // the next step to look at for input
    std::size_t next_check = 0; // and for a check
    int frame = 0; // the frame being played, from 0
    int checks = 0;
    int failures = 0;
    bool ended = false; // `end` was reached
    Mode mode = Mode::REPORT;
    fs::path golden;
    fs::path out;
    // The controls already reported as out of reach, each said once.
    std::vector<std::string> unreachable;
    // When it started, for the line that says how long it took: real time,
    // which the game never sees. A duration since the clock's epoch, because a
    // time_point's constructor may throw and this is built before main().
    std::chrono::steady_clock::duration started{};
} run;

// What the script holds down: this frame's devices, the last frame's (a
// press is the difference), and what lets go on its own next frame -- and
// what it typed this frame, which a text field takes as it reads it, the way
// raylib's character queue empties.
struct {
    rmp::input::detail::DeviceState now;
    rmp::input::detail::DeviceState before;
    std::vector<e2e::Step> tapped;
    bool clicked = false;
    std::vector<int> typed;
} hands;

void fail(int line, const std::string &why) {
    ++run.failures;
    std::printf("E2E FAIL %d: %s\n", line, why.c_str());
}

// ---- the ears ------------------------------------------------------------------

// What the framework said about the game's mistakes, in the log's words. A
// report is a WARNING (an ERROR under [dev] strict) logged right after
// rmp::detail::report_count() went up (src/rmp/report.cpp), so the log line
// that finds the count higher than the last one did is the report.
struct {
    int heard = 0; // report_count() at the last report line
    std::vector<std::string> lines;
} said;

std::string_view log_prefix(int level) {
    switch (level) {
        case LOG_TRACE:
            return "TRACE: ";
        case LOG_DEBUG:
            return "DEBUG: ";
        case LOG_INFO:
            return "INFO: ";
        case LOG_WARNING:
            return "WARNING: ";
        case LOG_ERROR:
            return "ERROR: ";
        case LOG_FATAL:
            return "FATAL: ";
        default:
            return "";
    }
}

// raylib's TraceLogCallback. A callback replaces raylib's printing, so this
// prints what raylib would have (utils.c, TraceLog()) -- and ends the program
// on a FATAL, which raylib skips when there is a callback.
// NOLINTBEGIN(custom-rmp-raw-pointer,cppcoreguidelines-pro-type-vararg): raylib's signature
void hear(int level, const char *format, va_list args) {
    // NOLINTEND(custom-rmp-raw-pointer,cppcoreguidelines-pro-type-vararg)
    va_list counting;
    va_copy(counting, args);
    const int length = std::vsnprintf(nullptr, 0, format, counting);
    va_end(counting);
    std::string message(static_cast<std::size_t>(std::max(length, 0)), '\0');
    std::vsnprintf(message.data(), message.size() + 1, format, args);
    const std::string line = std::string(log_prefix(level)) + message;
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    if (level >= LOG_WARNING && rmp::detail::report_count() > said.heard) {
        said.heard = rmp::detail::report_count();
        said.lines.push_back(line);
    }
    if (level == LOG_FATAL) std::exit(EXIT_FAILURE);
}

// Every report of the run is a failure at `line`: the run is over.
void reported(int line) {
    const int count = rmp::detail::report_count();
    for (const std::string &heard : said.lines) {
        fail(line,
             "the framework reported a mistake of the game's during the run: " + heard);
    }
    // Said where the log could not be heard: the count is the proof.
    if (std::cmp_greater(count, said.lines.size())) {
        fail(line,
             "the framework reported " +
                 std::to_string(count - static_cast<int>(said.lines.size())) +
                 " more mistake(s) of the game's during the run; the log has them");
    }
}

// Read twice rather than kept: the driver holds no C string, and getenv()
// answers the same twice with nothing in between.
bool environment_is_one(const std::string &name) {
    if (std::getenv(name.c_str()) == nullptr) return false;
    // NOLINTNEXTLINE(clang-analyzer-core.NonNullParamChecker): not null, the line above says so
    return std::string_view(std::getenv(name.c_str())) == "1";
}

bool in_the_image() {
    std::error_code error;
    return fs::is_regular_file(fs::path(IMAGE_MANIFEST), error);
}

// ---- the hands ---------------------------------------------------------------

std::size_t slot(int key) { return static_cast<std::size_t>(key); }
bool down(int key) { return hands.now.keys[slot(key)]; }
bool pressed(int key) {
    return hands.now.keys[slot(key)] && !hands.before.keys[slot(key)];
}
bool pad_down(int button) { return hands.now.pad[slot(button)]; }
bool pad_pressed(int button) {
    return hands.now.pad[slot(button)] && !hands.before.pad[slot(button)];
}

// What a key step works: a key, or one of the first gamepad's buttons.
bool &held(const e2e::Step &step) {
    return step.pad ? hands.now.pad[slot(step.key)] : hands.now.keys[slot(step.key)];
}

void sample(rmp::input::detail::DeviceState &out) { out = hands.now; }

// The UI's navigation, from the script's keyboard and gamepad: the same two
// functions nav_from_raylib() reads real ones with (src/rmp/ui/internal.h).
// The sticks rest at 0: a script has buttons and no axes.
void navigate(rmp::ui::detail::NavState &out) {
    out = rmp::ui::detail::nav_from_keys(
        [](int key) { return down(key); }, [](int key) { return pressed(key); },
        [](int &x, int &y, bool &submit, bool &cancel) {
            rmp::ui::detail::nav_from_pad(
                [](int button) { return pad_down(button); },
                [](int button) { return pad_pressed(button); },
                [](int axis) { return hands.now.axes[slot(axis)]; }, x, y, submit,
                cancel);
        });
}

// What the text field that has the keyboard types: this frame's `type`, and
// one Backspace for a tap of KEY_BACKSPACE -- the UI's seam for it,
// read_typed() (src/rmp/ui/internal.h), which reads raylib's queue in a game.
// Everything a `type` may hold has to fit what the field reads in a frame, or
// the end of one would be dropped below without a word.
static_assert(e2e::MAX_TYPED_PER_FRAME <= rmp::ui::detail::MAX_TYPED,
              "a `type` the grammar allows must fit in what the text field reads");
void typing(rmp::ui::detail::Typed &out) {
    out = rmp::ui::detail::Typed{};
    for (const int cp : hands.typed) {
        if (out.count < rmp::ui::detail::MAX_TYPED)
            out.codepoints[static_cast<std::size_t>(out.count++)] = cp;
    }
    hands.typed.clear();
    out.backspaces = pressed(KEY_BACKSPACE) ? 1 : 0;
}

void point(Clay_Vector2 &position, bool &button) {
    position = Clay_Vector2{ hands.now.pointer.x, hands.now.pointer.y };
    button = hands.now.mouse[MOUSE_BUTTON_LEFT];
}

// No sound device, whatever the machine has: the same run everywhere.
bool no_sound() { return false; }

// The devices for `frame`: what a tap or a click held for one frame lets go,
// then every input step of that frame, in the order written.
void hands_for(int frame) {
    hands.before = hands.now;
    for (const e2e::Step &tap : hands.tapped) held(tap) = false;
    hands.tapped.clear();
    if (hands.clicked) {
        hands.now.mouse[MOUSE_BUTTON_LEFT] = false;
        hands.clicked = false;
    }
    hands.typed.clear(); // what nothing read is gone, as from raylib's queue
    const std::vector<e2e::Step> &steps = run.script.steps;
    while (run.next_input < steps.size() && steps[run.next_input].frame <= frame) {
        const e2e::Step &step = steps[run.next_input];
        ++run.next_input;
        switch (step.verb) {
            case e2e::Verb::PRESS:
                held(step) = true;
                break;
            case e2e::Verb::RELEASE:
                held(step) = false;
                break;
            case e2e::Verb::TAP:
                held(step) = true;
                hands.tapped.push_back(step);
                break;
            case e2e::Verb::CLICK:
                hands.now.pointer =
                    Vector2{ static_cast<float>(step.x), static_cast<float>(step.y) };
                hands.now.mouse[MOUSE_BUTTON_LEFT] = true;
                hands.clicked = true;
                break;
            case e2e::Verb::TYPE:
                hands.typed = step.typed;
                break;
            case e2e::Verb::SHOT:
            case e2e::Verb::EXPECT_SCENE:
            case e2e::Verb::END:
                break; // the eyes' steps, at the end of the frame
        }
    }
}

// ---- the eyes ------------------------------------------------------------------

std::string shown(const fs::path &path) { return path.generic_string(); }

void shoot(const e2e::Step &step) {
    ++run.checks;
    e2e::OwnedImage frame(SmokeTest_Upright(LoadImageFromScreen()));
    const e2e::Picture actual = e2e::picture_of(frame.image());
    std::printf("E2E SHOT %s %dx%d hash=%08x\n", step.name.c_str(), actual.width,
                actual.height, static_cast<unsigned>(e2e::fingerprint(actual)));
    const fs::path golden = run.golden / (step.name + ".png");

    if (run.mode == Mode::UPDATE) {
        if (e2e::write_png(actual, golden)) {
            std::printf("E2E RECORDED %s\n", shown(golden).c_str());
        } else {
            fail(step.line, "could not write the golden " + shown(golden));
        }
        return;
    }

    const std::optional<e2e::Picture> expected = e2e::load_png(golden);
    if (!expected) {
        e2e::write_png(actual, run.out / (step.name + ".actual.png"));
        const std::string why = "shot `" + step.name + "` has no golden, " +
            shown(golden) + ": record it inside the image with `rmp test e2e-update`";
        if (run.mode == Mode::COMPARE) {
            fail(step.line, why);
        } else {
            std::printf("E2E NOTE %d: %s\n", step.line, why.c_str());
        }
        return;
    }

    // Exact: see compare.h for why the tolerance is 0.
    const e2e::Difference d = e2e::compare(*expected, actual, 0);
    if (d.none()) return;
    const bool wrote = e2e::write_failure(*expected, actual, run.out, step.name);
    std::string why = "shot `" + step.name + "` at frame " + std::to_string(step.frame);
    if (d.same_size) {
        why += " differs from its golden in " + std::to_string(d.pixels) +
            " pixel(s), by up to " + std::to_string(d.largest) + ", the first at (" +
            std::to_string(d.first_x) + ", " + std::to_string(d.first_y) + ")";
    } else {
        why += " is " + std::to_string(actual.width) + "x" +
            std::to_string(actual.height) + " and its golden " +
            std::to_string(expected->width) + "x" + std::to_string(expected->height);
    }
    why += wrote ? "; expected, actual and diff in " + shown(run.out)
                 : "; and the three pictures could not be written to " + shown(run.out);
    if (run.mode == Mode::COMPARE) {
        fail(step.line, why);
    } else {
        std::printf(
            "E2E DIFF %d: %s -- reported, not enforced: the goldens are exact inside "
            "the pinned build image only\n",
            step.line, why.c_str());
    }
}

void expect_scene(const e2e::Step &step) {
    ++run.checks;
    std::string top = "no scene at all";
    if (rmp::Scene::depth() > 0) {
        const rmp::Scene &scene = rmp::Scene::current();
        top = e2e::readable_type(typeid(scene).name());
    }
    if (!e2e::names_class(top, step.name)) {
        fail(step.line,
             "expected " + step.name + " on top after frame " +
                 std::to_string(step.frame) + ", and it is " + top);
    }
}

void finish(const e2e::Step &step) {
    run.ended = true;
    reported(step.line);
    const std::chrono::duration<double> took =
        std::chrono::steady_clock::now().time_since_epoch() - run.started;
    std::printf("E2E %s: %d check(s) over %d frame(s), %d failed, in %.1f s\n",
                std::string(e2e::target::NAME).c_str(), run.checks, step.frame + 1,
                run.failures, took.count());
    if (run.failures == 0) std::printf("E2E PASS\n");
    rmp::app::quit();
}

// Every frame, whatever the script checks: a control the example drew
// where nobody can reach it -- outside its window, in no scroll area --
// cannot be seen or clicked by anyone, and no shot of it would say so if the
// script never looked down there. ui/04_settings drew its Apply button below
// the bottom of its own 800x450 window, and the style page half of itself.
void reach() {
    for (const std::string &name : rmp::ui::detail::controls_out_of_reach()) {
        if (std::ranges::find(run.unreachable, name) != run.unreachable.end()) continue;
        run.unreachable.push_back(name);
        fail(0,
             "frame " + std::to_string(run.frame) + " lays out the control `" + name +
                 "` outside its " + std::to_string(GetScreenWidth()) + "x" +
                 std::to_string(GetScreenHeight()) +
                 " window and in no scroll area: nobody can see it or click it");
    }
}

// The end of every frame: the seed, once, what nobody can reach, then this
// frame's checks, then the next frame's input.
void on_frame_end() {
    if (run.ended) return;
    if (run.frame == 0 && rmp::random::current_seed() != SEED) {
        fail(0,
             "rmp::random's seed is " + std::to_string(rmp::random::current_seed()) +
                 " after frame 0, not " + std::to_string(SEED) +
                 ": begin_run() did not take set_test_seed(), or the game seeded it "
                 "itself -- and a run seeded differently is a different run");
    }
    reach();
    const std::vector<e2e::Step> &steps = run.script.steps;
    while (run.next_check < steps.size() && steps[run.next_check].frame <= run.frame) {
        const e2e::Step &step = steps[run.next_check];
        ++run.next_check;
        if (step.verb == e2e::Verb::SHOT) shoot(step);
        if (step.verb == e2e::Verb::EXPECT_SCENE) expect_scene(step);
        if (step.verb == e2e::Verb::END) {
            finish(step);
            return;
        }
    }
    ++run.frame;
    hands_for(run.frame);
}

// A game that stops before `end` -- a crash is caught by the shell, but a
// Quit pressed too early, or quit() from the game, ends main() normally.
// And the exit status, which main() of the game decides: 0. With a failure
// it has to be something else, and _Exit is how, after the output is out.
void at_exit() {
    if (!run.ended) {
        const e2e::Step &end = run.script.steps.back();
        fail(end.line,
             "the game stopped after " + std::to_string(run.frame) +
                 " frame(s), before `end` at frame " + std::to_string(end.frame));
        reported(end.line);
    }
    if (run.failures > 0) {
        std::fflush(stdout);
        std::_Exit(1);
    }
}

[[noreturn]] void refuse(int line, const std::string &why) {
    std::printf("E2E FAIL %d: %s\n", line, why.c_str());
    std::fflush(stdout);
    std::exit(1);
}

// Pictures this run will write: the last run's go first, so what is there
// afterwards is this run's. Only *.png in the folder, and the save folder,
// by name -- nothing else is ours to remove.
void clear_pictures(const fs::path &dir) {
    std::error_code error;
    fs::create_directories(dir, error);
    std::vector<fs::path> old;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir, error)) {
        if (entry.path().extension() == ".png") old.push_back(entry.path());
    }
    for (const fs::path &path : old) fs::remove(path, error);
}

void install() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    SetTraceLogCallback(hear);

    const std::string script(e2e::target::SCRIPT);
    const std::ifstream file(script, std::ios::binary);
    if (!file) refuse(0, "cannot read the script " + script);
    std::ostringstream text;
    text << file.rdbuf();
    e2e::ParseError error;
    if (!e2e::parse(text.str(), run.script, error)) {
        refuse(error.line, error.reason + " (" + script + ")");
    }

    run.golden = fs::path(e2e::target::GOLDEN);
    run.out = fs::path(e2e::target::OUT);
    if (environment_is_one("RMP_E2E_UPDATE")) {
        if (!in_the_image()) {
            refuse(
                0,
                "RMP_E2E_UPDATE records goldens, and only inside the pinned build image: "
                "they are compared exactly, so they come from one compiler and one "
                "renderer: tools/dev_shell.sh bash -c 'python3 tools/configure.py && "
                "./rmp test e2e-update'");
        }
        run.mode = Mode::UPDATE;
        clear_pictures(run.golden);
    } else if (environment_is_one("RMP_E2E_COMPARE")) {
        run.mode = Mode::COMPARE;
    }
    clear_pictures(run.out);
    std::error_code removed;
    fs::remove_all(run.out / "save", removed);
    fs::create_directories(run.out / "save", removed);

    rmp::app::detail::set_fixed_delta(STEP);
    rmp::app::detail::set_test_seed(SEED);
    rmp::app::detail::set_frame_observer([] { on_frame_end(); });
    rmp::input::detail::set_sample_provider(sample);
    rmp::ui::detail::set_nav_provider(navigate);
    rmp::ui::detail::set_pointer_provider(point);
    rmp::ui::detail::set_text_provider(typing);
    rmp::audio::detail::set_device_opener(no_sound);
    rmp::save::detail::set_folders_for_tests("", (run.out / "save").string());
    std::atexit(at_exit);

    std::string mode = "compared and reported, not enforced";
    if (run.mode == Mode::UPDATE) mode = "recording the goldens";
    if (run.mode == Mode::COMPARE) mode = "compared exactly";
    run.started = std::chrono::steady_clock::now().time_since_epoch();
    std::printf("E2E START %s: %d frame(s), %s\n", std::string(e2e::target::NAME).c_str(),
                run.script.steps.back().frame + 1, mode.c_str());
    hands_for(0);
}

// Before main(), so that the first frame already runs on the script's clock,
// devices and disk. Everything it sets lives in constant-initialised storage
// (src/rmp/internal.h says so for the clock), so the order the linker puts
// this file in does not matter.
struct Install {
    Install() noexcept { install(); }
};
const Install INSTALL;

} // namespace
