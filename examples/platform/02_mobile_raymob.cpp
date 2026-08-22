// ---------------------------------------------------------------------------
// examples/platform/02_mobile_raymob.cpp
//
// How to use the raymob mobile API (thirdparty/raymob/raymob.h): vibration,
// soft keyboard, sensors, screen orientation, keep-screen-on and app storage.
//
// Unlike <admob.h>, these functions exist ONLY on Android — raymob.h declares
// nothing elsewhere. So guard the calls with #ifdef __ANDROID__ (or keep them
// in Android-only code paths). The rest of your game still runs on desktop;
// these features simply compile out off-device.
//
//   Vibrate / VibrateMS / VibrateEx     -> haptics
//   ShowSoftKeyboard / HideSoftKeyboard -> on-screen keyboard
//   GetLastSoftKeyChar / ...            -> read what was typed
//   InitSensorManager / EnableSensor    -> accelerometer / gyroscope
//   GetAccelerotmerAxis / GetGyroscopeAxis
//   KeepScreenOn(bool)                  -> stop the screen from sleeping
//   GetScreenOrientation()              -> portrait / landscape
//   GetAppStoragePath / WriteToAppStorage / ReadFromAppStorage
//
// This file is REFERENCE ONLY (not compiled by the build). See README.md.
// ---------------------------------------------------------------------------

#include <raylib.h>
#ifdef __ANDROID__
#include <raymob.h>
#endif

static void on_ready() {
    InitWindow(800, 450, "raymob mobile example");

#ifdef __ANDROID__
    KeepScreenOn(true); // don't let the screen sleep
    InitSensorManager(); // needed before enabling any sensor
    EnableSensor(SENSOR_ACCELEROMETER); // turn the accelerometer on
#endif
}

static void on_frame() {
#ifdef __ANDROID__
    // Haptics: vibrate for 50 ms whenever the screen is tapped.
    if (IsKeyPressed(KEY_BACK) || IsGestureDetected(GESTURE_TAP)) VibrateMS(50);

    // Soft keyboard: toggle it and read the last typed character.
    if (IsKeyPressed(KEY_K)) ShowSoftKeyboard();
    if (IsKeyPressed(KEY_H)) HideSoftKeyboard();
    char lastKey = GetLastSoftKeyChar(); // 0 if nothing new was typed
    ClearLastSoftKey();

    // Sensors: read the accelerometer axis.
    Vector3 accel = GetAccelerotmerAxis();

    Orientation orient = GetScreenOrientation();
#endif

    BeginDrawing();
    ClearBackground(RAYWHITE);
#ifdef __ANDROID__
    DrawText(TextFormat("accel: %.2f %.2f %.2f", accel.x, accel.y, accel.z), 10, 40, 20,
             DARKGRAY);
    DrawText(TextFormat("last key: %c   orientation: %d", lastKey ? lastKey : '-',
                        (int)orient),
             10, 80, 20, DARKGRAY);
#else
    DrawText("raymob mobile features only run on Android", 10, 40, 20, GRAY);
#endif
    EndDrawing();
}

static void on_exit() {
#ifdef __ANDROID__
    DisableSensor(SENSOR_ACCELEROMETER);
#endif
    CloseWindow();
}

int main() {
    on_ready();
    while (!WindowShouldClose()) on_frame();
    on_exit();
    return 0;
}
