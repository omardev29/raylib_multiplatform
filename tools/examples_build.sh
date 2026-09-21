#!/usr/bin/env bash
# Build every example for real, and BOOT each one under raylib's software
# renderer with the same gate the game passes on every push.
#
# WHY. For months the examples job ran `g++ -fsyntax-only` and said `ok`. It
# could not see that four of the six games did not work: a ball that never
# moved, a formation that never marched, a player nothing could hurt. A file
# that parses is not a game that plays, and the only way to know is to run it.
# PLATFORM=Memory is what makes that possible with no GPU, no window and no
# display: the same pixels on every operating system, so a PNG of every
# example comes out of the run and can be looked at.
#
# One implementation, called from `just test examples` and from the CI job, so
# the two cannot drift -- the render check was consolidated into
# tools/render_check.sh for the same reason, after its two copies did.
#
# What it asserts, in order:
#   1. tools/examples_check.sh: every example has the SHAPE of an entry point.
#   2. Every example directory with a src/main.c* became a CMake target, and the
#      count of targets equals the count of directories -- a glob that stops
#      matching looks exactly like a folder where everything passes.
#   3. Every example that has an entry point boots, draws pixels and exits
#      within its frame budget (RAY_TEST_BOOT_OK with assets_failed=0,
#      RAY_TEST_RENDER_OK, RAY_TEST_DONE_FRAMES), and wrote its screenshot.
#
# What is NOT run: examples/plain_c, which is the opt-out and has no frame
# budget (it would loop forever under a platform with no window to close), and
# the header-only examples, which are compiled and have nothing to run.
#
# Usage: tools/examples_build.sh                 (from the repo root)
#        RMP_EXAMPLE_FRAMES=60 tools/examples_build.sh
#
# ASCII only and no GNU-only constructs -- tools/portable_check.sh covers this
# directory, and macOS ships bash 3.2.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD="build/examples-check"
SHOTS="$BUILD/screenshots"
FRAMES="${RMP_EXAMPLE_FRAMES:-30}"

# macOS has no `timeout`; there the run simply has no ceiling.
LIMIT=""
if command -v timeout >/dev/null 2>&1; then LIMIT="timeout 180"; fi

echo "== examples: shape =="
bash tools/examples_check.sh

echo "== examples: build =="
# The directories that ARE examples: one src/main.cpp or src/main.c each.
mains=$(find examples -path '*/src/main.cpp' -o -path '*/src/main.c' | sort)
expected=$(printf '%s\n' "$mains" | grep -c . || true)
# Plus the header-only ones: a src/ with .cpp files and no main.
headers=0
for d in $(find examples -type d -name src | sort); do
  if [ ! -f "$d/main.cpp" ] && [ ! -f "$d/main.c" ]; then
    if find "$d" -name '*.cpp' | grep -q .; then headers=$((headers + 1)); fi
  fi
done
echo "  $expected examples with an entry point, $headers header-only"

# A fresh checkout has no build/ at all, and the configure log below lives next
# to the build directory rather than inside it. CI found this on the first run.
mkdir -p "$BUILD"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPRODUCTION_BUILD=OFF \
      -DPLATFORM=Memory -DRMP_BUILD_EXAMPLES=ON > "$BUILD.configure.log" 2>&1 \
  || { cat "$BUILD.configure.log"; echo "FALLA: the examples did not configure"; exit 1; }
cmake --build "$BUILD" > "$BUILD.build.log" 2>&1 \
  || { tail -60 "$BUILD.build.log"; echo "FALLA: an example did not build"; exit 1; }

targets=$(cat "$BUILD/example_targets.txt")
made=$(printf '%s\n' "$targets" | grep -c . || true)
if [ "$made" -ne $((expected + headers)) ]; then
  echo "FALLA: $made example targets were configured for $expected + $headers example directories."
  echo "       The glob in CMakeLists.txt (RMP_BUILD_EXAMPLES) no longer matches the tree."
  exit 1
fi
echo "  ok    $made targets built"

echo "== examples: boot =="
mkdir -p "$SHOTS"
ran=0
failed=0
for t in $targets; do
  exe="$BUILD/$t"
  if [ ! -f "$exe" ]; then
    echo "  ok    $t  (header-only: compiled, nothing to run)"
    continue
  fi
  if [ "$t" = "example_plain_c" ]; then
    echo "  ok    $t  (the opt-out: built, not booted -- it has no frame budget)"
    continue
  fi
  shot="$SHOTS/$t.png"
  rm -f "$shot"
  out=$(RAY_TEST_MAX_FRAMES="$FRAMES" RAY_TEST_SCREENSHOT="$shot" $LIMIT "$exe" 2>&1) && status=0 || status=$?
  ran=$((ran + 1))
  ok=1
  echo "$out" | grep -q "RAY_TEST_BOOT_OK assets_failed=0 " || ok=0
  echo "$out" | grep -q "RAY_TEST_RENDER_OK" || ok=0
  echo "$out" | grep -q "RAY_TEST_DONE_FRAMES" || ok=0
  [ -s "$shot" ] || ok=0
  if [ "$ok" -eq 1 ] && [ "$status" -eq 0 ]; then
    echo "  ok    $t  $(echo "$out" | grep -o 'pixels=[0-9]* ratio=[0-9.]*' | head -1)"
  else
    failed=$((failed + 1))
    echo "  FAIL  $t  (exit $status)"
    echo "$out" | tail -25 | sed 's/^/          /'
  fi
done

# The opt-out and the header-only ones are the only ones not booted.
if [ "$ran" -ne $((expected - 1)) ]; then
  echo "FALLA: booted $ran examples, expected $((expected - 1)). Something was skipped."
  exit 1
fi
if [ "$failed" -ne 0 ]; then
  echo "FALLA: $failed example(s) did not boot, draw and exit. Screenshots of the ones that did: $SHOTS/"
  exit 1
fi
echo "PASS: $ran examples booted, drew and exited; screenshots in $SHOTS/"
