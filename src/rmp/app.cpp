// ===========================================================================
// rmp::app — see include/rmp/app.h.
//
// This file is where the entry point's includes live. rmp/app.h names nothing
// from rmp::ui or rmp::assets on purpose, so that a translation unit with an
// entry point does not drag the whole interface layer in; the six detail
// functions below are what the macro calls instead.
// ===========================================================================

#include <rmp/app.h>

#include "scene_internal.h"

#include <raylib.h>
#include <rmp/assets.h>
#include <rmp/config.h>
#include <rmp/scene.h>
#include <rmp/ui.h>
#include <smoke_test.h>

#include <cstdlib> // std::exit() on the iOS CI path
#include <memory>
#include <ranges>
#include <utility>
#include <vector>

// The other half of the entry-point guard in rmp/app.h. Referencing the symbol
// here is what turns a program with no RMP_GAME and no RMP_ENTRY_POINT into a
// link error that names the thing you forgot, instead of "undefined reference
// to `main`".
extern "C" void rmp_entry_point_is_declared_exactly_once();
namespace {
void (*const kRequireEntryPoint)() = &rmp_entry_point_is_declared_exactly_once;
} // namespace

#if defined(PLATFORM_ANDROID)
#include <raymob.h> // GetAndroidApp()
#include <android_native_app_glue.h> // struct android_app::activity
#include <android/native_activity.h> // ANativeActivity_finish()
#endif

namespace rmp::app {

namespace {
bool g_quit_requested = false;

// One entry per rmp::global<T>() that has been asked for, in the order they
// were first used. Torn down in reverse, which is the order a reader expects
// from anything that behaves like a static.
std::vector<void (*)()> g_globals;
} // namespace

void quit() {
#if defined(PLATFORM_IOS)
    // Deliberately nothing. Apple's QA1561 is explicit: an iOS app that
    // terminates itself "will appear to the user to have crashed", App Review
    // rejects anything that crashes or looks like it, and a UI control for
    // quitting already fails the Human Interface Guidelines on its own. On top
    // of that, applicationWillTerminate: never runs, so unsaved data is lost.
    //
    // So on iOS this logs and returns. The same source can keep its Quit
    // button and ship everywhere; on iPhone the button simply does nothing,
    // which is the behaviour Apple asks for. Hide it there if you would rather
    // it not be a dead control:
    //
    //     #if !defined(PLATFORM_IOS)
    //     if (rmp::ui::button("Quit")) rmp::app::quit();
    //     #endif
    //
    // The CI smoke test still exits the simulator, because a bounded test run
    // has to end — but that path is behind RAY_TEST_MAX_FRAMES, which no
    // shipped app ever sets.
    TraceLog(LOG_WARNING,
             "APP: quit() does nothing on iOS — Apple rejects apps that terminate "
             "themselves (QA1561). Ignoring.");
    return;
#else
    if (g_quit_requested) return;
    g_quit_requested = true;
    TraceLog(LOG_INFO, "APP: quit requested");

#if defined(PLATFORM_ANDROID)
    // Ending the loop is not enough here. Android owns the Activity, and a
    // process that simply stops leaves the task in the recents list pointing at
    // nothing. finish() is how you tell the system the Activity is done, and it
    // also makes the glue set destroyRequested, so the loop ends on its own
    // even if something else is holding it.
    struct android_app *app = GetAndroidApp();
    if (app != nullptr && app->activity != nullptr) {
        ANativeActivity_finish(app->activity);
    }
#endif
#endif // PLATFORM_IOS
}

bool quit_requested() { return g_quit_requested; }

namespace detail {

// Everything the entry point does before your ready hook. On iOS that includes
// a chdir: the process starts in the app container, not inside the bundle, and
// raylib's iOS backend does not chdir for you — so the relative RESOURCES_PATH
// would resolve to nothing and every asset would silently load as 0x0.
// GetApplicationDirectory() is the .app root there, which is where bundle
// resources live, and it has to happen before assets::init() looks for the pack.
void begin_run() {
    SmokeTest_Begin();
#if defined(PLATFORM_IOS)
    ChangeDirectory(GetApplicationDirectory());
#endif
    rmp::assets::init();
}

void after_ready() {
    SmokeTest_ReportBoot(rmp::assets::failed_loads(), rmp::assets::requested_loads());
}

// Three ways a run ends, and they are checked in one place so that the three
// platform macros cannot drift apart: the window's X (which does not exist on
// web or iOS and is skipped there), the CI frame budget, and quit().
bool keep_running() {
#if defined(PLATFORM_WEB) || defined(__EMSCRIPTEN__) || defined(PLATFORM_IOS)
    // WindowShouldClose() on web is nothing but emscripten_sleep(12), and that
    // sleep is the only thing in raylib that needs ASYNCIFY. Not calling it is
    // what lets ASYNCIFY stay out of the build.
    const bool closed = false;
#else
    const bool closed = WindowShouldClose();
#endif
    return !closed && !SmokeTest_Done() && !quit_requested();
}

void end_frame() { SmokeTest_Tick(); }

// Everything that owns something on the GPU is released HERE, before the stop
// hook runs — because the stop hook is where CloseWindow() lives, and an
// UnloadTexture() after that is a write through a GL context that no longer
// exists.
//
// The resource table joined this list in phase 3 and the comment in rmp/app.h
// had to change with it: it used to say the asset layer could close last
// "because it owns no GPU objects", which stopped being true the moment
// rmp::Texture existed. It cost a segfault on the way out, and only under
// xvfb — a real driver tolerated it and said nothing.
void begin_stop() {
    // ORDER, and every step of it was paid for once.
    //
    //   scenes first. A scene can hold an rmp::Texture as a member, and its
    //   destructor releases a slot in the resource table — so the table has to
    //   still be there. This is the same mistake phase 3 made in the other
    //   direction, and the reason it is written down rather than remembered.
    //   globals next, for exactly the same reason, and after the scenes because
    //   a scene's _end() is entitled to write to one.
    //   then the UI's font, then everything left in the resource table.
    //
    // All of it before the stop hook, because the stop hook is where
    // CloseWindow() lives and every one of these owns something on the GPU.
    rmp::scenes::detail::shutdown();

    // Reverse of first use, which is the order anything that behaves like a
    // static is destroyed in. std::ranges::reverse_view rather than rbegin/rend
    // so that the loop stays a range-based one, which is what the linter asks
    // of every other loop in the codebase.
    for (void (*destroy)() : std::ranges::reverse_view(g_globals)) destroy();
    g_globals.clear();

    rmp::ui::shutdown();
    rmp::detail::release_all();
}

// And here, after your hook, only what owns nothing on the GPU: the rres pack
// and raylib's loader hook. Those are file handles, and a file handle does not
// care that the window is gone.
void end_stop() { rmp::assets::shutdown(); }

void register_global(void (*destroy)()) { g_globals.push_back(destroy); }

// ---------------------------------------------------------------------------
// What RMP_GAME wires the three platform hooks to.
// ---------------------------------------------------------------------------

// The window opens HERE and not in anyone's code. It used to be the first two
// lines of every on_ready(), identical in every project, and getting them from
// [window] in the .toml is what lets the same source describe an 800x450 laptop
// window and a phone held sideways.
void start(rmp::Scene *first) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(APP_WINDOW_WIDTH, APP_WINDOW_HEIGHT, APP_WINDOW_TITLE);
    rmp::scenes::detail::start(first);
}

// One turn of the loop, and the order is the contract. It is documented in
// next_architecture/03-app-and-scenes.md and checked in tests/scene_test.cpp,
// because every interesting property of a scene stack is an ordering property.
void frame(float delta) {
    // 1. The frame boundary. Once, whatever the stack looks like: the pointer
    //    is sampled, the scroll advances and the animation clock ticks exactly
    //    one frame's worth even when three scenes describe UI.
    rmp::ui::detail::begin_frame();

    // 2. Update, bottom upwards, skipping what the scene above freezes.
    rmp::scenes::detail::update(delta);

    // 3/4. Draw. The clear colour comes from the lowest scene that is still
    //      visible, which is what makes a full-screen loading scene work with
    //      one field and no policy.
    BeginDrawing();
    ClearBackground(rmp::scenes::detail::clear_color());
    rmp::scenes::detail::draw();

    // 5. The CI render gate, between the last draw call and EndDrawing().
    //    Neither side of that line works — see tests/smoke_test.h. It used to
    //    be a line in the user's main.cpp; now nobody can put it in the wrong
    //    place because nobody puts it anywhere.
    SmokeTest_CaptureFrame();

    // 6.
    EndDrawing();
    rmp::ui::detail::end_frame();

    // 7. Deferred work, once nothing is mid-frame: scenes queued by change(),
    //    push(), replace() and pop() are applied here and nowhere else.
    rmp::scenes::detail::apply_pending();
}

// The scene stack is already gone by the time this runs — begin_stop() unwinds
// it while the window is still open, because a scene's members can own GPU
// objects. What is left is the window itself.
void stop() { CloseWindow(); }

#if defined(PLATFORM_IOS)
void exit_process() { std::exit(0); }
#endif

} // namespace detail

} // namespace rmp::app
