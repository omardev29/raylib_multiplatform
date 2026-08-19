# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

cmake_minimum_required(VERSION 3.22.1)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

project("${APP_LIB_NAME}")

option(PRODUCTION_BUILD "Make this a production build" OFF)
# Define absolute paths to the raylib_cmake_template root directory
# CMAKE_CURRENT_SOURCE_DIR is raymob/app/src/main/cpp
get_filename_component(USER_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../" ABSOLUTE)
set(USER_SRC_DIR "${USER_ROOT_DIR}/src/")
set(USER_INCLUDE_DIR "${USER_ROOT_DIR}/include/")
set(RAYLIB_DIR "${USER_ROOT_DIR}/thirdparty/raylib")
set(RAYMOB_DIR "${USER_ROOT_DIR}/thirdparty/raymob")
# should be in assets/
# Note: Resources are now copied by Gradle preBuild task (before CMake runs)
set(RESOURCES_PATH "")

# Force static linking for raylib on Android
set(BUILD_SHARED_LIBS_RAYLIB OFF CACHE BOOL "" FORCE)

# Include raylib and raymob as subdirectories (using absolute paths)
add_subdirectory(${RAYLIB_DIR} "${CMAKE_BINARY_DIR}/raylib-build")
add_subdirectory(${RAYMOB_DIR} "${CMAKE_BINARY_DIR}/raymob")

# Android builds its OWN copy of raylib, so [raylib] disabled_modules has to be
# applied here too. Injecting it only in the root CMakeLists.txt would leave
# Android silently linking the full engine while every other platform shrank.
include("${USER_ROOT_DIR}/cmake/generated/raylib_flags.cmake")
target_compile_definitions(raylib PRIVATE ${RAYLIB_CONFIG_DEFS})

# Fetch all source files from the user's src/ directory (recursively)
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${USER_SRC_DIR}/*.c"
    "${USER_SRC_DIR}/*.cpp"
)

# Add android_native_app_glue to sources (CRITICAL for Android!)
include_directories(${ANDROID_NDK}/sources/android/native_app_glue/)
list(APPEND SOURCES ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

# Create a shared library with game source files
add_library(${APP_LIB_NAME} SHARED ${SOURCES} ${RAYLIB_STUB_SOURCES})

# Define compiler macros for the library
target_compile_definitions(${APP_LIB_NAME} PRIVATE
    PLATFORM_ANDROID
    RESOURCES_PATH="${RESOURCES_PATH}"
)

if(NOT PRODUCTION_BUILD)
  target_compile_definitions(${APP_LIB_NAME} PRIVATE _DEBUG DEBUG)
endif()

# Include directories
target_include_directories(${APP_LIB_NAME} PRIVATE
    "${USER_INCLUDE_DIR}"
    "${RAYLIB_DIR}/src"
    "${RAYMOB_DIR}"
    "${USER_ROOT_DIR}/thirdparty/rres"
    "${USER_ROOT_DIR}/thirdparty/clay"
    "${USER_ROOT_DIR}/tests"
)

# Apply optimizations based on build type (same as root CMakeLists.txt)
if(PRODUCTION_BUILD)
  # LTO for release builds
  include(CheckIPOSupported)
  check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_ERROR)

  if(LTO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    message("===INITIALIZING RELEASE BUILD WITH LTO===")
  else()
    message(WARNING "LTO not supported: ${LTO_ERROR}")
  endif()

  # Release optimizations
  target_compile_options(${APP_LIB_NAME} PRIVATE -O3 -flto)
  target_compile_options(raylib PRIVATE -O3 -flto)
else()
  # Debug optimizations
  target_compile_options(${APP_LIB_NAME} PRIVATE -O0)
  target_compile_options(raylib PRIVATE -O0)
  message("===INITIALIZING DEBUG BUILD===")
endif()

# Link required libraries to the native application
target_link_libraries(${APP_LIB_NAME} PRIVATE raylib_static raymoblib)
