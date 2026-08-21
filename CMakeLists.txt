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

#basically this will change the RESOURCES_PATH to be the local path

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER   ${CCACHE_PROGRAM})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

option(PRODUCTION_BUILD "Make this a production build" OFF)

# [dev] compiler/linker from the config. Deliberately only for development
# builds: CI builds `release`/`web`, so nothing here can change what ships.
# Must precede project(), which is where CMake locks in the compiler.
if(NOT PRODUCTION_BUILD)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/generated/dev_toolchain.cmake" OPTIONAL)
endif()

if(MSVC)
  # Static CRT in every configuration (/MT for Release, /MTd for Debug)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/arch:AVX2) #make sure SIMD optimizations take place
endif()


set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build Raylib as static libraries" FORCE)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Generate compile_commands.json" FORCE)

project(${TEMPLATE_PROJECT_NAME})
add_executable("${CMAKE_PROJECT_NAME}")



#Add raylib, and here you would add other libraries if needed!
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
# remove compile warnings to raylib
if (MSVC)
  # Ensure the linker deletes death code
  target_link_options("${CMAKE_PROJECT_NAME}" PRIVATE /OPT:REF /OPT:ICF)
  # msvc doesnt have this flag but the error doesnt occur with it
  target_compile_options(raylib PRIVATE)
elseif(EMSCRIPTEN)
  # Emscripten handles dead code elimination differently, no extra flags needed
  # wasm-ld has its own --gc-sections but it's enabled by default
elseif(APPLE)
  # Apple ld64 uses -dead_strip instead of --gc-sections
  target_link_options("${CMAKE_PROJECT_NAME}" PRIVATE -Wl,-dead_strip)
  target_compile_options("${CMAKE_PROJECT_NAME}" PRIVATE -ffunction-sections -fdata-sections)
  target_compile_options(raylib PRIVATE -ffunction-sections -fdata-sections -Wno-stringop-overflow)
else()
  # GNU ld (Linux, MinGW)
  target_link_options("${CMAKE_PROJECT_NAME}" PRIVATE -Wl,--gc-sections)
  target_compile_options("${CMAKE_PROJECT_NAME}" PRIVATE -ffunction-sections -fdata-sections)
  target_compile_options(raylib PRIVATE -ffunction-sections -fdata-sections -Wno-stringop-overflow)
endif()

# MY_SOURCES is defined to be a list of all the source files for my game
# DON'T ADD THE SOURCES BY HAND, they are already added with this macro
file(GLOB_RECURSE MY_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)

# RESOURCES_PATH macro used by the game code to locate its assets
if(PRODUCTION_BUILD)
  # Production: relative to the executable, ship resources/ next to it
  set(RESOURCES_PATH "./resources/")
else()
  # Development: absolute path so the game loads its assets no matter
  # which working directory it is launched from
  set(RESOURCES_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/")
endif()
target_compile_definitions("${CMAKE_PROJECT_NAME}" PUBLIC RESOURCES_PATH="${RESOURCES_PATH}")


set_property(TARGET "${CMAKE_PROJECT_NAME}" PROPERTY CXX_STANDARD 20)

# RAYLIB_STUB_SOURCES is empty unless a module was disabled that rres still
# references. It lives in cmake/generated/, never in src/, which is the user's.
target_sources("${CMAKE_PROJECT_NAME}" PRIVATE ${MY_SOURCES} ${RAYLIB_STUB_SOURCES})

if(PRODUCTION_BUILD)
  # setup the ASSETS_PATH macro to be in the root folder of your exe
  # remove the option to debug asserts.
  target_compile_definitions("${CMAKE_PROJECT_NAME}" PUBLIC PRODUCTION_BUILD=1)
  if(MSVC)
    set_target_properties("${CMAKE_PROJECT_NAME}" PROPERTIES
            LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
    target_compile_options("${CMAKE_PROJECT_NAME}" PRIVATE /O2 /GL)
    target_compile_options(raylib PRIVATE /O2 /GL)
    set_property(TARGET "${CMAKE_PROJECT_NAME}" PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_property(TARGET raylib PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message("===INITIALIZING PRODUCTION BUILD WITH MSVC===")
  elseif(EMSCRIPTEN)
    # Emscripten/WebAssembly - skip LTO, use -Oz for size optimization
    target_compile_options("${CMAKE_PROJECT_NAME}" PRIVATE -Oz)
    target_compile_options(raylib PRIVATE -Oz)
    message("===INITIALIZING PRODUCTION BUILD WITH EMSCRIPTEN===")
  else() # Linux, macOS, MinGW
    if (MINGW)
      target_link_options("${CMAKE_PROJECT_NAME}" PRIVATE -mwindows)
    endif()
    target_compile_options("${CMAKE_PROJECT_NAME}" PRIVATE -O3)
    target_compile_options(raylib PRIVATE -O3)
    if(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
      # NetBSD's linker does not process the GCC LTO bytecode stored in static
      # archives, leaving raylib symbols (TraceLog, LoadImage, ...) undefined at
      # link time. Skip LTO there; keep it everywhere else.
      message("===INITIALIZING PRODUCTION BUILD WITH GCC OR CLANG (NetBSD: LTO disabled)===")
    else()
      set_property(TARGET "${CMAKE_PROJECT_NAME}" PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
      set_property(TARGET raylib PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
      message("===INITIALIZING PRODUCTION BUILD WITH GCC OR CLANG===")
    endif()
  endif()
  # There´s no console to quit on Linux/MacOS
else()
  target_compile_definitions("${CMAKE_PROJECT_NAME}" PUBLIC PRODUCTION_BUILD=0)
  if (MSVC)
    target_compile_options("${CMAKE_PROJECT_NAME}" PRIVATE /Od /Zi)
    message("===INITIALIZING DEBUGGING WITH MSVC===")
  else()
    target_compile_options("${CMAKE_PROJECT_NAME}" PRIVATE -O0)

    message("===INITIALIZING DEBUGGING WITH GCC OR CLANG===")
  endif()
endif()

message(STATUS "=== RESOURCES PATH: ${RESOURCES_PATH} ===")

target_include_directories("${CMAKE_PROJECT_NAME}" PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rres"
  "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/raymob"
  "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/clay"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests"
)

# --- rres resource packing --------------------------------------------------
# Password shared by the packer and the runtime loader. Obfuscation only (the
# password ends up in the binary). Override with -DRRES_PACK_PASSWORD="...".
if(NOT DEFINED RRES_PACK_PASSWORD)
  set(RRES_PACK_PASSWORD "${TEMPLATE_RRES_PASSWORD}")
endif()
target_compile_definitions("${CMAKE_PROJECT_NAME}" PRIVATE RRES_PASSWORD="${RRES_PACK_PASSWORD}")

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
  file(GLOB UI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/rmp/ui/*.cpp")
  add_executable(ui_layout_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_layout_test.cpp" ${UI_SOURCES})
  set_property(TARGET ui_layout_test PROPERTY CXX_STANDARD 20)
  target_include_directories(ui_layout_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/clay"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rres"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/raymob"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests"
  )
  # raylib is linked but never asked to open anything: MeasureTextEx and the
  # drawing calls are all behind the injected seams.
  target_compile_definitions(ui_layout_test PRIVATE RESOURCES_PATH="${RESOURCES_PATH}")
  target_link_libraries(ui_layout_test PRIVATE raylib_static)
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

if(PLATFORM STREQUAL "Web")
  set_target_properties("${CMAKE_PROJECT_NAME}" PROPERTIES
        SUFFIX ".html"
        # No ASYNCIFY, on purpose. It is only needed to run a synchronous while
        # loop in an asynchronous browser, and include/rmp/
        # lifecycle.h hands the frame loop to emscripten_set_main_loop instead.
        # ASYNCIFY is not free: it instruments every function that could be on
        # the stack at a suspension point, which is most of them, and charges
        # for it in code size and speed whether or not anything ever suspends.
        # If you put a while loop back, you have to put this back with it.
        LINK_FLAGS "-s USE_GLFW=3 -s TOTAL_MEMORY=67108864 --preload-file ${CMAKE_SOURCE_DIR}/resources@/resources/"
    )
endif()


# Link raylib library statically, Here you would add other libraries!
if (PRODUCTION_BUILD)

  target_link_libraries("${CMAKE_PROJECT_NAME}" PRIVATE
 raylib_static

)
else()

  target_link_libraries("${CMAKE_PROJECT_NAME}" PRIVATE
 raylib_static
 raymoblib
)
endif()
