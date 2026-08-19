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
}

bool exit_requested() { return g_exitRequested; }

} // namespace rmp::utils
