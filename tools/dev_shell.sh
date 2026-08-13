#!/usr/bin/env bash
# Run a command inside the pinned build image.
#
# Exists so that anyone — a contributor reproducing a CI failure, or the autofix
# agent — gets the exact toolchain CI uses without having to reconstruct a
# `docker run` invocation from memory and get the mounts subtly wrong.
#
#   tools/dev_shell.sh cmake --preset release
#   tools/dev_shell.sh bash -lc 'cd raymob && ./gradlew assembleDebug --no-daemon'
#   tools/dev_shell.sh                       # interactive shell
#
# macOS has no Docker on GitHub runners, so the Apple jobs — and the agent when
# it is fixing them — build natively instead. This is for the Linux, Web and
# Android families.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# One source of truth, the same one CI reads.
IMAGE_DIGEST=$(sed -nE 's/^build_image_digest[[:space:]]+(.*)$/\1/p' \
                 "$ROOT/thirdparty/FROZEN_VERSIONS.md" | head -1)
if [ -z "$IMAGE_DIGEST" ]; then
    echo "dev_shell: no build_image_digest in thirdparty/FROZEN_VERSIONS.md" >&2
    exit 1
fi
IMAGE="ghcr.io/omardev29/raylib-build@${IMAGE_DIGEST}"

if ! command -v docker >/dev/null 2>&1; then
    echo "dev_shell: docker is not installed." >&2
    echo "  On macOS this is expected — the Apple targets build natively." >&2
    exit 1
fi

# --user keeps generated files owned by you rather than by root, which
# otherwise leaves a working tree you cannot clean without sudo.
exec docker run --rm -it \
    --user "$(id -u):$(id -g)" \
    -v "$ROOT:/work" \
    -w /work \
    -e HOME=/tmp \
    "$IMAGE" \
    "${@:-bash}"
