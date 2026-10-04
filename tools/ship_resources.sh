#!/bin/sh
# What goes in a release archive's resources/: the pack, or the loose files.
#
# A game built on the framework ships resources/resources.rres and nothing
# else: the framework's loader reads it, and plain raylib calls reach it through
# the loader hook. A game in plain C -- examples/plain_c, whose recipe deletes
# src/rmp/ -- has neither, and raw raylib cannot read the pack: shipping it
# alone, as every release job did, shipped a game that found none of its files.
# So without src/rmp/ the archive gets the loose files, and a PACK_SKIPPED.txt
# that says why, the way the cross-compiled targets' archives do.
#
# One file, called by every POSIX packaging step (_linux.yml, _apple.yml,
# _bsd.yml) and by tools/shipped_check.sh; _windows.yml says the same in
# PowerShell. Runnable here:
#
#     sh tools/ship_resources.sh package
#
# Usage: ship_resources.sh <package-dir>

set -e

DEST="${1:?usage: ship_resources.sh <package-dir>}"
mkdir -p "$DEST/resources"

if [ -d src/rmp ]; then
    test -f resources/resources.rres || {
        echo "FAIL: resources/resources.rres is not there. The pack_resources target"
        echo "      makes it, and a release of a framework game ships it alone."
        exit 1; }
    cp resources/resources.rres "$DEST/resources/"
    echo "  ok    $DEST/resources/resources.rres"
    exit 0
fi

# Everything in resources/ but a pack, which this game cannot read.
cp -R resources/. "$DEST/resources/"
rm -f "$DEST/resources/resources.rres"
printf '%s\n' \
    "resources.rres is not in this archive, on purpose." \
    "This game is written in plain C, without the framework's src/rmp/, and raw" \
    "raylib cannot read the pack: it reads these files, next to the executable." \
    "See the plain C example in the raylib_multiplatform framework." \
    > "$DEST/resources/PACK_SKIPPED.txt"
echo "  ok    $DEST/resources/ (loose files: a plain C game cannot read the pack)"
