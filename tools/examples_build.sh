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
# One implementation, called from `rmp test examples` and from the CI job, so
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
#      RAY_TEST_RENDER_OK, RAY_TEST_DONE_FRAMES), wrote its screenshot, and
#      made no mistake the framework reports (RAY_TEST_REPORTS=0) -- but the
#      plain C one, which has no framework to report anything.
#   4. scenes/02_stretch, booted again in a window of another shape than the
#      design's -- a phone held upright and a strip wider than 16:9 -- is
#      letterboxed: bars of its clear colour, the court centred between them.
#   5. The platformer is played to the end (tests/platformer_play.cpp), and
#      rmp::input is read both ways a game is written (tests/input_play.cpp).
#   6. Every example with a script in tests/e2e/ is PLAYED from it, frame by
#      frame, and every shot compared with its golden PNG: exactly, inside the
#      pinned build image; reported and not enforced anywhere else. As many
#      scripts as e2e targets as runs. See tests/e2e/driver.cpp.
#
# 1-5 are `boot` and 6 is `e2e`, and with no argument it is both, one after
# the other -- `rmp test examples`. CI runs the two halves in two jobs side by
# side (`examples` and `e2e` in ci.yml), each building what it runs: the e2e
# targets are out of the build's `all` (CMakeLists.txt, rmp_add_e2e), so the
# boot run neither builds nor plays them.
#
# raylib's renderer -- rcore.c, which carries the rasteriser, rshapes.c and
# rtext.c -- is compiled at -O2 -ffp-contract=off here, still under ASan and
# UBSan, and everything else at -O0 like every Debug build
# (RMP_RAYLIB_OPTIMIZED in CMakeLists.txt): a script spends most of its frames
# inside raylib's software renderer, which this way plays about five times
# faster and draws every e2e shot byte for byte the same, in the image.
# Nothing else that builds raylib for PLATFORM=Memory asks for it -- the
# render check's golden hash is drawn at -O0 on every platform that compares
# it.
#
# What is NOT run: the header-only examples, which are compiled and have
# nothing to run. examples/plain_c is booted like the rest: it is plain C with
# its own main(), and it calls the C smoke-test hooks every other example gets
# from the entry point -- it used to be skipped for having no frame budget,
# which meant the one file people copy to leave the framework was never run.
#
# Usage: tools/examples_build.sh                 (from the repo root) 1-6
#        RMP_EXAMPLE_FRAMES=60 tools/examples_build.sh
#        tools/examples_build.sh boot            1-5: shape, build, boot, the
#                                                 stretch, the platformer and
#                                                 the input
#        tools/examples_build.sh e2e             6. alone: builds the e2e
#                                                 targets and plays them
#        RMP_E2E_UPDATE=1 tools/examples_build.sh e2e
#                                                 records the goldens, inside
#                                                 the image only: `rmp test
#                                                 e2e-update`
#
# ASCII only and no GNU-only constructs -- tools/portable_check.sh covers this
# directory, and macOS ships bash 3.2.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD="build/examples-check"
SHOTS="$BUILD/screenshots"
FRAMES="${RMP_EXAMPLE_FRAMES:-30}"
ONLY="${1:-all}"
case "$ONLY" in
  all | boot | e2e) ;;
  *) echo "usage: tools/examples_build.sh [boot | e2e]"; exit 2 ;;
esac

# macOS has no `timeout`; there the run simply has no ceiling. A script plays
# far longer than a boot -- a whole game, won and lost, is minutes under ASan
# with raylib at -O0 -- so an e2e run has a ceiling of its own, and a stuck one
# still ends.
LIMIT=""
E2E_LIMIT=""
if command -v timeout >/dev/null 2>&1; then
  LIMIT="timeout 180"
  E2E_LIMIT="timeout 600"
fi

# What says this is the pinned build image; tools/versions_check.sh reads it
# too. The e2e goldens are compared exactly, so one compiler, one libm and one
# renderer draw them and check them: this image's.
IMAGE_MANIFEST=/etc/raylib-build-image.json
# Refused before the build, which takes minutes. The driver refuses it too,
# and it is the one that would write.
if [ "${RMP_E2E_UPDATE:-}" = "1" ] && [ ! -r "$IMAGE_MANIFEST" ]; then
  echo "FAIL: RMP_E2E_UPDATE records the e2e goldens, and only inside the pinned build image:"
  echo "      tools/dev_shell.sh bash -c 'python3 tools/configure.py && ./rmp test e2e-update'"
  exit 1
fi

configure_examples() {
  # A build directory configured from somewhere else cannot be configured
  # here: CMake refuses a cache made in another source directory. The goldens
  # are recorded inside the image, which sees this checkout at /work
  # (tools/dev_shell.sh), and played on the laptop, which sees it at its own
  # path -- so the folder of the other one is started again, and said so.
  local made_in
  if [ -f "$BUILD/CMakeCache.txt" ]; then
    made_in=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt")
    if [ -n "$made_in" ] && [ "$made_in" != "$(pwd -P)" ]; then
      echo "  $BUILD was configured from $made_in: starting it again from $(pwd -P)"
      rm -rf "$BUILD"
    fi
  fi
  # A fresh checkout has no build/ at all, and the configure log below lives
  # next to the build directory rather than inside it. CI found this on the
  # first run.
  mkdir -p "$BUILD"
  cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPRODUCTION_BUILD=OFF \
        -DPLATFORM=Memory -DRMP_BUILD_EXAMPLES=ON -DRMP_RAYLIB_OPTIMIZED=ON \
        > "$BUILD.configure.log" 2>&1 \
    || { cat "$BUILD.configure.log"; echo "FAIL: the examples did not configure"; exit 1; }
}

# The e2e targets, which are out of the build's `all`: the boot run builds
# everything else, and this is the rest.
build_e2e() {
  echo "== examples: build the e2e targets =="
  # shellcheck disable=SC2046  # one --target per line of the file, by design
  cmake --build "$BUILD" $(sed 's/^/--target /' "$BUILD/e2e_targets.txt") \
        > "$BUILD.build.log" 2>&1 \
    || { tail -60 "$BUILD.build.log"; echo "FAIL: an e2e target did not build"; exit 1; }
}

# One e2e target, started in the background by play_e2e: its output and its
# exit status go to files of their own, read once every script has ended. The
# seconds it took go with them, so a slow script is seen to be slow.
play_one() {
  local t=$1 logs=$2 status=0 started
  started=$(date +%s)
  $LIMIT "$BUILD/$t" > "$logs/$t.out" 2>&1 < /dev/null || status=$?
  echo "$(( $(date +%s) - started ))" > "$logs/$t.seconds"
  echo "$status" > "$logs/$t.status"
}

# 6. Booting proves an example draws; the platformer's bot proves one game can
# be won. This plays examples from a SCRIPT -- what a player presses, frame by
# frame, at exactly 1/60 s with rmp::random at seed 1 -- and compares what they
# draw with tests/e2e/golden/, pixel for pixel. See tests/e2e/driver.cpp.
play_e2e() {
  echo "== examples: e2e, played from tests/e2e/ =="
  local scripts nscripts e2e_targets nmade played e2e_failed t status out ok
  local slots logs order s seconds
  # A script's own ceiling (above), for every launch in here.
  local LIMIT="$E2E_LIMIT"
  scripts=$(find tests/e2e -name '*.e2e' | sort)
  nscripts=$(printf '%s\n' "$scripts" | grep -c . || true)
  e2e_targets=$(cat "$BUILD/e2e_targets.txt")
  nmade=$(printf '%s\n' "$e2e_targets" | grep -c . || true)
  if [ "$nmade" -ne "$nscripts" ]; then
    echo "FAIL: $nmade e2e targets were configured for $nscripts scripts under tests/e2e/."
    echo "      CMakeLists.txt (rmp_add_e2e) no longer finds every script."
    exit 1
  fi
  if [ "${RMP_E2E_UPDATE:-}" = "1" ]; then
    echo "  recording the goldens (RMP_E2E_UPDATE=1)"
  elif [ -r "$IMAGE_MANIFEST" ]; then
    export RMP_E2E_COMPARE=1
    echo "  inside the pinned build image: every shot must match its golden exactly"
  else
    echo "  outside the pinned build image: a difference is reported, not enforced"
  fi
  # Played side by side, as many at once as the machine has processors -- or
  # as the build may compile at once, when CMAKE_BUILD_PARALLEL_LEVEL says so:
  # several builds sharing one machine lower it, and every script is a
  # sanitized process of a few hundred megabytes. Each script is a process of
  # its own, with its own folder under $BUILD/e2e/ and its own saves, and every
  # frame is exactly 1/60 s whatever else the machine is doing -- so playing
  # them together cannot change what they draw, only how long this waits: one
  # after another they took about twelve minutes in the image, the
  # platformer's alone about four. The scripts with the most frames start
  # first, so the longest is not the last to begin. A target is
  # e2e_<area>_<name>, from tests/e2e/<area>/<name>.e2e (CMakeLists.txt).
  slots=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)
  case "${CMAKE_BUILD_PARALLEL_LEVEL:-}" in
    '' | *[!0-9]* | 0) ;;
    *) slots=$CMAKE_BUILD_PARALLEL_LEVEL ;;
  esac
  logs="$BUILD/e2e-logs"
  rm -rf "$logs"
  mkdir -p "$logs"
  order=$(for s in $scripts; do
            printf '%s %s\n' "$(awk '$2 == "end" { n = $1 } END { print n + 0 }' "$s")" "$s"
          done | sort -rn | cut -d' ' -f2)
  for s in $order; do
    t=$(printf '%s\n' "$s" | sed 's|^tests/e2e/||; s|\.e2e$||; s|/|_|g; s|^|e2e_|')
    if ! printf '%s\n' "$e2e_targets" | grep -qx "$t"; then
      echo "FAIL: $s has no target $t among the configured ones."
      exit 1
    fi
    # bash 3.2 (macOS) has no `wait -n`: a slot is free when a job has ended.
    while [ "$(jobs -rp | wc -l)" -ge "$slots" ]; do sleep 1; done
    play_one "$t" "$logs" &
  done
  wait

  played=0
  e2e_failed=0
  for t in $e2e_targets; do
    if [ ! -f "$logs/$t.status" ]; then
      e2e_failed=$((e2e_failed + 1))
      echo "  FAIL  $t  (it was never started, or never ended)"
      continue
    fi
    status=$(cat "$logs/$t.status")
    seconds=$(cat "$logs/$t.seconds")
    out=$(cat "$logs/$t.out")
    played=$((played + 1))
    printf '%s\n' "$out" | grep -E '^E2E (FAIL|DIFF|NOTE|RECORDED) |^E2E [a-z]' | sed 's/^/  /' || true
    ok=1
    printf '%s\n' "$out" | grep -q '^E2E PASS$' || ok=0
    if printf '%s\n' "$out" | grep -q '^E2E FAIL '; then ok=0; fi
    printf '%s\n' "$out" | grep -q 'RAY_TEST_BOOT_OK assets_failed=0 ' || ok=0
    if printf '%s\n' "$out" | grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:'; then
      printf '%s\n' "$out" | awk '/ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:/ { n = 40 } n-- > 0' \
        | sed 's/^/          /'
      ok=0
    fi
    if [ "$ok" -eq 1 ] && [ "$status" -eq 0 ]; then
      echo "  ok    $t  (${seconds} s)"
    else
      e2e_failed=$((e2e_failed + 1))
      echo "  FAIL  $t  (exit $status, ${seconds} s)"
      # A check that failed says what and where above. Anything else -- a
      # crash, a sanitizer, a game that never reached `end` -- is in the tail.
      if ! printf '%s\n' "$out" | grep -q '^E2E FAIL '; then
        printf '%s\n' "$out" | grep -v '^E2E ' | tail -15 | sed 's/^/          /'
      fi
    fi
  done
  if [ "$played" -ne "$nscripts" ]; then
    echo "FAIL: played $played scripts, expected $nscripts. Something was skipped."
    exit 1
  fi
  if [ "$e2e_failed" -ne 0 ]; then
    echo "FAIL: $e2e_failed of $played script(s) failed. Expected, actual and diff of every"
    echo "      shot that differed: $BUILD/e2e/<area>/<example>/"
    exit 1
  fi
  if [ "${RMP_E2E_UPDATE:-}" = "1" ]; then
    echo "PASS: $played script(s) played and their goldens recorded in tests/e2e/golden/:"
    echo "      look at every picture that changed before committing it"
  else
    echo "PASS: $played script(s) played to their end"
  fi
}

if [ "$ONLY" = e2e ]; then
  configure_examples
  build_e2e
  play_e2e
  exit 0
fi

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

configure_examples
cmake --build "$BUILD" > "$BUILD.build.log" 2>&1 \
  || { tail -60 "$BUILD.build.log"; echo "FAIL: an example did not build"; exit 1; }

targets=$(cat "$BUILD/example_targets.txt")
made=$(printf '%s\n' "$targets" | grep -c . || true)
if [ "$made" -ne $((expected + headers)) ]; then
  echo "FAIL: $made example targets were configured for $expected + $headers example directories."
  echo "      The glob in CMakeLists.txt (RMP_BUILD_EXAMPLES) no longer matches the tree."
  exit 1
fi
echo "  ok    $made targets built"

echo "== examples: boot =="
mkdir -p "$SHOTS"
# The examples in plain C, as targets: examples/plain_c/src/main.c is
# example_plain_c (CMakeLists.txt). They have their own main() and none of
# the framework, so nothing in them counts a report.
plain_c=$(find examples -path '*/src/main.c' \
            | sed 's|^examples/||; s|/src/main\.c$||; s|/|_|g; s|^|example_|')
ran=0
failed=0
for t in $targets; do
  exe="$BUILD/$t"
  if [ ! -f "$exe" ]; then
    echo "  ok    $t  (header-only: compiled, nothing to run)"
    continue
  fi
  shot="$SHOTS/$t.png"
  rm -f "$shot"
  # Two statements and not `... && status=0 || status=$?`: that shape runs the
  # `||` branch when the MIDDLE command fails too, which is the bug
  # tools/shell_pattern_check.sh exists for. Here it happened to be harmless
  # (an assignment cannot fail), and "happened to be harmless" is how the shape
  # stays in a codebase until the day it is not.
  status=0
  out=$(RAY_TEST_MAX_FRAMES="$FRAMES" RAY_TEST_SCREENSHOT="$shot" $LIMIT "$exe" 2>&1 < /dev/null) || status=$?
  ran=$((ran + 1))
  ok=1
  echo "$out" | grep -q "RAY_TEST_BOOT_OK assets_failed=0 " || ok=0
  echo "$out" | grep -q "RAY_TEST_RENDER_OK" || ok=0
  echo "$out" | grep -q "RAY_TEST_DONE_FRAMES" || ok=0
  [ -s "$shot" ] || ok=0
  # Every mistake of the example's that the framework said once
  # (RMP_REPORT_ONCE: a widget outside begin()/end(), an action nobody
  # defined...) is counted, and the count printed after the last frame. An
  # example is what people copy: it makes none. A report is a WARNING line
  # of the log, and in a debug build a line on the screen too.
  reported=0
  if ! printf '%s\n' "$plain_c" | grep -qx "$t"; then
    echo "$out" | grep -q 'RAY_TEST_REPORTS=0$' || reported=1
  fi
  if [ "$ok" -eq 1 ] && [ "$reported" -eq 0 ] && [ "$status" -eq 0 ]; then
    echo "  ok    $t  $(echo "$out" | grep -o 'pixels=[0-9]* ratio=[0-9.]*' | head -1)"
  else
    failed=$((failed + 1))
    echo "  FAIL  $t  (exit $status)"
    if [ "$reported" -eq 1 ]; then
      echo "        the framework reported a mistake in it, and an example makes none:"
      echo "$out" | grep -E 'WARNING|RAY_TEST_REPORTS' | sed 's/^/          /'
    fi
    echo "$out" | tail -25 | sed 's/^/          /'
  fi
done

# The header-only ones are the only ones not booted.
if [ "$ran" -ne "$expected" ]; then
  echo "FAIL: booted $ran examples, expected $expected. Something was skipped."
  exit 1
fi
if [ "$failed" -ne 0 ]; then
  echo "FAIL: $failed example(s) did not boot, draw and exit cleanly, or made a mistake the"
  echo "      framework reported. Screenshots of the ones that drew: $SHOTS/"
  exit 1
fi
echo "PASS: $ran examples booted, drew and exited; screenshots in $SHOTS/"

# At the design size every [window] stretch draws the same picture, so the
# boot above says nothing about one. scenes/02_stretch is booted twice more
# in a window of another shape -- a phone held upright, and a strip wider
# than 16:9 -- which RAY_TEST_WINDOW opens under the smoke test and nowhere
# else; and its last frame is read back. Under LETTERBOX, the framework's
# .toml's: everything outside the design's area, scaled to fit and centred,
# is the scene's clear colour; the world reaches every edge of that area; and
# the court's white lines are where a mirror puts them, about the window's
# middle row and column. A world drawn over the bars, an interface outside
# the area or a camera off centre each fails one of the three.
echo "== examples: scenes/02_stretch, letterboxed in two other shapes =="
STRETCH=example_scenes_02_stretch
# CourtScene's `background`, in examples/scenes/02_stretch/src/main.cpp.
STRETCH_CLEAR=8,8,12
for size in 450x800 1280x450; do
  shot="$SHOTS/$STRETCH-$size.png"
  rm -f "$shot"
  status=0
  out=$(RAY_TEST_MAX_FRAMES="$FRAMES" RAY_TEST_WINDOW="$size" RAY_TEST_SCREENSHOT="$shot" $LIMIT "$BUILD/$STRETCH" 2>&1 < /dev/null) || status=$?
  ok=1
  for marker in "RAY_TEST_WINDOW $size\$" 'RAY_TEST_BOOT_OK assets_failed=0 ' \
                RAY_TEST_RENDER_OK 'RAY_TEST_REPORTS=0$'; do
    printf '%s\n' "$out" | grep -q "$marker" || ok=0
  done
  if [ "$ok" -ne 1 ] || [ "$status" -ne 0 ] || [ ! -s "$shot" ]; then
    printf '%s\n' "$out" | tail -25 | sed 's/^/          /'
    echo "FAIL: $STRETCH did not boot, draw and exit cleanly in a $size window (exit $status)"
    exit 1
  fi
  python3 - "$shot" include/rmp/generated/config.h "$STRETCH_CLEAR" <<'PY' || exit 1
import re
import struct
import sys
import zlib

shot, header, clear = sys.argv[1], sys.argv[2], tuple(int(c) for c in sys.argv[3].split(","))


def fail(why):
    print(f"FAIL: {shot}: {why}")
    sys.exit(1)


config = open(header).read()


def define(name):
    found = re.search(rf"^#define {name}\s+(\d+)$", config, re.M)
    if not found:
        fail(f"{header} defines no {name}: python3 tools/configure.py")
    return int(found.group(1))


design_w, design_h = define("RMP_WINDOW_WIDTH"), define("RMP_WINDOW_HEIGHT")
if define("RMP_WINDOW_STRETCH") != 1:
    fail("[window] stretch is not \"letterbox\" in raylib_multiplatform.toml, and this "
         "is what a letterbox looks like")


def read_png(path):
    """(width, height, rows of (r, g, b)) -- an 8-bit RGB or RGBA PNG, not
    interlaced, which is what raylib writes. The standard library and nothing
    else."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        fail("not a PNG")
    at, chunks, head = 8, [], None
    while at + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        if kind == b"IHDR":
            head = struct.unpack(">IIBBBBB", data[at + 8:at + 8 + length])
        elif kind == b"IDAT":
            chunks.append(data[at + 8:at + 8 + length])
        at += 12 + length
    width, height, depth, color_type, _, _, interlace = head
    channels = {2: 3, 6: 4}.get(color_type)
    if depth != 8 or channels is None or interlace != 0:
        fail(f"a PNG this does not read: depth {depth}, colour type {color_type}, "
             f"interlace {interlace}")
    raw = zlib.decompress(b"".join(chunks))
    stride = width * channels
    rows, above, at = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride if kind else 0):
            a = line[i - channels] if i >= channels else 0
            b = above[i]
            c = above[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * channels:x * channels + 3]) for x in range(width)])
        above = line
    return width, height, rows


width, height, px = read_png(shot)
scale = min(width / design_w, height / design_h)
left, top = (width - design_w * scale) / 2, (height - design_h * scale) / 2
right, bottom = width - left, height - top
if max(left, top) < 8:
    fail(f"a {width}x{height} window has no bars to look at")

# The bars. A pixel on the area's edge may be either: the edge need not fall
# on a whole pixel.
wrong = [(x, y) for y in range(height) for x in range(width)
         if (x < left - 1 or x >= right + 1 or y < top - 1 or y >= bottom + 1)
         and px[y][x] != clear]
if wrong:
    x, y = wrong[0]
    fail(f"{len(wrong)} pixel(s) outside the {right - left:g}x{bottom - top:g} area are "
         f"not the clear colour {clear}: the first, ({x}, {y}), is {px[y][x]}")

# The world, to every edge of the area: two pixels in, nothing is the clear
# colour.
x0, x1, y0, y1 = int(left) + 2, int(right) - 3, int(top) + 2, int(bottom) - 3
edge = [(x, y) for x in range(x0, x1 + 1) for y in (y0, y1)] + \
       [(x, y) for y in range(y0, y1 + 1) for x in (x0, x1)]
gaps = [(x, y) for x, y in edge if px[y][x] == clear]
if gaps:
    fail(f"{len(gaps)} pixel(s) just inside the area are the clear colour: the world "
         f"stops short of its edge, first at {gaps[0]}")


# The court: its white lines on the middle row (the sidelines, the centre
# circle, the halfway line) and the middle column (the halfway line, from
# one end line to the other) are where a mirror puts them, to a pixel.
def unmirrored(line, length):
    white = [i for i, colour in enumerate(line) if colour == (255, 255, 255)]
    return white, [i for i in white if not any(abs(length - 1 - i - j) <= 1 for j in white)]


row, off_row = unmirrored(px[height // 2], width)
column, off_column = unmirrored([px[y][width // 2] for y in range(height)], height)
if not row or not column:
    fail("no court line crosses the middle of the window")
if off_row or off_column:
    fail(f"the court is not centred: white at x {off_row[:5]} on the middle row and "
         f"y {off_column[:5]} on the middle column has no mirror image")
print(f"  ok    {shot}: a {right - left:g}x{bottom - top:g} area, the bars "
      f"{clear}, the court centred")
PY
done

# Booting a game for thirty frames proves its title screen draws, and nothing
# else. tests/platformer_play.cpp PLAYS the platformer through its real loop:
# to the flag at 60 Hz, Play again, three falls, game over, Menu; a jump from
# the moving platform at 60 and 240 Hz; to the flag again at 240 Hz. See the
# file for why each one is there.
echo "== examples: the platformer, played =="
if [ ! -x "$BUILD/platformer_play" ]; then
  echo "FAIL: $BUILD/platformer_play was not built (CMakeLists.txt, RMP_BUILD_EXAMPLES)"
  exit 1
fi
status=0
out=$($LIMIT "$BUILD/platformer_play" 2>&1 < /dev/null) || status=$?
printf '%s\n' "$out" | grep '^PLAY' | sed 's/^/  /'
if [ "$status" -ne 0 ] || ! printf '%s\n' "$out" | grep -q '^PLAY PASS$'; then
  printf '%s\n' "$out" | tail -15 | sed 's/^/          /'
  echo "FAIL: the platformer could not be played to the end (exit $status)"
  exit 1
fi
echo "PASS: the platformer was won at 60 and 240 Hz, lost, and started again"

# rmp::input through the real runner, in both shapes a game is written in: a
# hook that reads the input itself (RMP_ENTRY_POINT) and a scene under frame()
# (RMP_GAME). Each press has to be seen, and seen once. See tests/input_play.cpp.
echo "== examples: the input, read both ways =="
if [ ! -x "$BUILD/input_play" ]; then
  echo "FAIL: $BUILD/input_play was not built (CMakeLists.txt, RMP_BUILD_EXAMPLES)"
  exit 1
fi
status=0
out=$($LIMIT "$BUILD/input_play" 2>&1 < /dev/null) || status=$?
printf '%s\n' "$out" | grep '^INPUT' | sed 's/^/  /'
if [ "$status" -ne 0 ] || ! printf '%s\n' "$out" | grep -q '^INPUT PASS$'; then
  printf '%s\n' "$out" | tail -15 | sed 's/^/          /'
  echo "FAIL: rmp::input missed a press, or saw one twice (exit $status)"
  exit 1
fi
echo "PASS: a press is seen once, by a game's own hook and by a scene"

if [ "$ONLY" = boot ]; then
  exit 0
fi
build_e2e
play_e2e
