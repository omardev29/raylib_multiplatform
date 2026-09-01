#!/usr/bin/env bash
# Build the Linux target so that it runs on distributions older than this one.
#
# THE PROBLEM. A Linux binary records, per imported symbol, the glibc version
# that symbol came from, and the loader refuses anything it cannot satisfy.
# Building on ubuntu-24.04 means glibc 2.39, and 2.39 does not exist on Debian
# 12, Ubuntu 22.04, RHEL 8, or inside Steam's sniper runtime. The build gives no
# hint: it compiles, links, and runs perfectly on the machine that made it.
#
# WHAT IT COSTS TO FIX, measured rather than assumed: nothing. Every symbol in
# this project that asks for a modern glibc is either libm (powf, hypot, fmod,
# acosf) or one of the pthread/dl functions glibc 2.34 folded into libc. There
# is not one new API anywhere, so the floor is a build setting.
#
# zig cc is the compiler because it ships glibc stub libraries for every version
# back to 2.17 and picks one with a flag. Verified on this project: at the same
# optimisation level it produces a byte-identical render to gcc — the software
# renderer hashes the same — so this is a compatibility change and not a
# behaviour change.
#
# WITH [linux] glibc EMPTY this does nothing but the ordinary build.
#
# Usage: tools/linux_build.sh <build-dir> [extra cmake args...]

set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_DIR="${1:?usage: linux_build.sh <build-dir> [cmake args...]}"
shift || true

# RMP_LIBC=musl builds the Alpine target. The default is glibc, which is what
# every other Linux distribution uses.
#
# WHY MUSL IS ITS OWN BINARY AND NOT A FLAG: musl and glibc are different C
# libraries, not versions of one. A glibc binary cannot run on Alpine and a musl
# binary cannot run on Debian, so this is a second artifact -- linux-x64-musl --
# and not a setting on the first.
LIBC="${RMP_LIBC:-glibc}"
GLIBC=$(python3 tools/configure.py --print-glibc)

if [ "$LIBC" = "musl" ]; then
  GLIBC=""            # musl has no version to target; it is one ABI
elif [ -z "$GLIBC" ]; then
  echo "  [linux] glibc is empty — building against this machine's glibc"
  cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPRODUCTION_BUILD=ON "$@"
  cmake --build "$BUILD_DIR"
  exit 0
fi

case "$(uname -m)" in
  x86_64|amd64)  ZIG_ARCH=x86_64;  TRIPLE_ARCH=x86_64 ;;
  aarch64|arm64) ZIG_ARCH=aarch64; TRIPLE_ARCH=aarch64 ;;
  *) echo "FALLA: no pinned zig for $(uname -m). Set [linux] glibc = \"\" to build here."
     exit 1 ;;
esac

VERSION=$(awk '/^zig /{print $2}' thirdparty/FROZEN_VERSIONS.md)
SHA=$(awk -v k="zig_sha256_${ZIG_ARCH}" '$1==k{print $2}' thirdparty/FROZEN_VERSIONS.md)
[ -n "$VERSION" ] && [ -n "$SHA" ] || {
  echo "FALLA: no zig pin for $ZIG_ARCH in thirdparty/FROZEN_VERSIONS.md"; exit 1; }

# THE IMAGE'S COPY FIRST. CI runs inside a container that already has zig at
# the pinned version, with its libc++ cache warm -- see CLAUDE.md: a Linux job
# downloads nothing, because a download that fails on a bad day takes twenty
# minutes of matrix with it. The download below is the laptop fallback.
ZIG_BIN=""
if command -v zig > /dev/null 2>&1 && [ "$(zig version)" = "$VERSION" ]; then
  ZIG_BIN=$(command -v zig)
  ZIG_HOME=$(dirname "$ZIG_BIN")
  echo "  using the zig already here: $ZIG_BIN ($VERSION)"
fi

ZIG_HOME="${ZIG_HOME:-$PWD/.zig-$VERSION-$ZIG_ARCH}"
NAME="zig-${ZIG_ARCH}-linux-${VERSION}"
if [ -z "$ZIG_BIN" ] && [ ! -x "$ZIG_HOME/zig" ]; then
  TMP=$(mktemp -d)
  trap 'rm -rf "$TMP"' EXIT
  echo "  downloading zig ${VERSION} (${ZIG_ARCH})"
  curl -fsSL -o "$TMP/zig.tar.xz" \
    "https://ziglang.org/download/${VERSION}/${NAME}.tar.xz"
  echo "${SHA}  ${TMP}/zig.tar.xz" | sha256sum -c -
  tar -xJf "$TMP/zig.tar.xz" -C "$TMP"
  rm -rf "$ZIG_HOME"
  mv "$TMP/$NAME" "$ZIG_HOME"
fi

if [ "$LIBC" = "musl" ]; then
  # No version suffix: musl is one ABI, not a series of them. The binary comes
  # out DYNAMIC because it links against the system's X11 and GL, which is what
  # we want -- a static musl binary cannot dlopen, and dlopen is exactly how
  # GLFW reaches the window system.
  TARGET="${TRIPLE_ARCH}-linux-musl"
else
  TARGET="${TRIPLE_ARCH}-linux-gnu.${GLIBC}"
fi
echo "  building for ${TARGET}"

# CMake wants one executable, not "zig cc", so the target is baked into two
# wrapper scripts rather than passed through the environment — a build that
# forgets to export a variable would silently produce a host binary again.
ZIG_BIN="${ZIG_BIN:-$ZIG_HOME/zig}"
# The wrappers go somewhere writable: $ZIG_HOME is /opt inside the image.
WRAP="${PWD}/.zig-wrappers"
mkdir -p "$WRAP"
# -Wno-nullability-completeness for OUR translation units: zig's libc++ headers
# are not annotated for nullability and clang says so once per declaration.
#
# It does NOT silence the thousands of the same warning you will see the first
# time this runs, and that is worth knowing rather than being surprised by:
# those come from zig compiling its own bundled libc++ for this target
# (extern_template_lists.h, stdexcept.cpp and friends), which happens inside
# zig with zig's own flags. Nothing on our command line reaches them. They are
# noise, they are not about this project, and they cost a log page per run.
#
# -idirafter /usr/include is the other half, and it is load-bearing. CMake's
# find_package(X11) reports /usr/include, and since that is not an implicit
# directory for zig cc, CMake puts it on the command line as -I -- BEFORE zig's
# own libc headers. The musl build then compiled <math.h> from glibc and died on
# a header glibc keeps in a multiarch directory:
#
#   /usr/include/math.h:27: fatal error: 'bits/libc-header-start.h' file not found
#
# The glibc build had the same shadowing and merely got away with it, which is
# worse: it was compiling against the host's 2.39 headers while linking zig's
# 2.28 stubs. -idirafter puts /usr/include LAST, so X11/Xlib.h is still found
# and math.h comes from the libc actually being targeted.
ZIG_QUIET="-Wno-nullability-completeness -idirafter /usr/include"
# The suppression goes LAST, after the caller's own flags: CMake appends its
# warning options to the command line, and a -Wno- that comes before them is
# turned back on by whatever follows.
printf '#!/bin/sh\nexec "%s" cc -target %s "$@" %s\n'  "$ZIG_BIN" "$TARGET" "$ZIG_QUIET" > "$WRAP/cc"
printf '#!/bin/sh\nexec "%s" c++ -target %s "$@" %s\n' "$ZIG_BIN" "$TARGET" "$ZIG_QUIET" > "$WRAP/c++"
# And the archiver. CMake derives CMAKE_<LANG>_COMPILER_AR from the compiler --
# gcc-ar for gcc, llvm-ar for clang -- and for zig cc it derives nothing, so the
# static libraries were archived by a program called
# "CMAKE_C_COMPILER_AR-NOTFOUND". It only showed up inside the build image; on a
# desktop with llvm-ar installed CMake had found one by luck.
printf '#!/bin/sh\nexec "%s" ar "$@"\n'     "$ZIG_BIN" > "$WRAP/ar"
printf '#!/bin/sh\nexec "%s" ranlib "$@"\n' "$ZIG_BIN" > "$WRAP/ranlib"
chmod +x "$WRAP/cc" "$WRAP/c++" "$WRAP/ar" "$WRAP/ranlib"

# WHERE THE SYSTEM LIBRARIES LIVE. CMake normally learns the multiarch directory
# -- /usr/lib/x86_64-linux-gnu on Debian and Ubuntu -- by asking the compiler for
# its implicit link paths. zig cc does not answer that question the way gcc
# does, so find_package(X11) came back with "Could NOT find X11 (missing:
# X11_X11_LIB)" on a machine where libX11 was plainly installed.
#
# Only set when the directory is really there, so this stays correct on
# distributions that put everything in /usr/lib (Arch, Fedora) rather than
# adding a search path that does not exist.
#
# X11 is needed at CONFIGURE time and not at run time: GLFW opens libX11 with
# dlopen, which is why the finished binary has no NEEDED entry for it.
MULTIARCH_ARGS=()
if [ -d "/usr/lib/${TRIPLE_ARCH}-linux-gnu" ]; then
  MULTIARCH_ARGS=(-DCMAKE_LIBRARY_ARCHITECTURE="${TRIPLE_ARCH}-linux-gnu")
  echo "  system libraries: /usr/lib/${TRIPLE_ARCH}-linux-gnu"
fi

# CMAKE_LINK_DEPENDS_USE_LINKER=OFF is load-bearing. With Ninja, CMake passes
# `-Xlinker --dependency-file=...` to track link inputs, and zig cc SEGFAULTS on
# it -- no diagnostic, exit 139, on every link in the project. It took isolating
# one link command by hand to find, because the failure names nothing.
cmake -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DPRODUCTION_BUILD=ON \
      -DCMAKE_C_COMPILER="$WRAP/cc" \
      -DCMAKE_CXX_COMPILER="$WRAP/c++" \
      -DCMAKE_AR="$WRAP/ar" \
      -DCMAKE_RANLIB="$WRAP/ranlib" \
      -DCMAKE_C_COMPILER_AR="$WRAP/ar" \
      -DCMAKE_CXX_COMPILER_AR="$WRAP/ar" \
      -DCMAKE_C_COMPILER_RANLIB="$WRAP/ranlib" \
      -DCMAKE_CXX_COMPILER_RANLIB="$WRAP/ranlib" \
      -DCMAKE_LINK_DEPENDS_USE_LINKER=OFF \
      -DCMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES=/usr/include \
      -DCMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES=/usr/include \
      "${MULTIARCH_ARGS[@]}" \
      "$@"
cmake --build "$BUILD_DIR"
