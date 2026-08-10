#!/bin/bash
# Generates compile_commands.json for Android using NDK toolchain
# Run from project root: ./generate_android_commands.sh

set -e

ANDROID_NDK="${ANDROID_NDK:-$ANDROID_HOME/ndk/26.1.10909125}"
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAYMOB_DIR="$PROJECT_ROOT/raymob"
BUILD_DIR="$RAYMOB_DIR/.cxx"

echo "Using NDK: $ANDROID_NDK"
echo "Project root: $PROJECT_ROOT"

mkdir -p "$BUILD_DIR"

cmake -S "$RAYMOB_DIR/app/src/main/cpp" \
    -B "$BUILD_DIR/compile_commands" \
    -G Ninja \
    -D CMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -D CMAKE_SYSTEM_NAME=Android \
    -D CMAKE_SYSTEM_VERSION=24 \
    -D ANDROID_ABI="arm64-v8a" \
    -D PLATFORM=Android \
    -D APP_LIB_NAME="raymob" \
    -D GLFW_BUILD_X11=ON \
    -D GLFW_BUILD_WAYLAND=ON \
    -D PROJECT_ROOT="$PROJECT_ROOT" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo "compile_commands.json generated at: $BUILD_DIR/compile_commands/compile_commands.json"
