// ===========================================================================
// rmp::utils — see include/raylib_multiplatform/utils.h.
// ===========================================================================

#include <raylib_multiplatform/utils.h>

#include <raylib.h>

#if defined(PLATFORM_ANDROID)
#include <raymob.h>                             // GetAndroidApp()
#include <android_native_app_glue.h>            // struct android_app::activity
#include <android/native_activity.h>            // ANativeActivity_finish()
#endif

namespace rmp::utils {

namespace {
bool g_exitRequested = false;
}

void exit() {
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
    //     if (rmp::ui::button("Quit")) rmp::utils::exit();
    //     #endif
    //
    // The CI smoke test still exits the simulator, because a bounded test run
    // has to end — but that path is behind RAY_TEST_MAX_FRAMES, which no
    // shipped app ever sets.
    TraceLog(LOG_WARNING,
             "APP: exit() does nothing on iOS — Apple rejects apps that terminate "
             "themselves (QA1561). Ignoring.");
    return;
#else
    if (g_exitRequested) return;
    g_exitRequested = true;
    TraceLog(LOG_INFO, "APP: exit requested");

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

bool exit_requested() { return g_exitRequested; }

} // namespace rmp::utils
