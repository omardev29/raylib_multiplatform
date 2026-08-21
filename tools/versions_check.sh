#!/usr/bin/env bash
# Fail if the pinned versions have drifted apart.
#
# The problem this solves: a project can be perfectly pinned and still be
# lying, because the pins live in five files that nobody updates together —
# the Dockerfile in another repo, gradle-wrapper.properties, build.gradle, the
# workflow's image digest, and the docs. The docs rot first and quietly become
# fiction.
#
# So the docs are the source of truth here, and they are executable. The
# ```versions block in thirdparty/FROZEN_VERSIONS.md is parsed, and every value
# is compared against the file that actually controls it. Inside a container
# job it is ALSO compared against /etc/raylib-build-image.json — the manifest
# baked into the running build image — which is the check that catches the
# nastiest class of bug: Gradle asking for an SDK the image does not have.
#
# Usage:  tools/versions_check.sh          (from the repo root)
# Exit:   0 = consistent, 1 = drift (with a diff of what disagrees)

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FROZEN=thirdparty/FROZEN_VERSIONS.md
IMAGE_MANIFEST=/etc/raylib-build-image.json

fails=0
checks=0

# --- parse the ```versions block -------------------------------------------
declare -A WANT
while read -r key value; do
    [ -z "${key:-}" ] && continue
    case "$key" in \#*) continue ;; esac
    WANT[$key]="$value"
done < <(awk '/^```versions$/{f=1;next} /^```$/{f=0} f' "$FROZEN")

if [ ${#WANT[@]} -eq 0 ]; then
    echo "FAIL: no \`\`\`versions block found in $FROZEN"
    exit 1
fi

# check <label> <expected-key> <actual-value>
check() {
    local label="$1" key="$2" actual="$3"
    local want="${WANT[$key]:-<missing from FROZEN_VERSIONS.md>}"
    checks=$((checks + 1))
    if [ "$want" = "$actual" ]; then
        printf '  ok    %-26s %s\n' "$label" "$actual"
    else
        printf '  DRIFT %-26s declared=%s  actual=%s\n' "$label" "$want" "$actual"
        fails=$((fails + 1))
    fi
}

# Pull `key value` style settings out of a Gradle file, tolerating both
# `compileSdk 36` and `compileSdk = 36`, quoted or not.
gradle_val() {
    sed -nE "s/^[[:space:]]*$2[[:space:]]*=?[[:space:]]*'?\"?([^'\"[:space:]]+)'?\"?.*/\1/p" "$1" \
        | head -1
}

echo "== Android (raymob/) =="
APP=raymob/app/build.gradle
check "compileSdk"        android_compile_sdk  "$(gradle_val $APP compileSdk)"
check "targetSdk"         android_target_sdk   "$(gradle_val $APP targetSdk)"
# minSdk now comes from raylib_multiplatform.toml, so it is read from the
# generated properties rather than from build.gradle — where the line is now
# `Integer.parseInt(project.properties[...])` and the old regex would happily
# report the literal `project.properties[` as the version, forever.
GEN_PROPS=raymob/generated.properties
if [ -r "$GEN_PROPS" ]; then
    check "minSdk" android_min_sdk \
        "$(sed -nE 's/^app\.min_sdk=(.*)$/\1/p' "$GEN_PROPS" | head -1)"
else
    echo "  skip  minSdk                     $GEN_PROPS not generated yet (run tools/configure.py)"
fi
check "buildToolsVersion" android_build_tools  "$(gradle_val $APP buildToolsVersion)"
check "ndkVersion"        android_ndk          "$(gradle_val $APP ndkVersion)"
# The externalNativeBuild `version` line, not the CMakeLists path above it.
check "externalNativeBuild cmake" android_sdk_cmake \
    "$(awk '/externalNativeBuild/,/^    }/' $APP | sed -nE "s/^[[:space:]]*version[[:space:]]*'([^']+)'.*/\1/p" | head -1)"
check "AGP" agp \
    "$(sed -nE "s/.*com\.android\.application' version '([^']+)'.*/\1/p" raymob/build.gradle | head -1)"

echo "== Gradle wrapper =="
WRAP=raymob/gradle/wrapper/gradle-wrapper.properties
check "gradle"        gradle        "$(sed -nE 's#.*/gradle-([0-9.]+)-bin\.zip.*#\1#p' $WRAP | head -1)"
check "gradle sha256" gradle_sha256 "$(sed -nE 's/^distributionSha256Sum=(.*)$/\1/p' $WRAP | head -1)"

# The formatter and the linter, checked against the binary that is actually on
# PATH rather than against another file. This is the one check here that
# compares a pin to reality, and it is the one that matters most day to day: a
# clang-format one minor version out reformats files that were already correct
# and every diff becomes noise. Skipped where the tools are not installed —
# most jobs have no reason to.
echo "== clang tooling (if installed) =="
for tool in clang-format clang-tidy; do
    key="${tool//-/_}"
    if command -v "$tool" >/dev/null 2>&1; then
        check "$tool" "$key" \
            "$("$tool" --version | sed -nE 's/.*version ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' | head -1)"
    else
        printf '  skip  %-26s not installed here\n' "$tool"
    fi
done

echo "== Build image pin (workflows) =="
# canary.yml is excluded from both checks below, deliberately and by name. Its
# entire purpose is to run the pipeline against the FLOATING `:latest` image to
# find out what is about to break, so the two rules here — "exactly one digest"
# and "never :latest" — are exactly wrong for it. Without this exclusion the
# canary would fail the lint job on the day it was added.
WORKFLOWS=$(find .github/workflows -name '*.yml' ! -name 'canary.yml')

# Every container job must pin the same digest, and it must be the declared one.
# shellcheck disable=SC2086
mapfile -t DIGESTS < <(grep -hoE 'raylib-build@sha256:[0-9a-f]{64}' $WORKFLOWS \
                        | sed 's/.*@//' | sort -u)
if [ ${#DIGESTS[@]} -eq 0 ]; then
    echo "  DRIFT build image             no digest-pinned image found in .github/workflows/"
    fails=$((fails + 1)); checks=$((checks + 1))
elif [ ${#DIGESTS[@]} -gt 1 ]; then
    echo "  DRIFT build image             workflows disagree: ${DIGESTS[*]}"
    fails=$((fails + 1)); checks=$((checks + 1))
else
    check "build image digest" build_image_digest "${DIGESTS[0]}"
fi

# A `:latest` reference anywhere defeats the whole point — outside the canary.
# shellcheck disable=SC2086
if grep -qE 'raylib-build:latest' $WORKFLOWS; then
    echo "  DRIFT build image             a :latest reference is still present (use the digest)"
    fails=$((fails + 1)); checks=$((checks + 1))
fi

# --- the check that actually matters, when we can make it ------------------
if [ -r "$IMAGE_MANIFEST" ]; then
    echo "== Running build image ($IMAGE_MANIFEST) =="
    m() { sed -nE "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"([^\"]*)\".*/\1/p" "$IMAGE_MANIFEST" | head -1; }
    check "image android_platform"    android_platform    "$(m android_platform)"
    check "image android_build_tools" android_build_tools "$(m android_build_tools)"
    check "image android_ndk"         android_ndk         "$(m android_ndk)"
    check "image android_sdk_cmake"   android_sdk_cmake   "$(m android_sdk_cmake)"
    # compileSdk N needs platforms;android-N present in the image, or AGP will
    # silently try to download it at job time and the "frozen" claim is void.
    want_plat="android-${WANT[android_compile_sdk]}"
    checks=$((checks + 1))
    if [ "$(m android_platform)" = "$want_plat" ]; then
        printf '  ok    %-26s %s\n' "image serves compileSdk" "$want_plat"
    else
        printf '  DRIFT %-26s gradle wants %s, image has %s\n' \
               "image serves compileSdk" "$want_plat" "$(m android_platform)"
        fails=$((fails + 1))
    fi
else
    echo "== Running build image =="
    echo "  skip  $IMAGE_MANIFEST not present (not inside the build image)"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "PASS: $checks checks, everything agrees with $FROZEN"
    exit 0
fi
echo "FAIL: $fails of $checks checks drifted."
echo "Either fix the file that drifted, or update $FROZEN if the new value is intended."
exit 1
