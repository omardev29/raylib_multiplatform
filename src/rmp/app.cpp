// ===========================================================================
// rmp::app — see include/rmp/app.h.
//
// This file is where the entry point's includes live. rmp/app.h names nothing
// from rmp::ui or rmp::assets on purpose, so that a translation unit with an
// entry point does not drag the whole interface layer in; the six detail
// functions below are what the macro calls instead.
// ===========================================================================

#include <rmp/app.h>

#include <raylib.h>
#include <rmp/assets.h>
#include <rmp/ui.h>
#include <smoke_test.h>

#include <cstdlib> // std::exit() on the iOS CI path

#if defined(PLATFORM_ANDROID)
#include <raymob.h> // GetAndroidApp()
#include <android_native_app_glue.h> // struct android_app::activity
#include <android/native_activity.h> // ANativeActivity_finish()
#endif

namespace rmp::app {

namespace {
bool g_quit_requested = false;
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
    rmp::ui::shutdown();
    rmp::detail::release_all();
}

// And here, after your hook, only what owns nothing on the GPU: the rres pack
// and raylib's loader hook. Those are file handles, and a file handle does not
// care that the window is gone.
void end_stop() { rmp::assets::shutdown(); }

#if defined(PLATFORM_IOS)
void exit_process() { std::exit(0); }
#endif

} // namespace detail

} // namespace rmp::app
