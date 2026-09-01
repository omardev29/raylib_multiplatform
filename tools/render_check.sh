#!/bin/sh
# Build with raylib's software renderer and prove the game runs.
#
# Used by the five BSD jobs, by the macOS job, and by anybody standing here.
#
# WHY THIS IS A FILE AND NOT LINES IN THE WORKFLOW. cross-platform-actions
# carries the step's script into the VM through its cpa.sh shell, and there is a
# LIMIT: the inline script grew from about 3.4 KB to 5.6 KB when this check was
# added inline, and it started being cut off. The first cut landed inside a
# quoted string, which came back as "Unterminated quoted string" on a line
# number nobody could place; the second landed somewhere syntactically complete,
# so the shell simply reached EOF and exited 0 -- a step that reported success
# having run half of itself.
#
# A file rsyncs in with the workspace and is invoked in one short line, so the
# inline script goes back to roughly the size that worked for months. It also
# means this is runnable HERE:
#
#     sh tools/render_check.sh Ninja "" ray_test
#
# which is the other half of why it is a file. A check that can only be
# exercised inside a FreeBSD VM in CI is a check you debug twenty minutes at a
# time.
#
# Usage: render_check.sh <generator> <extra-cmake-args> <project-name> [update]
#
# `update` records whatever the frame hashes to instead of comparing, which is
# what you run after changing the drawing on purpose. `just test render-update`.

set -e

GENERATOR="${1:?usage: render_check.sh <generator> <extra-cmake> <name> [update]}"
EXTRA="$2"
NAME="${3:?usage: render_check.sh <generator> <extra-cmake> <name> [update]}"
MODE="${4:-check}"
GOLDEN=tests/fixtures/render_hash.txt

echo "== software render (PLATFORM=Memory) =="

# PLATFORM=Memory is what makes running possible in a VM at all: raylib
# rasterises into a plain buffer, so there is no GPU, no window, no display
# server and nothing to install. It is a second build of raylib, and that is
# what it costs -- worth it for the difference between "it linked" and "it
# started, drew a frame and shut down cleanly".
rm -rf build/memory
# shellcheck disable=SC2086
cmake -B build/memory -G "$GENERATOR" -DCMAKE_BUILD_TYPE=Debug \
      -DPRODUCTION_BUILD=OFF -DPLATFORM=Memory $EXTRA
cmake --build build/memory

BIN="build/memory/$NAME"
# Inside the build tree, which is already ignored: written at the repository
# root it got committed once, and a log is not a source file.
LOG="build/memory/render.log"
test -x "$BIN" || { echo "FALLA: $BIN was not built"; exit 1; }

RAY_TEST_MAX_FRAMES=10 "./$BIN" > "$LOG" 2>&1 || true
tail -25 "$LOG"

# Three assertions, not one. The third is the one that would have caught the
# segfault-at-shutdown from phase 3: a process that dies on the way out never
# prints RAY_TEST_DONE_FRAMES, and until this existed nothing on these targets
# would have noticed.
grep -q "RAY_TEST_BOOT_OK" "$LOG" || { echo "FALLA: it did not boot"; exit 1; }
grep -q "RAY_TEST_RENDER_OK" "$LOG" || { echo "FALLA: it drew nothing"; exit 1; }
grep -q "RAY_TEST_DONE_FRAMES" "$LOG" || {
    echo "FALLA: it died before the end of its frame budget"; exit 1; }

# And the same pixels. Linux x86-64 and macos-26 (Apple Silicon) both produce
# this frame byte for byte, so one golden hash covers every operating system and
# both instruction sets -- there is no driver in between to differ.
GOT=$(grep -o "hash=[0-9a-f]*" "$LOG" | head -1 | cut -d= -f2)

if [ "$MODE" = "update" ]; then
    mkdir -p "$(dirname "$GOLDEN")"
    echo "$GOT" > "$GOLDEN"
    echo "  golden hash recorded: $GOT -- commit it, and say what changed visually"
    exit 0
fi

if [ ! -f "$GOLDEN" ]; then
    echo "  no golden hash yet. Record one with: just test render-update"
    exit 0
fi
WANT=$(cat "$GOLDEN")
echo "  software-render hash=$GOT want=$WANT"
test "$GOT" = "$WANT" || {
    echo "FALLA: the frame changed. If that was on purpose: just test render-update"
    exit 1; }

echo "PASS: booted, rendered and exited under software rendering"

# The proof that this ran to the end, for the caller to check. It is written
# HERE and not by the caller for a reason paid for twice: the workflow's inline
# script is truncated on its way into the VM, and OpenBSD's limit is tighter
# than FreeBSD's -- the two lines that used to follow the call to this script
# were inside the cut on OpenBSD and outside it on FreeBSD, so one BSD passed
# and the other did not, with identical code. With the marker written from in
# here there is nothing after the call to lose.
mkdir -p build
echo "reached-the-end" > build/.rmp-render-complete
