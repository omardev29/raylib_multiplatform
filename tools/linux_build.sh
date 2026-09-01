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

GLIBC=$(python3 tools/configure.py --print-glibc)

if [ -z "$GLIBC" ]; then
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

# Cached across steps of the same job, and re-downloaded if the pin moves.
ZIG_HOME="${ZIG_HOME:-$PWD/.zig-$VERSION-$ZIG_ARCH}"
NAME="zig-${ZIG_ARCH}-linux-${VERSION}"
if [ ! -x "$ZIG_HOME/zig" ]; then
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

TARGET="${TRIPLE_ARCH}-linux-gnu.${GLIBC}"
echo "  building for ${TARGET}"

# CMake wants one executable, not "zig cc", so the target is baked into two
# wrapper scripts rather than passed through the environment — a build that
# forgets to export a variable would silently produce a host binary again.
WRAP="$ZIG_HOME/wrappers"
mkdir -p "$WRAP"
printf '#!/bin/sh\nexec "%s/zig" cc -target %s "$@"\n'  "$ZIG_HOME" "$TARGET" > "$WRAP/cc"
printf '#!/bin/sh\nexec "%s/zig" c++ -target %s "$@"\n' "$ZIG_HOME" "$TARGET" > "$WRAP/c++"
chmod +x "$WRAP/cc" "$WRAP/c++"

# CMAKE_LINK_DEPENDS_USE_LINKER=OFF is load-bearing. With Ninja, CMake passes
# `-Xlinker --dependency-file=...` to track link inputs, and zig cc SEGFAULTS on
# it -- no diagnostic, exit 139, on every link in the project. It took isolating
# one link command by hand to find, because the failure names nothing.
cmake -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DPRODUCTION_BUILD=ON \
      -DCMAKE_C_COMPILER="$WRAP/cc" \
      -DCMAKE_CXX_COMPILER="$WRAP/c++" \
      -DCMAKE_LINK_DEPENDS_USE_LINKER=OFF \
      "$@"
cmake --build "$BUILD_DIR"
