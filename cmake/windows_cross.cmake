# The windows preset's toolchain file: the Windows build, made on Linux with the
# MinGW-w64 cross compiler (`rmp build windows`; rmp-docs, "Windows, from Linux").
#
# CMake reads a toolchain file first, before project(), when it builds for a
# system other than the one it runs on: the file says what that system is and
# which compiler builds for it. From there on the build sees WIN32 and MINGW,
# just as a MinGW build on Windows itself does, and takes the same paths:
# WINDOWS_BACKEND, -static, no console window, resources/ next to the .exe.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# GCC built to make Windows programs, named after the system it targets. Debian
# and Ubuntu package it once per thread model: -win32, their default, and
# -posix, taken here when it is there, because GCC has std::thread on -win32
# only from version 13, and Debian 12 ships 12. Other distributions have just
# the one, with no suffix.
find_program(_mingw_cc NAMES x86_64-w64-mingw32-gcc-posix x86_64-w64-mingw32-gcc NO_CACHE)
find_program(_mingw_cxx NAMES x86_64-w64-mingw32-g++-posix x86_64-w64-mingw32-g++ NO_CACHE)
find_program(_mingw_rc NAMES x86_64-w64-mingw32-windres NO_CACHE)
if(NOT _mingw_cc OR NOT _mingw_cxx)
  message(FATAL_ERROR
    "The windows preset needs the MinGW-w64 cross compiler, x86_64-w64-mingw32-g++, "
    "and it is not on PATH. rmp-docs, \"Windows, from Linux\", has the package for "
    "each distribution; the build image has it.")
endif()
set(CMAKE_C_COMPILER "${_mingw_cc}")
set(CMAKE_CXX_COMPILER "${_mingw_cxx}")
if(_mingw_rc)
  set(CMAKE_RC_COMPILER "${_mingw_rc}")
endif()

# Libraries and headers come from MinGW-w64's own tree, never from Linux's
# /usr/lib: a Linux library found by mistake would only fail later, at the link.
# The first path is where Debian, Ubuntu, Arch and Alpine keep that tree, the
# second where Fedora and openSUSE do. Programs, ccache among them, are Linux's.
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 /usr/x86_64-w64-mingw32/sys-root/mingw)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
