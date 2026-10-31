# The windows-clang-cl preset's toolchain file: the Windows build, made on Linux
# with clang-cl (`rmp build windows`; rmp-docs, "Windows, from Linux").
#
# clang-cl is Clang with MSVC's command line. It compiles for MSVC's C runtime
# and C++ ABI, and LLVM's lld-link links what it writes, as MSVC's own linker
# would: what comes out is the program MSVC would build. What LLVM cannot bring
# is Microsoft's part — the C and C++ runtime, and Windows' headers and
# libraries. xwin downloads those from Microsoft, and this file takes them from
# where xwin put them: XWIN_DIR, ~/.xwin unless you say otherwise. From there
# on CMake configures as it would for clang-cl on Windows, with WIN32 and MSVC
# set, and the build takes MSVC's paths: the static C runtime, no console
# window, resources/ next to the .exe.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# Debian and Ubuntu keep each LLVM version in a folder of its own and put only
# suffixed names on PATH (clang-cl-20): the newest of those folders is searched
# after PATH. The rest of LLVM comes from the folder clang-cl really is in,
# so that the linker is as new as the compiler whose objects it reads.
file(GLOB _llvm_dirs /usr/lib/llvm-*/bin)
list(SORT _llvm_dirs COMPARE NATURAL ORDER DESCENDING)
find_program(_clang_cl NAMES clang-cl PATHS ${_llvm_dirs} NO_CACHE)
if(NOT _clang_cl)
  message(FATAL_ERROR
    "The windows-clang-cl preset needs clang-cl, lld-link and LLVM's tools, and "
    "clang-cl is not on PATH. rmp-docs, \"Windows, from Linux\", has the packages "
    "for each distribution.")
endif()
get_filename_component(_llvm_bin "${_clang_cl}" REALPATH)
get_filename_component(_llvm_bin "${_llvm_bin}" DIRECTORY)
foreach(_tool IN ITEMS lld-link llvm-lib llvm-rc llvm-mt)
  find_program(_${_tool} NAMES ${_tool} HINTS "${_llvm_bin}" NO_CACHE)
  if(NOT _${_tool})
    message(FATAL_ERROR
      "The windows-clang-cl preset found clang-cl (${_clang_cl}) but not ${_tool}, "
      "which comes with LLVM. rmp-docs, \"Windows, from Linux\", has the packages.")
  endif()
endforeach()
set(CMAKE_C_COMPILER "${_clang_cl}")
set(CMAKE_CXX_COMPILER "${_clang_cl}")
set(CMAKE_LINKER "${_lld-link}")
set(CMAKE_AR "${_llvm-lib}")
set(CMAKE_RC_COMPILER "${_llvm-rc}")
set(CMAKE_MT "${_llvm-mt}")

# For x86-64 even on an ARM or RISC-V Linux, where clang-cl would otherwise
# build for the machine it runs on.
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# Where xwin put Microsoft's files, kept in the cache so that a later configure
# finds them without XWIN_DIR in its environment, and handed to CMake's test
# builds, which read this file again with a cache of their own.
if(NOT DEFINED XWIN_DIR)
  if(DEFINED ENV{XWIN_DIR})
    set(XWIN_DIR "$ENV{XWIN_DIR}")
  else()
    set(XWIN_DIR "$ENV{HOME}/.xwin")
  endif()
endif()
set(XWIN_DIR "${XWIN_DIR}" CACHE PATH "Where xwin splat put the MSVC runtime and the Windows SDK")
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_DIR)

# lowercase windows.h is the link xwin adds next to Windows.h: raylib asks for
# it, and Linux's files, unlike Windows', have case.
if(NOT EXISTS "${XWIN_DIR}/crt/include/yvals_core.h" OR NOT EXISTS "${XWIN_DIR}/sdk/include/um/windows.h"
    OR NOT IS_DIRECTORY "${XWIN_DIR}/sdk/lib/um/x86_64")
  message(FATAL_ERROR
    "The windows-clang-cl preset needs Microsoft's C runtime and Windows SDK, as "
    "`xwin splat --include-debug-libs` lays them out, and \"${XWIN_DIR}\" does not "
    "have them. rmp-docs, \"Windows, from Linux\", says how to get them; XWIN_DIR "
    "says where they are, if not in ~/.xwin.")
endif()

# The debug C runtime too, though the preset builds a release: CMake's own test
# programs are debug builds — the one that checks the compiler works, and the
# one that checks link-time optimization does — and xwin leaves it out unless
# asked.
if(NOT EXISTS "${XWIN_DIR}/crt/lib/x86_64/libcmtd.lib")
  message(FATAL_ERROR
    "\"${XWIN_DIR}\" has no debug C runtime, which CMake's test programs link "
    "even for a release build: run `xwin --accept-license splat --include-debug-libs` "
    "again (rmp-docs, \"Windows, from Linux\").")
endif()

# Each Visual Studio's C++ library wants a clang-cl at least as new as the one
# that Visual Studio ships, and says which in yvals_core.h. A clang-cl that is
# older stops at the first standard header your C++ includes, with "error
# STL1000: Unexpected compiler version" — so it stops here instead, at once.
file(STRINGS "${XWIN_DIR}/crt/include/yvals_core.h" _stl REGEX "expected Clang [0-9]+")
execute_process(COMMAND "${_clang_cl}" --version OUTPUT_VARIABLE _clang_version ERROR_QUIET)
if(_stl MATCHES "expected Clang ([0-9]+)")
  set(_stl_clang "${CMAKE_MATCH_1}")
  if(_clang_version MATCHES "clang version ([0-9]+)" AND CMAKE_MATCH_1 LESS _stl_clang)
    message(FATAL_ERROR
      "${_clang_cl} is Clang ${CMAKE_MATCH_1}, and the C++ library in ${XWIN_DIR} "
      "takes Clang ${_stl_clang} or newer. Install a newer clang-cl, or have xwin "
      "fetch an older library: rmp-docs, \"Windows, from Linux\".")
  endif()
endif()

# Microsoft's headers and libraries, which clang-cl and lld-link would find on
# their own on Windows. /imsvc makes them system headers, as they are there:
# your warnings stay about your code. Built up apart and then set, because
# CMake can read this file more than once in one configure.
set(_includes "")
set(_rc_includes "")
foreach(_dir IN ITEMS crt/include sdk/include/ucrt sdk/include/um sdk/include/shared)
  string(APPEND _includes " /imsvc \"${XWIN_DIR}/${_dir}\"")
  string(APPEND _rc_includes " -I \"${XWIN_DIR}/${_dir}\"")
endforeach()
set(_libpaths "")
foreach(_dir IN ITEMS crt/lib/x86_64 sdk/lib/ucrt/x86_64 sdk/lib/um/x86_64)
  string(APPEND _libpaths " /libpath:\"${XWIN_DIR}/${_dir}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_includes}")
set(CMAKE_CXX_FLAGS_INIT "${_includes}")
set(CMAKE_RC_FLAGS_INIT "${_rc_includes}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_libpaths}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_libpaths}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_libpaths}")

# Libraries and headers are looked for among Microsoft's files only, never in
# Linux's /usr/lib, where anything found would be for the wrong system.
# Programs, ccache among them, are Linux's.
set(CMAKE_FIND_ROOT_PATH "${XWIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
