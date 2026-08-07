# CMake toolchain: cross-compile for riscv64 Linux from an amd64 host.
# Used by the CI riscv64 job (inside the build image, which ships the
# riscv64-linux-gnu compilers and riscv64 X11/GL dev libraries via multiarch).
#
#   cmake --preset release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-riscv64-linux.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR riscv64)

set(CMAKE_C_COMPILER   riscv64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER riscv64-linux-gnu-g++)

# Search headers/libs in the target multiarch dirs, programs on the host.
set(CMAKE_FIND_ROOT_PATH /usr /usr/riscv64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Help FindX11/FindOpenGL locate the riscv64 multiarch libraries.
set(CMAKE_LIBRARY_ARCHITECTURE riscv64-linux-gnu)
list(APPEND CMAKE_PREFIX_PATH /usr/lib/riscv64-linux-gnu)
