cmake_minimum_required(VERSION 3.30)

# --- Project configuration --------------------------------------------------
# raylib_multiplatform.toml is the single source of truth for the project's
# identity. tools/configure.py turns it into the files below plus the Gradle
# properties and the XcodeGen spec; none of that is committed, so it cannot
# drift. This has to run before project(), because the project name comes out
# of it.
include(cmake/configure_hook.cmake)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated/project.cmake")

set(CMAKE_CXX_STANDARD 20)

# i bet you don't want modules here, but if you want, turn this on
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

option(PRODUCTION_BUILD "Make this a production build" OFF)

# DRM/KMS. Passed by the linux-drm CI job and by anyone building for a TTY:
#   cmake --preset release -DRMP_TARGET_DRM=ON
# It picks raylib's PLATFORM=DRM below, which needs libdrm on the build machine
# and /dev/dri on the machine that runs it.
option(RMP_TARGET_DRM "Build for DRM/KMS: no X11, no Wayland, straight to the screen" OFF)

# Every example under examples/ as its own executable, next to the game. Off by
# default because a game build has no business compiling twenty examples; on
# for `just example`, for `just test examples` and for the CI job that boots
# every one of them. See rmp_add_game() below for what an example IS.
option(RMP_BUILD_EXAMPLES "Build every example under examples/ as an executable" OFF)

# [dev] compiler/linker from the config. Deliberately only for development
# builds: CI builds `release`/`web`, so nothing here can change what ships.
# Must precede project(), which is where CMake locks in the compiler.
if(NOT PRODUCTION_BUILD)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated/dev_toolchain.cmake" OPTIONAL)
elseif(DEFINED RMP_DEV_COMPILER)
  # The debug and release presets share ${sourceDir}/build, so `cmake --preset
  # release` can land on a cache that `cmake --preset debug` already filled in.
  # The build type and PRODUCTION_BUILD are overwritten by the new preset. The
  # COMPILER IS NOT: dev_toolchain.cmake wrote it with FORCE, and skipping the
  # include here does not take it back out of the cache.
  #
  # So the release build would quietly use your [dev] compiler instead of the
  # one CI ships with — and when the two disagree about LTO, as clang's
  # -flto=thin and GCC's -flto=auto do, you get "file format not recognized"
  # from the linker with nothing on screen to connect it to a preset you ran
  # ten minutes ago. That cost an afternoon once.
  #
  # Refusing is the honest answer: a release build you cannot trust to be the
  # release build is worth less than no release build at all.
  message(FATAL_ERROR
    "This build directory was configured by the DEBUG preset, which pinned "
    "the compiler to ${RMP_DEV_COMPILER} from [dev] in "
    "raylib_multiplatform.toml. A release build must not inherit it — CI "
    "builds with the platform default, and this one would not match.\n"
    "  Run `just clean` (or delete build/) and configure again.")
endif()

if(MSVC)
  # Static CRT in every configuration (/MT for Release, /MTd for Debug)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/arch:AVX2) #make sure SIMD optimizations take place
endif()


set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build Raylib as static libraries" FORCE)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json" FORCE)

project(${TEMPLATE_PROJECT_NAME})

# [dev] linker, and it has to be HERE rather than with the compiler above:
# choosing a linker means checking that it links, and checking that means
# compiling something, which needs project() to have run. See gen_dev_linker()
# in tools/configure.py for what the check is and why guessing was not enough.
if(NOT PRODUCTION_BUILD)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated/dev_linker.cmake" OPTIONAL)
endif()

# --- raylib -----------------------------------------------------------------
# Which backend raylib is built against. Everything except these two cases is
# raylib's own default (Desktop on the desktops, Web under Emscripten, Android
# under the NDK), and none of that is worth restating here.
#
#   [windows] backend = "win32"  -> PLATFORM=Win32, raylib's native Win32
#                                   backend and no GLFW at all. That branch of
#                                   raylib's CMake is OURS: see the patch note
#                                   in thirdparty/FROZEN_VERSIONS.md.
#   linux-drm                    -> PLATFORM=DRM, straight to the screen with
#                                   no X11 and no Wayland.
if(RMP_TARGET_DRM)
  set(PLATFORM "DRM" CACHE STRING "" FORCE)
  message(STATUS "=== PLATFORM: DRM/KMS, no X11 and no Wayland ===")
elseif(WIN32 AND TEMPLATE_WINDOWS_BACKEND STREQUAL "win32")
  set(PLATFORM "Win32" CACHE STRING "" FORCE)
  message(STATUS "=== WINDOWS BACKEND: native Win32, no GLFW ===")
elseif(WIN32 AND TEMPLATE_WINDOWS_BACKEND STREQUAL "rgfw")
  set(PLATFORM "RGFW" CACHE STRING "" FORCE)
  message(STATUS "=== WINDOWS BACKEND: RGFW ===")
elseif(UNIX AND NOT APPLE AND NOT EMSCRIPTEN AND TEMPLATE_LINUX_BACKEND STREQUAL "rgfw")
  set(PLATFORM "RGFW" CACHE STRING "" FORCE)
  message(STATUS "=== LINUX BACKEND: RGFW ===")
endif()

# [web] backend. The web preset asks for PLATFORM=Web, which means "build for the
# browser"; this turns that into WHICH of raylib's three web backends does it.
#
# Emscripten compiles the code in all three cases — what changes is who raylib
# talks to. "glfw" calls GLFW, and Emscripten emulates GLFW in JavaScript
# (-sUSE_GLFW=3, below, is that shim). "emscripten" calls the browser through
# Emscripten's own API with nothing in between. "rgfw" brings RGFW instead.
if(EMSCRIPTEN AND PLATFORM STREQUAL "Web")
  if(TEMPLATE_WEB_BACKEND STREQUAL "emscripten")
    set(PLATFORM "WebEmscripten" CACHE STRING "" FORCE)
    message(STATUS "=== WEB BACKEND: Emscripten directly, no GLFW shim ===")
  elseif(TEMPLATE_WEB_BACKEND STREQUAL "rgfw")
    set(PLATFORM "WebRGFW" CACHE STRING "" FORCE)
    message(STATUS "=== WEB BACKEND: RGFW ===")
  else()
    message(STATUS "=== WEB BACKEND: GLFW, through Emscripten's shim ===")
  endif()
endif()

# [linux] wayland. raylib 6.0 builds GLFW against X11 by default, so on a
# Wayland desktop the game runs through XWayland — an extra hop, and it is why
# fractional scaling and per-monitor DPI behave oddly there.
#
# Turning it on builds GLFW's native Wayland backend instead. It needs the
# Wayland development packages (libwayland-dev, wayland-protocols,
# libxkbcommon-dev) and it is OFF by default because raylib 6.0's Wayland path
# is still the less travelled one. raylib 6.2 turns it on upstream; when we bump
# to it, this default should flip and this comment should go.
#
# NOT applied to DRM or RGFW: DRM talks to the kernel directly and has no
# compositor at all, and RGFW brings its own windowing.
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN AND NOT ANDROID
   AND NOT RMP_TARGET_DRM AND NOT PLATFORM STREQUAL "RGFW")
  if(TEMPLATE_LINUX_WAYLAND)
    # BOTH, and this used to be Wayland ONLY -- which shipped a binary that did
    # not start at all on an X11 session. GLFW 3.4 can carry both backends and
    # picks between them at startup (GLFW_ANY_PLATFORM, which honours
    # XDG_SESSION_TYPE), so there is no reason to choose at build time and every
    # reason not to: the machine that runs the game is not the one that built it.
    #
    # What `wayland = true` buys is that a Wayland session gets Wayland NATIVELY
    # instead of going through XWayland. An X11 session still gets X11 natively.
    # Neither is a fallback for the other.
    set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11     ON CACHE BOOL "" FORCE)
    message(STATUS "=== LINUX: Wayland AND X11, chosen at startup ===")
  else()
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11     ON  CACHE BOOL "" FORCE)
  endif()
endif()

add_subdirectory(thirdparty/raylib)

# [raylib] disabled_modules. raylib's own SUPPORT_* CMake options are not used:
# they only take effect with CUSTOMIZE_BUILD=ON, and turning that on flips
# roughly fifteen flags that are 0 in config.h to ON, because
# cmake/ParseConfigHeader.cmake maps any uncommented #define to ON without
# reading its value. Supplying the complete set with EXTERNAL_CONFIG_FLAGS is
# the only way to get what the config actually asked for.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated/raylib_flags.cmake")
target_compile_definitions(raylib PRIVATE ${RAYLIB_CONFIG_DEFS})
if (NOT PRODUCTION_BUILD)
  add_subdirectory(thirdparty/raymob)
endif()

# Vendored code is not ours to fix, and its warnings drown out the ones that
# are. raylib builds clean on the compiler its authors use and noisily on the
# next one along: clang does not know GCC's -Wno-stringop-overflow and says so
# once per translation unit, and stb_vorbis.c has a pointer comparison that is
# tautological on every 64-bit target. Neither is a bug we can act on.
#
# So the whole raylib target is compiled quietly, for the same reason
# thirdparty/ is excluded from clang-format and clang-tidy: one rule, applied
# in three places, and the build log left holding only our own output.
#
# -Werror=... survives this. -w inhibits warnings, not errors, so raylib's own
# -Werror=pointer-arith and -Werror=implicit-function-declaration still stop
# the build if they ever fire. Silence is not the same as permission.
if(MSVC)
  target_compile_options(raylib PRIVATE /w)
else()
  target_compile_options(raylib PRIVATE -w)
endif()

# --- what every target of ours is compiled with ------------------------------
# One function, applied to raylib, to the framework library and to every
# executable, so the release flags cannot drift between them. CMake's IPO is
# per target, and a target that links raylib's LTO archive without it hands
# LLVM bitcode to plain ld ("file format not recognized"); applying the same
# function everywhere is what keeps that from ever happening again.
set(RMP_IPO OFF)
function(rmp_apply_compile_flags target)
  if(PRODUCTION_BUILD)
    if(MSVC)
      target_compile_options(${target} PRIVATE /O2 /GL)
      set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    elseif(EMSCRIPTEN)
      # Emscripten/WebAssembly - skip LTO, use -Oz for size optimization
      target_compile_options(${target} PRIVATE -Oz)
    else() # Linux, macOS, MinGW
      target_compile_options(${target} PRIVATE -O3)
      if(NOT CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
        # NetBSD's linker does not process the GCC LTO bytecode stored in static
        # archives, leaving raylib symbols (TraceLog, LoadImage, ...) undefined
        # at link time. Skip LTO there; keep it everywhere else.
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
      endif()
    endif()
  else()
    if(MSVC)
      target_compile_options(${target} PRIVATE /Od /Zi)
    else()
      target_compile_options(${target} PRIVATE -O0)
    endif()
  endif()
  # Dead-code stripping needs every function in its own section; the linker
  # half is in rmp_apply_link_flags. Not on MSVC (/OPT:REF does not need it)
  # and not on Emscripten (wasm-ld gc-sections is on by default).
  if(NOT MSVC AND NOT EMSCRIPTEN)
    target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
  endif()
endfunction()

if(PRODUCTION_BUILD)
  if(MSVC)
    set(RMP_IPO ON)
    message("===INITIALIZING PRODUCTION BUILD WITH MSVC===")
  elseif(EMSCRIPTEN)
    message("===INITIALIZING PRODUCTION BUILD WITH EMSCRIPTEN===")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
    message("===INITIALIZING PRODUCTION BUILD WITH GCC OR CLANG (NetBSD: LTO disabled)===")
  else()
    set(RMP_IPO ON)
    message("===INITIALIZING PRODUCTION BUILD WITH GCC OR CLANG===")
  endif()
else()
  if(MSVC)
    message("===INITIALIZING DEBUGGING WITH MSVC===")
  else()
    message("===INITIALIZING DEBUGGING WITH GCC OR CLANG===")
  endif()
endif()

# Ship the binary without its debug symbols.
#
# Nothing in the release path was taking them out, and under zig cc that is
# not a rounding error. The Linux x64 binary was 4,613,528 bytes, of which
# 3.7 MB was DWARF -- most of it for zig's own bundled libc and compiler-rt,
# code the game never steps through. Stripped it is 913,128, and after
# `upx --best` 301,848, against 303,620 for the same game built by the host's
# gcc. Targeting glibc 2.28 costs 41 KB uncompressed and less than nothing
# compressed; the megabyte was symbols no one was ever going to read.
#
# -Wl,-s rather than a POST_BUILD ${CMAKE_STRIP}, because most of these
# builds are cross-compiles and GNU strip is target-specific: the image's
# x86-64 /usr/bin/strip cannot touch the arm64 or riscv64 output, whereas the
# linker that produced the file is by definition the right tool for it.
#
# Not MSVC: its debug info is already in a separate .pdb. Not Emscripten: at
# -Oz the wasm carries no DWARF and wasm-ld reads -s as something else. Not
# Android: gradle strips the .so itself, and wants the unstripped one first so
# it can upload the symbols to Play for the crash reports.
#
# Checked rather than assumed, for the reason cmake/generated/dev_linker.cmake
# exists: this has to hold on 17 targets, four of which are BSD linkers
# twenty minutes away, and ld64 dropped plain -s years ago (-S and -x are the
# spellings Apple kept). A linker that will not take it simply does not strip.
set(RMP_STRIP_FLAGS "")
if(PRODUCTION_BUILD AND NOT MSVC AND NOT EMSCRIPTEN AND NOT ANDROID)
  include(CheckLinkerFlag)
  if(APPLE)
    set(_rmp_strip "-Wl,-S" "-Wl,-x")
  else()
    set(_rmp_strip "-Wl,-s")
  endif()
  check_linker_flag(CXX "${_rmp_strip}" RMP_LINKER_CAN_STRIP)
  if(RMP_LINKER_CAN_STRIP)
    set(RMP_STRIP_FLAGS ${_rmp_strip})
    message(STATUS "=== RELEASE: stripping symbols at link (${RMP_STRIP_FLAGS}) ===")
  else()
    message(STATUS "=== RELEASE: linker will not strip, shipping symbols ===")
  endif()
endif()

function(rmp_apply_link_flags target)
  # Dead-code stripping, the linker half.
  if(MSVC)
    target_link_options(${target} PRIVATE /OPT:REF /OPT:ICF)
  elseif(EMSCRIPTEN)
    # wasm-ld has its own --gc-sections and it is enabled by default
  elseif(APPLE)
    target_link_options(${target} PRIVATE -Wl,-dead_strip)
  else()
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
  endif()
  if(PRODUCTION_BUILD)
    if(MSVC)
      # There is no console to quit on Windows either
      set_target_properties(${target} PROPERTIES
              LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
    elseif(MINGW)
      target_link_options(${target} PRIVATE -mwindows)
    endif()
    if(RMP_STRIP_FLAGS)
      target_link_options(${target} PRIVATE ${RMP_STRIP_FLAGS})
    endif()
  endif()
endfunction()

rmp_apply_compile_flags(raylib)

# --- RESOURCES_PATH ----------------------------------------------------------
# Where the game reads its loose files from. Production: relative to the
# executable, ship resources/ next to it. Development: an absolute path, so the
# game loads its assets no matter which working directory it is launched from.
if(PRODUCTION_BUILD)
  set(RESOURCES_PATH "./resources/")
else()
  set(RESOURCES_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/")
endif()

# Password shared by the packer and the runtime loader. Obfuscation only (the
# password ends up in the binary). Override with -DRRES_PACK_PASSWORD="...".
if(NOT DEFINED RRES_PACK_PASSWORD)
  set(RRES_PACK_PASSWORD "${TEMPLATE_RRES_PASSWORD}")
endif()

# --- the framework: one library ---------------------------------------------
# Everything under src/rmp/ except app.cpp, compiled ONCE and linked into the
# game, into every example and into the test binaries. Before this there was
# one executable that globbed all of src/ -- the user's game and the framework
# indistinguishable to CMake -- and unit_test re-listed seventeen framework
# sources by hand right under a comment explaining why globbing is safer.
#
# app.cpp stays OUT, and it is the entry point's other half: it references the
# symbol RMP_ENTRY_POINT emits (so that "no entry point" is a link error with a
# name), so it can only live in a translation unit that has one. rmp_add_game()
# compiles it into each executable, with that executable's own RESOURCES_PATH.
#
# The include directories are PUBLIC here and nowhere else in this file: every
# consumer inherits the same list, so a vendored header on this list is on the
# path of every CMake-built target at once. (Android and iOS have their own
# lists -- see the five-path note in CLAUDE.md -- and tests/configure_test.py
# checks all of them agree.)
file(GLOB_RECURSE RMP_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/rmp/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/rmp/*.c"
)
list(REMOVE_ITEM RMP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/rmp/app.cpp")

# RAYLIB_STUB_SOURCES is empty unless a module was disabled that rres still
# references. It lives in cmake/generated/, never in src/, which is the user's.
add_library(rmp STATIC ${RMP_SOURCES} ${RAYLIB_STUB_SOURCES})
set_property(TARGET rmp PROPERTY CXX_STANDARD 20)
target_include_directories(rmp PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rres"
  "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cute_aseprite"
  "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cute_tiled"
  "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/raymob"
  "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/clay"
  "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)
# The library's RESOURCES_PATH is the game's, and it is only a DEFAULT: the
# entry point in each executable overrides it at run time, because an example
# with its own resources/ must not read the game's. See resources_root() in
# src/rmp/internal.h. PRIVATE, so it cannot collide with the executable's.
target_compile_definitions(rmp PRIVATE
  RESOURCES_PATH="${RESOURCES_PATH}"
  RRES_PASSWORD="${RRES_PACK_PASSWORD}")
if(PRODUCTION_BUILD)
  target_compile_definitions(rmp PUBLIC PRODUCTION_BUILD=1)
else()
  target_compile_definitions(rmp PUBLIC PRODUCTION_BUILD=0)
endif()
target_link_libraries(rmp PUBLIC raylib_static)
if(NOT PRODUCTION_BUILD)
  target_link_libraries(rmp PUBLIC raymoblib)
endif()
rmp_apply_compile_flags(rmp)

# --- a game is a directory --------------------------------------------------
# rmp_add_game(<target> <dir>) builds <dir>/src/**.cpp (and .c) plus the entry
# point into one executable that links the framework. <dir>/include/ is on
# its include path if it exists, and <dir>/resources/ is its RESOURCES_PATH
# if it exists (the game's resources/ otherwise). The game itself is
# rmp_add_game(<project name> <this directory>), and every example is
# rmp_add_game(example_<area>_<name> examples/<area>/<name>) -- the same
# function, so an example is a game and a game is an example.
#
# A directory whose src/ holds no C++ at all -- examples/plain_c -- is plain
# raylib: its own main(), no entry point, no framework. It gets raylib and
# nothing else, which is the whole point of that example.
function(rmp_add_game NAME DIR)
  file(GLOB_RECURSE _game_sources CONFIGURE_DEPENDS
    "${DIR}/src/*.cpp"
    "${DIR}/src/*.c"
  )
  # The project's own src/ contains src/rmp/, which is the library above.
  list(FILTER _game_sources EXCLUDE REGEX "/src/rmp/")

  set(_plain_c TRUE)
  foreach(_s ${_game_sources})
    if(_s MATCHES "\\.cpp$")
      set(_plain_c FALSE)
    endif()
  endforeach()

  if(_plain_c)
    add_executable(${NAME} ${_game_sources})
    set_property(TARGET ${NAME} PROPERTY C_STANDARD 99)
    target_link_libraries(${NAME} PRIVATE raylib_static)
    # A C++ executable gets libm through libstdc++; a C one has to ask.
    if(NOT MSVC)
      target_link_libraries(${NAME} PRIVATE m)
    endif()
  else()
    add_executable(${NAME} ${_game_sources} "${CMAKE_CURRENT_SOURCE_DIR}/src/rmp/app.cpp")
    set_property(TARGET ${NAME} PROPERTY CXX_STANDARD 20)
    target_link_libraries(${NAME} PRIVATE rmp)
  endif()

  if(EXISTS "${DIR}/include")
    target_include_directories(${NAME} PRIVATE "${DIR}/include")
  endif()

  if(PRODUCTION_BUILD)
    set(_res "./resources/")
    set(_res_dir "${CMAKE_CURRENT_SOURCE_DIR}/resources")
  elseif(EXISTS "${DIR}/resources")
    set(_res "${DIR}/resources/")
    set(_res_dir "${DIR}/resources")
  else()
    set(_res "${RESOURCES_PATH}")
    set(_res_dir "${CMAKE_CURRENT_SOURCE_DIR}/resources")
  endif()
  target_compile_definitions(${NAME} PRIVATE RESOURCES_PATH="${_res}")
  if(_plain_c)
    if(PRODUCTION_BUILD)
      target_compile_definitions(${NAME} PRIVATE PRODUCTION_BUILD=1)
    else()
      target_compile_definitions(${NAME} PRIVATE PRODUCTION_BUILD=0)
    endif()
  endif()

  rmp_apply_compile_flags(${NAME})
  rmp_apply_link_flags(${NAME})

  if(EMSCRIPTEN)
    # -sUSE_GLFW=3 is NOT set here. raylib adds it itself, PUBLIC, and now only
    # for PLATFORM=Web — see the patch in thirdparty/raylib/src/CMakeLists.txt.
    # Setting it in both places would be two sources of truth for one flag, and
    # the one that drifts is always the copy.
    set_target_properties(${NAME} PROPERTIES
          SUFFIX ".html"
          # No ASYNCIFY, on purpose. It is only needed to run a synchronous while
          # loop in an asynchronous browser, and rmp/app.h hands the frame loop
          # to emscripten_set_main_loop instead. ASYNCIFY is not free: it
          # instruments every function that could be on the stack at a
          # suspension point, which is most of them, and charges for it in code
          # size and speed whether or not anything ever suspends. If you put a
          # while loop back, you have to put this back with it.
          # TOTAL_MEMORY and ALLOW_MEMORY_GROWTH come from [web] in the .toml.
          # The failure mode when it is too small is specific and worth knowing:
          # the tab dies with "Cannot enlarge memory arrays", not a raylib error.
          LINK_FLAGS "-s TOTAL_MEMORY=${TEMPLATE_WEB_TOTAL_MEMORY} \
-s ALLOW_MEMORY_GROWTH=${TEMPLATE_WEB_ALLOW_MEMORY_GROWTH} \
--preload-file ${_res_dir}@/resources/"
      )
  endif()
endfunction()

# --- the game ---------------------------------------------------------------
rmp_add_game("${CMAKE_PROJECT_NAME}" "${CMAKE_CURRENT_SOURCE_DIR}")

# The Windows icon. Windows keeps the icon INSIDE the .exe as a resource, so a
# PNG is not enough: tools/configure.py turns [icon] source into a
# multi-resolution .ico and the one-line .rc below, and the resource compiler
# embeds it. Missing is fine — the generator skips it when Pillow is not
# installed, and an icon is cosmetic. The game only: an example has no icon.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated/app_icon.rc")
  enable_language(RC)
  target_sources("${CMAKE_PROJECT_NAME}" PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated/app_icon.rc")
endif()

message(STATUS "=== RESOURCES PATH: ${RESOURCES_PATH} ===")

# --- the examples -----------------------------------------------------------
# Every examples/<area>/<name>/ (and examples/<name>/) with a src/main.cpp or a
# src/main.c is a game: rmp_add_game(example_<area>_<name>). An example
# directory with sources but NO main -- examples/platform/03_minimal_includes,
# whose whole job is to prove rmp/ui.h stands alone -- is compiled as an object
# library, which is the check without a link. RMP_EXAMPLE_TARGETS lists what
# was made, and tools/examples_build.sh asserts the count against the tree so
# a glob that stops matching cannot look like a folder where everything passes.
if(RMP_BUILD_EXAMPLES)
  set(RMP_EXAMPLE_TARGETS "")
  file(GLOB _rmp_example_mains
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*/*/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*/*/src/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*/src/main.c"
  )
  list(SORT _rmp_example_mains)
  foreach(_main ${_rmp_example_mains})
    get_filename_component(_src_dir "${_main}" DIRECTORY)
    get_filename_component(_dir "${_src_dir}" DIRECTORY)
    file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}/examples" "${_dir}")
    string(REPLACE "/" "_" _name "${_rel}")
    set(_target "example_${_name}")
    rmp_add_game(${_target} "${_dir}")
    list(APPEND RMP_EXAMPLE_TARGETS ${_target})
  endforeach()

  # The header-only examples: a src/ with .cpp files and no main.
  file(GLOB _rmp_example_src_dirs
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*/*/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/examples/*/src"
  )
  foreach(_src_dir ${_rmp_example_src_dirs})
    if(EXISTS "${_src_dir}/main.cpp" OR EXISTS "${_src_dir}/main.c")
      continue()
    endif()
    file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS "${_src_dir}/*.cpp")
    if(NOT _srcs)
      continue()
    endif()
    get_filename_component(_dir "${_src_dir}" DIRECTORY)
    file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}/examples" "${_dir}")
    string(REPLACE "/" "_" _name "${_rel}")
    set(_target "example_${_name}")
    add_library(${_target} OBJECT ${_srcs})
    set_property(TARGET ${_target} PROPERTY CXX_STANDARD 20)
    target_link_libraries(${_target} PRIVATE rmp)
    if(EXISTS "${_dir}/include")
      target_include_directories(${_target} PRIVATE "${_dir}/include")
    endif()
    list(APPEND RMP_EXAMPLE_TARGETS ${_target})
  endforeach()

  list(LENGTH RMP_EXAMPLE_TARGETS _n)
  message(STATUS "=== EXAMPLES: ${_n} targets ===")
  file(WRITE "${CMAKE_BINARY_DIR}/example_targets.txt" "")
  foreach(_t ${RMP_EXAMPLE_TARGETS})
    file(APPEND "${CMAKE_BINARY_DIR}/example_targets.txt" "${_t}\n")
  endforeach()
endif()

# --- rres resource packing --------------------------------------------------
# Open packing tool (alternative to the paid rrespacker). Standalone C, no raylib.
add_executable(rres_pack tools/rres_pack.c tools/md5.c)
target_include_directories(rres_pack PRIVATE tools "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rres")

# Pack every loose resource into resources.rres (AES-encrypted).
#
# LIST_DIRECTORIES false is load-bearing. file(GLOB) lists directories by
# default, and fopen() on a directory succeeds on glibc, so a resources/
# containing a subfolder used to pack that subfolder as a resource — and drop
# every file inside it.
file(GLOB PACK_FILES LIST_DIRECTORIES false "${CMAKE_CURRENT_SOURCE_DIR}/resources/*")
list(FILTER PACK_FILES EXCLUDE REGEX "\\.rres$")
add_custom_target(pack_resources
  COMMAND $<TARGET_FILE:rres_pack> "${CMAKE_CURRENT_SOURCE_DIR}/resources/resources.rres" "${RRES_PACK_PASSWORD}" ${PACK_FILES}
  DEPENDS rres_pack
  COMMENT "Packing resources into resources.rres (AES)"
)
# The pack is flat: names are bare file names and this glob does not recurse.
# So a file in resources/textures/ is not packed, and the four targets that
# ship resources.rres *alone* (Linux x64/arm64, Windows x64, macOS) would ship
# a release without it — working in development, broken only once released.
#
# Streamed music is the other one. LoadMusicStream opens the file itself
# (drwav/drmp3/jar_xm take a path, by design: the point is not to hold the song
# in memory), so it never reaches the loader hook that serves everything else.
set(_PACK_SUBDIRS "")
file(GLOB _RES_ENTRIES "${CMAKE_CURRENT_SOURCE_DIR}/resources/*")
foreach(_f ${_RES_ENTRIES})
  if(IS_DIRECTORY "${_f}")
    get_filename_component(_n "${_f}" NAME)
    list(APPEND _PACK_SUBDIRS "${_n}")
  endif()
endforeach()
if(_PACK_SUBDIRS AND PRODUCTION_BUILD)
  string(JOIN ", " _d ${_PACK_SUBDIRS})
  message(WARNING
    "resources/ has subfolders the pack does not reach: ${_d}\n"
    "  Resource names are flat, so nothing inside them is packed, and Linux, Windows x64 "
    "and macOS releases ship resources.rres on its own.\n"
    "  Move those files up into resources/, or ship the folder next to the executable "
    "yourself. See TECHNICAL.md, \"Resources\".")
endif()

# --- unit tests (doctest) ---------------------------------------------------
# No window, no GPU, milliseconds. Every tests/*_test.cpp except the one that
# has its own main() is globbed in, so a new test file needs no CMake edit; the
# framework comes from the `rmp` library, so a new framework file needs none
# either. (That list used to be seventeen paths written by hand.)
#
#   cmake --preset debug -DBUILD_TESTS=ON && cmake --build build --target unit_test
#   ./build/unit_test -ts=random
option(BUILD_TESTS "Build the doctest unit tests" OFF)
if(BUILD_TESTS)
  # CONFIGURE_DEPENDS, and it is not a nicety: without it a NEW test file is
  # simply not compiled until somebody happens to reconfigure, and a test that
  # does not run looks exactly like a test that passes. tests/input_test.cpp
  # was written, built and reported green while never having been in the binary.
  file(GLOB UNIT_TEST_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/tests/*_test.cpp")
  list(REMOVE_ITEM UNIT_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_layout_test.cpp")

  add_executable(unit_test ${UNIT_TEST_SOURCES})
  set_property(TARGET unit_test PROPERTY CXX_STANDARD 20)
  # SYSTEM for doctest, so its 9 000 lines of macros are not ours to fix.
  target_include_directories(unit_test SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/doctest"
  )
  target_compile_definitions(unit_test PRIVATE RESOURCES_PATH="${RESOURCES_PATH}")
  # An absolute path, so a fixture is found whatever directory the test binary
  # is run from. `just test` runs it from the repo root and CI does too, but
  # "works from the directory we happen to use" is how a test starts failing for
  # somebody with a different habit.
  target_compile_definitions(unit_test PRIVATE
    RMP_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/")
  # The library calls raylib's Unload* functions, so this DOES link raylib —
  # but it never opens a window. The tests use zeroed payloads, and raylib's
  # unloaders are defined on those: UnloadImage is free(nullptr), UnloadTexture
  # guards on id > 0. That is what makes reference counting a unit test.
  target_link_libraries(unit_test PRIVATE rmp)
  rmp_apply_compile_flags(unit_test)
endif()

# --- Headless UI layout test ------------------------------------------------
# Off by default: it is a developer tool, not part of a game build. The layout
# runs with the text measurement, the pointer and the viewport all injected, so
# it needs no window, no GL context and no display — which is what lets it run
# in a container.
#
#   cmake --preset debug -DBUILD_UI_TESTS=ON
#   cmake --build build --target ui_layout_test && ./build/ui_layout_test
option(BUILD_UI_TESTS "Build the headless UI layout test" OFF)
if(BUILD_UI_TESTS)
  add_executable(ui_layout_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_layout_test.cpp")
  set_property(TARGET ui_layout_test PROPERTY CXX_STANDARD 20)
  target_compile_definitions(ui_layout_test PRIVATE RESOURCES_PATH="${RESOURCES_PATH}")
  # raylib is linked but never asked to open anything: MeasureTextEx and the
  # drawing calls are all behind the injected seams.
  target_link_libraries(ui_layout_test PRIVATE rmp)
  rmp_apply_compile_flags(ui_layout_test)
endif()

# Remove the generated pack (e.g. to fall back to loose files in dev).
add_custom_target(unpack_resources
  COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_CURRENT_SOURCE_DIR}/resources/resources.rres"
  COMMENT "Removing resources.rres (back to loose files)"
)

# if you want to see the assembler
# This target generates .s (assembler) files for all sources in the project
# Compatible with GCC/Clang/MinGW and MSVC

# Determine optimization flag based on build type
if(PRODUCTION_BUILD)
  set(ASM_OPT_FLAGS "-O3")
else()
  set(ASM_OPT_FLAGS "-O0")
endif()

# Create defines based on build type (same as the main target)
if(PRODUCTION_BUILD)
  set(ASM_PROD_BUILD "1")
else()
  set(ASM_PROD_BUILD "0")
endif()

# Include paths needed to recompile the sources standalone
set(ASM_INCLUDE_FLAGS
  "-I${CMAKE_CURRENT_SOURCE_DIR}/include"
  "-I${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/raylib/src"
  "-I${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rres"
  "-I${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/raymob"
  "-I${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/clay"
  "-I${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cute_aseprite"
  "-I${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/cute_tiled"
  "-I${CMAKE_CURRENT_SOURCE_DIR}/thirdparty"
  "-I${CMAKE_CURRENT_SOURCE_DIR}/tests"
)

# Create a header file that defines RESOURCES_PATH as a string literal (with quotes included)
# This allows proper string concatenation in C preprocessor
file(WRITE ${CMAKE_BINARY_DIR}/asm_defines.h "#pragma once
#define RESOURCES_PATH \"${RESOURCES_PATH}\"
")

# Add assembler target
# Generates the .s file for src/main.cpp
if(MSVC)
  # MSVC: wrapper batch script invoked through cmd.exe
  string(JOIN " " ASM_INCLUDES_STR ${ASM_INCLUDE_FLAGS})
  file(WRITE "${CMAKE_BINARY_DIR}/generate_asm.bat" "\
@echo off
cd /D ${CMAKE_CURRENT_SOURCE_DIR}
\"${CMAKE_CXX_COMPILER}\" -S -fverbose-asm ${ASM_OPT_FLAGS} ${ASM_INCLUDES_STR} -DPRODUCTION_BUILD=${ASM_PROD_BUILD} -include ${CMAKE_BINARY_DIR}/asm_defines.h %* -o ${CMAKE_BINARY_DIR}/main.s
")
  add_custom_target(assembler
    COMMAND cmd /C ${CMAKE_BINARY_DIR}/generate_asm.bat ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    COMMENT "Generating assembler files"
    VERBATIM
  )
else()
  # GCC/Clang/MinGW: invoke the compiler directly (cmd.exe does not exist on Unix)
  add_custom_target(assembler
    COMMAND ${CMAKE_CXX_COMPILER} -S -fverbose-asm ${ASM_OPT_FLAGS} ${ASM_INCLUDE_FLAGS}
            -DPRODUCTION_BUILD=${ASM_PROD_BUILD}
            -include ${CMAKE_BINARY_DIR}/asm_defines.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
            -o ${CMAKE_BINARY_DIR}/main.s
    COMMENT "Generating assembler files"
    VERBATIM
  )
endif()
