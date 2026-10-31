#pragma once
// ---------------------------------------------------------------------------
// Private to src/rmp/. Deliberately NOT in include/: nothing
// here is part of the framework's surface, and a header the user can reach is a
// header the user will end up depending on.
//
// The public surface is include/rmp/assets.h.
// ---------------------------------------------------------------------------

#include <raylib.h>
#include <rmp/assets.h> // rmp::Font, which assets::detail::load_font_glyphs() returns

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rmp {
template <class... A> class Callback; // rmp/object.h
} // namespace rmp

namespace rmp::app {
enum class Stretch; // rmp/app.h; a scoped enum, so declaring it is enough here
} // namespace rmp::app

namespace rmp::app::detail {

// --- stretch.cpp -----------------------------------------------------------
// How the world fits the window: rmp::app::stretch(), in numbers. `area` is
// the rectangle of the window, in pixels, that the design-size world
// ([window] width x height) is drawn into, and `scale` how many pixels a
// world unit is there -- before the camera's own zoom. With s = min(screen
// width / design width, screen height / design height):
//
//   NONE       area = the screen, scale = 1
//   LETTERBOX  area = design x s, centred on the screen, scale = s
//   EXPAND     area = the screen, scale = s
//
// One function of the mode and the screen, so the camera (scene.cpp,
// camera.cpp) and the interface (rmp::ui, against its own viewport) cannot
// disagree about where the game is. A screen with no area -- or one that is
// not a number, or so small the design fits it at no scale a float holds --
// is the design size at 1:1. Otherwise `scale` is a positive finite number and
// `area` is inside the screen, LETTERBOX's never a hair past it.
struct WorldViewport {
    Rectangle area;
    float scale;
};
WorldViewport world_viewport(rmp::app::Stretch mode, Vector2 screen);

// The same, for the mode in use and the screen the camera sees:
// rmp::objects::detail::view_rect(), the design size while there is no window.
WorldViewport world_viewport();

// --- clock.cpp -------------------------------------------------------------
// The framework's one clock. step_delta() (rmp/app.h) is what every runner
// hands the game, and frame_delta() is what it handed this frame -- which is
// what rmp::ui animates and repeat-scrolls by, so the menus and the game
// cannot disagree about how long a frame was. A test can fix the step and
// the seed and be told when a frame ends: that is the whole of what
// tests/e2e/driver.cpp needs to play a game and get the same pixels twice.
//
// The storage behind these is constant-initialised, so a test's static
// initializer may call them before main() in any order.

// A frame of `raw` seconds as the game is handed it: `max` seconds at most --
// [app] max_delta, RMP_MAX_DELTA -- and never less than 0. A `max` that is not
// a positive finite number switches the ceiling off. What is not a number of
// seconds -- NaN, a negative reading, +inf with no ceiling to cut it to -- is
// 0: a frame in which no time passed. Whatever goes in, what comes out is a
// finite number in [0, max] (or [0, raw] with no ceiling), so no NaN or
// infinity reaches a position through the step. Pure: step_delta() is this
// applied to what raylib timed.
float clamp_delta(float raw, float max);

// What step_delta() returned this frame. Before any runner has called it --
// rmp::ui in a plain raylib loop of the game's own, which nothing steps --
// the frame raylib timed, clamped the same way.
float frame_delta();

// Back to no runner having stepped, for the tests of the line above.
void forget_steps_for_tests();

// More than 0: every frame advances exactly `seconds` -- still clamped by
// [app] max_delta -- whatever the machine took. 0 goes back to the real clock,
// and so does anything that is not a finite number of seconds.
void set_fixed_delta(float seconds);

// What begin_run() seeds rmp::random with, instead of the wall clock. nullopt
// goes back to the wall clock.
void set_test_seed(std::optional<std::uint64_t> seed);
std::optional<std::uint64_t> test_seed();

// Called by end_frame(), last, once a frame -- after EndDrawing(), when the
// frame's pixels are still in a software framebuffer and the scenes it queued
// have been applied. An empty callback removes it.
void set_frame_observer(rmp::Callback<> on_frame_end);
void frame_ended();

// --- console.cpp -----------------------------------------------------------
// The Linux console back, however a DRM/KMS game ends. guard_console() runs
// before InitWindow: on a DRM/KMS build it remembers the terminal raylib is
// about to change and puts it back on a fatal signal, on a sanitizer's way out
// and at exit; on anything else it does nothing.
void guard_console();

// The same, for any terminal: false, and nothing installed, when `fd` is not
// one. tests/console_test.cpp and tests/console_canary.cpp call these.
bool guard_terminal(int fd);
void restore_terminal();
void unguard_terminal();

// What a DRM/KMS game says at start when sudo made it root (SUDO_USER,
// someone other than root): it saves as root, in `saves`. Empty otherwise,
// and on anything but Linux.
std::string sudo_note(bool root, std::string_view sudo_user, std::string_view saves);

} // namespace rmp::app::detail

namespace rmp::assets::detail {

// --- pack.cpp --------------------------------------------------------------
// The .rres container: opening it, closing it, and pulling one entry out.

// Look for RMP_RESOURCES_PATH/resources.rres and open its central directory.
// Returns true if a usable pack was opened. Safe to call twice.
bool open_pack();

// Close it. Safe to call when nothing was open.
void close_pack();

// True between a successful open_pack() and close_pack().
bool pack_is_open();

// Files of resources/ watched for changes while a Debug build runs on the
// desktop (watch.cpp): the index of `name`'s file, or -1 where nothing is
// watched -- a release, a phone, the web, an open pack, a file not there. Its
// generation moves each time the file's bytes change; what loaded it compares.
int watch_file(std::string_view name);
unsigned watch_generation(int index);
// The watched file's bytes now; nothing when it cannot be read (gone, for now).
std::optional<std::string> read_watched(int index);
void forget_watched(); // for the tests: watch nothing again
void poll_watched_now(); // for the tests: the next query looks at the files

// One entry, by its name in the central directory. Returns bytes owned by the
// caller (malloc'd, so UnloadFileData/RL_FREE frees them), or nullptr if the
// name is not packed, does not decrypt, or no pack is open. `Size` receives
// the byte count and is zeroed on failure.
unsigned char *pack_read(const char *name, int *size);

// Whether the pack holds an entry by this name, without reading it.
bool pack_has(const char *name);

// Whether an asset by this name exists at all: in the pack when one is open,
// otherwise as a loose file under resources_root(). Reads nothing. Defined in
// assets.cpp, where the loose path is built.
bool resource_exists(const char *name);

// An image, decoded. Images are the one resource rrespacker stores decoded
// (an IMGE chunk) rather than as the original file, so this cannot go through
// pack_read(). Returns a zeroed Image when the name is not packed or does not
// decode; the caller then falls back to the loose file.
// `::Image` and not `Image`, and this one cost a link error on exactly ONE of
// the seventeen targets. Since rmp::Image exists, the unqualified name inside
// rmp:: means the counted handle — so this declaration meant rmp::Image in
// assets.cpp (which includes rmp/assets.h) and ::Image in pack.cpp (which does
// not). Two translation units, two different functions, one missing symbol at
// link time on Windows ARM64.
//
// Inside rmp::, a raylib type we shadow is always written with ::.
::Image pack_read_image(const char *name);

// --- loader_hook.cpp -------------------------------------------------------
// Routing raylib's own LoadFileData/LoadFileText through the pack.

void install_loader_hook();
void remove_loader_hook();

// True when `path` points inside RMP_RESOURCES_PATH. Exposed here because it is
// the rule that decides what the hook is responsible for, and that rule is
// worth being able to read in one place.
bool in_resources_dir(const char *path);

// --- assets.cpp ------------------------------------------------------------
// Counters behind rmp::assets::requested_loads() / failed_loads(), which the CI boot
// gate reads. Defined in assets.cpp.
struct LoadCounts {
    int requested = 0;
    int failed = 0;
};
extern LoadCounts loads;

// Where loose files live: the directory that RMP_RESOURCES_PATH used to name at
// every call site. It is a runtime value now because the framework is compiled
// ONCE, into the `rmp` library, and linked into the game and into every
// example -- and an example that carries its own resources/ must not read the
// game's. The library starts with the RMP_RESOURCES_PATH it was compiled with, and
// the entry point (src/rmp/app.cpp, compiled into each executable with that
// executable's own RMP_RESOURCES_PATH) overrides it before anything is loaded.
// Always ends in a separator or is empty, exactly like the macro.
const char *resources_root();
void set_resources_root(const char *root);

// The glyphs every .ttf is baked with unless the caller names its own:
// printable ASCII, Latin-1, Latin Extended-A and the common typography.
// raylib's own default is ASCII alone.
std::span<const int> default_codepoints();

// A font baked with exactly `codepoints` (empty: default_codepoints()), NOT
// cached under its name: rmp::ui bakes a file again when a text asks for a
// letter the atlas does not have, and that atlas is no one else's.
rmp::Font load_font_glyphs(std::string_view name, int font_size,
                           std::span<const int> codepoints);

} // namespace rmp::assets::detail

// A printf format the compiler checks: RMP_PRINTF_FORMAT(2, 3) says argument 2
// is the format and the values start at 3. A report_once() whose `%s` was
// handed an int prints garbage, or reads a pointer that is not one, and only
// on the frame the mistake happens -- which in a diagnostic is the one frame
// nobody is watching. GCC and clang check it; elsewhere the attribute is
// nothing, and those builds compile the same calls the checked ones refused.
// mingw-w64 names its own archetype, because its printf is not glibc's.
#if defined(__MINGW32__) && defined(__MINGW_PRINTF_FORMAT)
#define RMP_PRINTF_FORMAT(string_index, first_to_check) \
    __attribute__((format(__MINGW_PRINTF_FORMAT, string_index, first_to_check)))
#elif defined(__GNUC__) || defined(__clang__)
#define RMP_PRINTF_FORMAT(string_index, first_to_check) \
    __attribute__((format(printf, string_index, first_to_check)))
#else
#define RMP_PRINTF_FORMAT(string_index, first_to_check)
#endif

// And the check is an error, not a warning, in every file that reports,
// whatever flags its target was built with: gcc only warns about a format
// under -Wall, and a warning a build does not turn on is not a check.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic error "-Wformat"
#endif

namespace rmp::detail {

// --- resource.cpp: hot reload of a slot ------------------------------------
// The slot remembers the file it was loaded from (an index of watch.cpp, -1
// for none), and the generation of that file its payload is. replace_payload()
// unloads what it held and takes `payload` in its place, its name, count and
// handles kept; false, and nothing changed, for an empty or released slot.
void watch_slot(Slot &slot, int watch);
bool slot_watched(const Slot &slot); // held, and with a file
bool replace_payload(Slot &slot, std::shared_ptr<void> payload);

// --- report.cpp ------------------------------------------------------------
// One diagnostic per call site, for the mistakes the framework notices every
// frame: an action nobody defined, a pop() with nothing under it, a tag that
// is not in the sheet. Sixty copies a second of the same warning is the same
// as no warning at all, and every module used to keep its own "said it
// already" flag. Now there is one table, one function, one rule.
//
// `site` identifies the diagnostic -- the RMP_REPORT_ONCE macro below passes
// the address of a static local, so each expansion is its own site. `key`
// makes it once per site AND value: "no action called \"jump\"" and "no action
// called \"fire\"" are two different mistakes and each one deserves its line.
//
// Printf-style, because the callers pass raylib types and this feeds TraceLog
// -- the one variadic function of ours, and its formats are checked: see
// RMP_PRINTF_FORMAT above.
// Under [dev] strict = true (debug builds only, see set_strict()) the first
// report aborts, so a warning cannot scroll past in a log nobody reads.
// NOLINTNEXTLINE(modernize-avoid-variadic-functions)
void report_once(const void *site, const char *fmt, ...) RMP_PRINTF_FORMAT(2, 3);
// NOLINTNEXTLINE(modernize-avoid-variadic-functions)
void report_once_keyed(const void *site, const char *key, const char *fmt, ...)
    RMP_PRINTF_FORMAT(3, 4);
// The same, keyed by every byte of `key`: a name the game chose -- a sound, a
// song -- can hold a NUL, and two names that differ only after it are two
// mistakes. As a C string they were one line (audio's property found it).
// NOLINTNEXTLINE(modernize-avoid-variadic-functions,custom-rmp-raw-pointer): the site is an address and the format printf's, as in the C string form above
void report_once_keyed(const void *site, std::string_view key, const char *fmt, ...)
    RMP_PRINTF_FORMAT(3, 4);

// [dev] strict. Off unless the entry point turns it on from RMP_DEV_STRICT,
// which the unit tests never do -- they exercise the warnings on purpose.
void set_strict(bool on);
bool strict();

// What strict does on a report: abort(), unless a test has swapped it for a
// counter. Returns the previous handler so a test can put it back.
using StrictHandler = void (*)();
StrictHandler set_strict_handler(StrictHandler handler);

// Who else hears a report: called with the line, as the log has it, each time
// a new one is said -- before [dev] strict stops the program, so it is heard
// even then. The entry point gives it to rmp::ui in a debug build, which puts
// the line on screen (rmp::ui::notify()); nobody else does. An empty callback
// takes it away.
void set_report_sink(rmp::Callback<std::string_view> on_report);

// Tests. How many distinct diagnostics have fired since the last reset, and
// the way to forget them so the same test can watch one fire again.
int report_count();
void reset_reports_for_tests();

} // namespace rmp::detail

// The site is the address of a static local, so each expansion of the macro is
// one line in the log and not one per frame. Both forms take a printf format.
#define RMP_REPORT_ONCE(...)                                            \
    do {                                                                \
        static const char rmp_report_once_site = 0;                     \
        ::rmp::detail::report_once(&rmp_report_once_site, __VA_ARGS__); \
    } while (0)
#define RMP_REPORT_ONCE_KEYED(key, ...)                                              \
    do {                                                                             \
        static const char rmp_report_once_site = 0;                                  \
        ::rmp::detail::report_once_keyed(&rmp_report_once_site, (key), __VA_ARGS__); \
    } while (0)
