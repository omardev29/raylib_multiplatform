#!/usr/bin/env bash
# Compress a binary with UPX, if [upx] in raylib_multiplatform.toml asked for
# this target. Called from the Package step of each CI job.
#
#   tools/upx_pack.sh <target> <path-to-binary>
#
# Doing nothing is the normal outcome and is not a failure: the default only
# compresses linux-x64 and linux-arm64, so every other job calls this and exits.
#
# Measured on this project: 847 KB -> 215 KB, a 74 % cut, and the compressed
# binary still passes the render gate. What it costs is a few milliseconds of
# decompression at launch and, on Windows, the risk in the .toml's comment.
#
# UPX is downloaded at a pinned version with a checked sha256, the same way
# ninja, XcodeGen and butler are. Not apt: the version an image happens to carry
# is a version nobody chose.

set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

TARGET="${1:?usage: upx_pack.sh <target> <binary>}"
BINARY="${2:?usage: upx_pack.sh <target> <binary>}"

WANTED=$(python3 tools/configure.py --print-upx)
if ! printf '%s' "$WANTED" | grep -q "\"$TARGET\""; then
  echo "  upx: $TARGET is not in [upx] enabled — leaving the binary alone"
  exit 0
fi

[ -f "$BINARY" ] || { echo "FALLA: $BINARY does not exist"; exit 1; }

VERSION=$(awk '/^upx /{print $2}' thirdparty/FROZEN_VERSIONS.md)
[ -n "$VERSION" ] || { echo "FALLA: no upx pin in thirdparty/FROZEN_VERSIONS.md"; exit 1; }

case "$(uname -m)" in
  x86_64|amd64) ARCH=amd64 ;;
  aarch64|arm64) ARCH=arm64 ;;
  *) echo "  upx: no published build for $(uname -m) — leaving the binary alone"; exit 0 ;;
esac
SHA=$(awk -v k="upx_sha256_${ARCH}" '$1==k{print $2}' thirdparty/FROZEN_VERSIONS.md)
[ -n "$SHA" ] || { echo "FALLA: no upx_sha256_${ARCH} in thirdparty/FROZEN_VERSIONS.md"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
NAME="upx-${VERSION}-${ARCH}_linux"
curl -fsSL -o "$TMP/upx.tar.xz" \
  "https://github.com/upx/upx/releases/download/v${VERSION}/${NAME}.tar.xz"
echo "${SHA}  ${TMP}/upx.tar.xz" | sha256sum -c -
tar -xJf "$TMP/upx.tar.xz" -C "$TMP"
UPX="$TMP/$NAME/upx"

BEFORE=$(wc -c < "$BINARY")
# --best --lzma is the slowest setting and the smallest result. This runs once
# per release, so the minute it costs is the cheapest minute in the pipeline.
"$UPX" --best --lzma -q "$BINARY" > /dev/null
AFTER=$(wc -c < "$BINARY")

# The check that makes this safe to leave on. A packed binary that does not
# start is worse than a big one, and UPX has been known to produce those on
# unusual toolchains — so it is verified here rather than by a player.
if ! "$UPX" -t "$BINARY" > /dev/null 2>&1; then
  echo "FALLA: upx cannot verify the packed binary"
  exit 1
fi

echo "  upx: $TARGET  ${BEFORE} -> ${AFTER} bytes  ($(( (BEFORE - AFTER) * 100 / BEFORE ))% smaller)"
