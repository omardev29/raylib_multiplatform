# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

cmake_minimum_required(VERSION 3.22.1)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

project("${APP_LIB_NAME}")

# A release is a build type, not an option. Gradle configures its debug variant
# as CMAKE_BUILD_TYPE=Debug and its release variant as RelWithDebInfo, and it
# never passes PRODUCTION_BUILD -- so the option that used to be here was OFF
# for both, and every release APK and AAB was compiled at -O0 with _DEBUG.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "")
  set(PRODUCTION_BUILD OFF)
else()
  set(PRODUCTION_BUILD ON)
endif()
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

# [android] gl_version, from Gradle: what raylib and the game are compiled for,
# and the GLES library the game links. BEFORE raylib is configured, because that
# is when it reads OPENGL_VERSION. See gl_version.cmake.
include("${CMAKE_CURRENT_SOURCE_DIR}/gl_version.cmake")
rmp_android_gl("${GL_VERSION}" RMP_GL_OPENGL RMP_GL_LIBRARY RMP_GL_DEFINE)
set(OPENGL_VERSION "${RMP_GL_OPENGL}" CACHE STRING "" FORCE)
message(STATUS "=== ANDROID: [android] gl_version ${GL_VERSION} -> ${RMP_GL_DEFINE}, ${RMP_GL_LIBRARY} ===")

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

# A designated initialiser out of declaration order is an error, as GCC makes
# it on the desktop targets: the NDK's Clang only warns. RMP_STRICT_CXX_FLAGS in
# the root CMakeLists.txt is the same flag for every other build.
target_compile_options(${APP_LIB_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Werror=reorder-init-list>)

# Define compiler macros for the library
target_compile_definitions(${APP_LIB_NAME} PRIVATE
    PLATFORM_ANDROID
    RMP_RESOURCES_PATH="${RESOURCES_PATH}"
    # The game is compiled for the GL raylib is: rlgl.h is header-only in part,
    # and an ES3 raylib behind an ES2 game is two different rlgl.h.
    ${RMP_GL_DEFINE}
)

if(PRODUCTION_BUILD)
  target_compile_definitions(${APP_LIB_NAME} PRIVATE RMP_PRODUCTION_BUILD=1)
else()
  target_compile_definitions(${APP_LIB_NAME} PRIVATE RMP_PRODUCTION_BUILD=0 _DEBUG DEBUG)
endif()

# Include directories
target_include_directories(${APP_LIB_NAME} PRIVATE
    "${USER_INCLUDE_DIR}"
    "${RAYLIB_DIR}/src"
    "${RAYMOB_DIR}"
    "${USER_ROOT_DIR}/thirdparty/rres"
    "${USER_ROOT_DIR}/thirdparty/clay"
    "${USER_ROOT_DIR}/thirdparty/cute_aseprite"
    "${USER_ROOT_DIR}/thirdparty/cute_tiled"
    "${USER_ROOT_DIR}/thirdparty/cJSON"
    # The bare root too: rmp/math.h includes <raylib-cpp/Vector2.hpp>, which is
    # reached by prefix. It was missing here, so that header did not compile on
    # Android at all -- the same lesson as cute_tiled, one directory up.
    "${USER_ROOT_DIR}/thirdparty"
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
target_link_libraries(${APP_LIB_NAME} PRIVATE raylib_static raymoblib ${RMP_GL_LIBRARY})
