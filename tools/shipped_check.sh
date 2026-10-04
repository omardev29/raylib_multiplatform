#!/bin/sh
# A release, laid out the way the archive ships it, started from SOMEWHERE ELSE.
#
# A production build reads "./resources/", and "." is the working directory
# unless the entry point moves it. Every CI boot used to start the binary from
# the repository root, where resources/ happens to be, so a release that loaded
# nothing when started from a file manager, a shortcut or another terminal
# folder passed every gate there was. This one builds the production binary
# under raylib's software renderer (no display, so it runs in the macOS job and
# in the BSD VMs too), puts it next to resources/resources.rres exactly as the
# Package steps do, and starts it from a folder with no resources/ in it.
#
#     sh tools/shipped_check.sh Ninja "" ray_test
#
# NetBSD and OpenBSD are the exception, and the check says so rather than
# skipping: raylib's GetApplicationDirectory() has no answer there, so a release
# reads resources/ from the working directory and logs that it does. There the
# check starts it from its own folder and asserts that warning instead.
#
# Usage: shipped_check.sh <generator> <extra-cmake-args> <project-name>

set -e

GENERATOR="${1:?usage: shipped_check.sh <generator> <extra-cmake> <name>}"
EXTRA="$2"
NAME="${3:?usage: shipped_check.sh <generator> <extra-cmake> <name>}"
ROOT=$(pwd)
BUILD=build/memory-release

echo "== a release, started from another folder (PLATFORM=Memory) =="

rm -rf "$BUILD"
# shellcheck disable=SC2086
cmake -B "$BUILD" -G "$GENERATOR" -DCMAKE_BUILD_TYPE=Release \
      -DPRODUCTION_BUILD=ON -DPLATFORM=Memory $EXTRA
cmake --build "$BUILD" --target "$NAME"
test -x "$BUILD/$NAME" || { echo "FAIL: $BUILD/$NAME was not built"; exit 1; }

# The pack the Package steps ship. CI has already made it; on a laptop it is
# made here and taken away again, so that the development build does not start
# reading a pack it was never asked to.
MADE_PACK=0
if [ ! -f resources/resources.rres ]; then
    cmake --build "$BUILD" --target pack_resources
    MADE_PACK=1
fi
SHIP="$BUILD/shipped"
mkdir -p "$SHIP/resources" "$BUILD/elsewhere"
cp "$BUILD/$NAME" "$SHIP/"
cp resources/resources.rres "$SHIP/resources/"
if [ "$MADE_PACK" -eq 1 ]; then rm -f resources/resources.rres; fi

LOG="$ROOT/$BUILD/shipped.log"
case "$(uname -s)" in
    NetBSD | OpenBSD)
        cd "$SHIP"
        RAY_TEST_MAX_FRAMES=5 "./$NAME" > "$LOG" 2>&1 || true
        cd "$ROOT"
        tail -15 "$LOG"
        grep -q "this system does not say where the executable is" "$LOG" || {
            echo "FAIL: $(uname -s) cannot say where an executable is, and the release did"
            echo "      not say it is reading resources/ from the working directory"; exit 1; }
        grep -q "RAY_TEST_BOOT_OK assets_failed=0 " "$LOG" || {
            echo "FAIL: started from its own folder, the release failed to load an asset"; exit 1; }
        echo "PASS: $(uname -s) reads resources/ from the working directory, and says so"
        exit 0
        ;;
esac

cd "$BUILD/elsewhere"
RAY_TEST_MAX_FRAMES=5 "$ROOT/$SHIP/$NAME" > "$LOG" 2>&1 || true
cd "$ROOT"
tail -15 "$LOG"
grep -q "RAY_TEST_BOOT_OK assets_failed=0 " "$LOG" || {
    echo "FAIL: started from another folder, the release did not boot or did not find"
    echo "      its resources/ -- it has to read the one next to the executable"; exit 1; }
grep -q "RAY_TEST_DONE_FRAMES" "$LOG" || {
    echo "FAIL: it died before the end of its frame budget"; exit 1; }
echo "PASS: the release found its resources/ from another folder"
