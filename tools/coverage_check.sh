#!/usr/bin/env bash
# What the tests run of the framework: clang's source-based coverage of
# src/rmp/, held by two ratchets.
#
# Builds the unit tests, the headless UI layout test and tests/input_play.cpp
# (the one program that drives src/rmp/app.cpp, through the real runner) with
# -fprofile-instr-generate -fcoverage-mapping -- RMP_COVERAGE in
# CMakeLists.txt, which also links librmp.a WHOLE, so a file no test reaches is
# in the report at 0% instead of missing from it. Runs them, merges the
# profiles, and hands llvm-cov's per-line export to tools/coverage_report.py,
# whose exit status is this script's: the floors in tools/coverage_floor.txt,
# which only rise, and tools/untested_api.txt, the public functions no test
# calls, which only shrinks. See that file for what fails where.
#
# The numbers are the pinned image's: its clang, its tests (the audio device
# and the test locales are required there). Anywhere else they are printed.
#
#   bash tools/coverage_check.sh              or   rmp test coverage
#   bash tools/coverage_check.sh --update     in the image: raise the floors
#                                             to this run, drop what is called
#
# Writes build/coverage/: coverage.lcov (what the report reads),
# coverage.json, summary.md, untested_api.txt (this run's list) and html/,
# llvm-cov's annotated sources.
#
# Its own build directory, so it never disturbs build/debug; a Debug build
# without the sanitizers, which tell nothing about which lines ran and make the
# run several times slower -- the lint job runs these tests under them.
#
# ASCII only and no GNU-only constructs: tools/portable_check.sh covers this
# directory, and macOS ships bash 3.2.
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD="build/coverage"
PROF="$BUILD/prof"
UPDATE=""
for arg in "$@"; do
  case "$arg" in
    --update) UPDATE="--update" ;;
    *) echo "usage: bash tools/coverage_check.sh [--update]"; exit 2 ;;
  esac
done

hint() {
  echo "      Coverage needs clang with its profile runtime (compiler-rt) and the"
  echo "      llvm-profdata, llvm-cov and llvm-cxxfilt of the same LLVM release."
  echo "      The pinned build image has all of them, and its numbers are the ones"
  echo "      the floors were measured with:"
  echo "        tools/dev_shell.sh bash tools/coverage_check.sh"
  echo "      (or podman/docker run --rm -v \"\$PWD\":/work -w /work <image> ..., the"
  echo "      image named by build_image_digest in thirdparty/FROZEN_VERSIONS.md)"
}

# --- the probe ---------------------------------------------------------------
# Never a skip: a coverage run that quietly measured nothing would print the
# floors as notes and pass. Every tool is asked for and RUN, on a program
# with one called and one uncalled function, all the way to the export.
echo "== coverage: the toolchain =="
if ! command -v clang++ >/dev/null 2>&1 || ! command -v clang >/dev/null 2>&1; then
  echo "FAIL: there is no clang/clang++ here, and the coverage is clang's."
  hint; exit 1
fi
MAJOR=$(clang++ -dumpversion | cut -d. -f1)
# llvm-<tool>-<major> where a distribution versions them (Ubuntu), else the
# plain name -- but only when it belongs to the same LLVM: a profile written
# by one release is not read by another.
llvm_tool() {
  local found=""
  if command -v "$1-$MAJOR" >/dev/null 2>&1; then
    found="$1-$MAJOR"
  elif command -v "$1" >/dev/null 2>&1; then
    found="$1"
  else
    echo "FAIL: no $1 (nor $1-$MAJOR) for clang $MAJOR." >&2
    return 1
  fi
  local got
  got=$("$found" --version 2>/dev/null | sed -nE 's/.*LLVM version ([0-9]+).*/\1/p' | head -1)
  if [ "$got" != "$MAJOR" ]; then
    echo "FAIL: $found is LLVM ${got:-unknown} and clang++ is $MAJOR." >&2
    return 1
  fi
  echo "$found"
}
PROFDATA=$(llvm_tool llvm-profdata) || { hint; exit 1; }
LLVMCOV=$(llvm_tool llvm-cov) || { hint; exit 1; }
CXXFILT=$(llvm_tool llvm-cxxfilt) || { hint; exit 1; }
echo "  clang $MAJOR: $(clang++ --version | head -1)"
echo "  $PROFDATA, $LLVMCOV, $CXXFILT"

PROBE="$BUILD/probe"
rm -rf "$PROBE"
mkdir -p "$PROBE"
cat > "$PROBE/probe.cpp" <<'EOF'
int ran(int x) { return x + 1; }
int never(int x) { return x - 1; }
int main(int argc, char **) { return ran(argc) > argc ? 0 : 1; }
EOF
probe_failed() {
  echo "FAIL: $1"
  if [ -s "$PROBE/log" ]; then sed 's/^/      /' "$PROBE/log" | head -12; fi
  hint
  exit 1
}
clang++ -fprofile-instr-generate -fcoverage-mapping "$PROBE/probe.cpp" -o "$PROBE/probe" \
  > "$PROBE/log" 2>&1 || probe_failed "clang++ cannot link a program with -fprofile-instr-generate"
LLVM_PROFILE_FILE="$PROBE/probe.profraw" "$PROBE/probe" > "$PROBE/log" 2>&1 < /dev/null \
  || probe_failed "the instrumented probe did not run"
[ -s "$PROBE/probe.profraw" ] || probe_failed "the instrumented probe wrote no profile"
"$PROFDATA" merge -sparse -o "$PROBE/probe.profdata" "$PROBE/probe.profraw" > "$PROBE/log" 2>&1 \
  || probe_failed "$PROFDATA could not read the profile clang's runtime wrote"
"$LLVMCOV" export -format=lcov -instr-profile "$PROBE/probe.profdata" "$PROBE/probe" \
  > "$PROBE/probe.lcov" 2> "$PROBE/log" || probe_failed "$LLVMCOV could not export the probe"
if ! grep -q '^FNDA:1,_Z3rani$' "$PROBE/probe.lcov" || ! grep -q '^FNDA:0,_Z5neveri$' "$PROBE/probe.lcov"; then
  probe_failed "$LLVMCOV's export does not say which function ran"
fi
[ "$(echo _Z5neveri | "$CXXFILT")" = "never(int)" ] || probe_failed "$CXXFILT does not demangle"
echo "  ok    a probe was built, run, merged, exported and demangled"

# --- build -------------------------------------------------------------------
# PLATFORM=Memory: raylib's software platform, so input_play runs with no
# display. The unit tests open no window on any platform.
echo "== coverage: build =="
mkdir -p "$BUILD"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPRODUCTION_BUILD=OFF \
      -DBUILD_TESTS=ON -DBUILD_UI_TESTS=ON -DPLATFORM=Memory -DRMP_COVERAGE=ON \
      -DRMP_SANITIZE=OFF -DRMP_DEV_TOOLCHAIN=OFF \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ > "$BUILD.configure.log" 2>&1 \
  || { tail -40 "$BUILD.configure.log"; echo "FAIL: the coverage build did not configure"; exit 1; }
cmake --build "$BUILD" --target unit_test ui_layout_test input_play > "$BUILD.build.log" 2>&1 \
  || { tail -60 "$BUILD.build.log"; echo "FAIL: the coverage build did not build"; exit 1; }
echo "  ok    unit_test, ui_layout_test, input_play"

# --- run -----------------------------------------------------------------------
# One profile per program, named after it, so a program that wrote none -- it
# died before exit, or never started -- is a failure and not a file that is
# simply absent from the merge.
echo "== coverage: run =="
rm -rf "$PROF"
mkdir -p "$PROF"
bash tools/test_locales.sh build/test-locales
export LOCPATH="$PWD/build/test-locales"
# Inside the image the numbers are the gate, so the tests that skip where they
# cannot run may not skip there: a skipped test is lines that did not run.
if [ -f /etc/raylib-build-image.json ]; then
  export RMP_REQUIRE_TEST_LOCALES=1 RMP_REQUIRE_AUDIO_DEVICE=1
fi
run() {  # name, the line that says it passed, arguments...
  local name="$1" pass="$2" status=0 out
  shift 2
  out=$(LLVM_PROFILE_FILE="$PWD/$PROF/$name-%p.profraw" "$BUILD/$name" "$@" 2>&1 < /dev/null) \
    || status=$?
  if [ "$status" -ne 0 ] || ! printf '%s\n' "$out" | grep -q "$pass"; then
    printf '%s\n' "$out" | tail -30 | sed 's/^/      /'
    echo "FAIL: $name did not pass under coverage (exit $status)"
    exit 1
  fi
  if ! ls "$PROF/$name"-*.profraw >/dev/null 2>&1; then
    echo "FAIL: $name passed and wrote no profile to $PROF/"
    exit 1
  fi
  echo "  ok    $name"
}
run unit_test 'Status: SUCCESS'
run ui_layout_test '^PASS$'
run input_play '^INPUT PASS$'

# input_play once more, as the CI smoke test starts a game: under
# RAY_TEST_MAX_FRAMES, its window at a size of RAY_TEST_WINDOW's. That is
# app.cpp's smoke path -- the save round trip before RAY_TEST_BOOT_OK, the
# window it opens, the count RAY_TEST_REPORTS prints on the last frame -- which
# a run that is not the smoke test never reaches. Its own profile name, so it
# is a program that has to have written one.
smoke_status=0
export LLVM_PROFILE_FILE="$PWD/$PROF/input_play_smoke-%p.profraw"
smoke=$(RAY_TEST_MAX_FRAMES=40 RAY_TEST_WINDOW=640x360 "$BUILD/input_play" < /dev/null 2>&1) \
  || smoke_status=$?
unset LLVM_PROFILE_FILE
for marker in '^INPUT PASS$' 'RAY_TEST_WINDOW 640x360' 'RAY_TEST_SAVE_OK' \
              'RAY_TEST_BOOT_OK assets_failed=0 ' 'RAY_TEST_REPORTS=0$'; do
  if [ "$smoke_status" -ne 0 ] || ! printf '%s\n' "$smoke" | grep -q "$marker"; then
    printf '%s\n' "$smoke" | tail -30 | sed 's/^/      /'
    echo "FAIL: input_play under the smoke test did not say $marker (exit $smoke_status)"
    exit 1
  fi
done
if ! ls "$PROF"/input_play_smoke-*.profraw >/dev/null 2>&1; then
  echo "FAIL: input_play under the smoke test passed and wrote no profile to $PROF/"
  exit 1
fi
echo "  ok    input_play, under the smoke test"

# --- merge and report ------------------------------------------------------------
echo "== coverage: report =="
"$PROFDATA" merge -sparse -o "$BUILD/coverage.profdata" "$PROF"/*.profraw
OBJECTS="$BUILD/unit_test -object $BUILD/ui_layout_test -object $BUILD/input_play"
# llvm-cov warns that "N functions have mismatched data" when a program maps a
# function it never emitted -- an inline one it did not call -- and another
# program's profile holds that name with its real hash. That record is the one
# counted, and the export is the same in any order of the programs; anything
# else it says is printed.
# shellcheck disable=SC2086  # OBJECTS is words on purpose
"$LLVMCOV" export -format=lcov -instr-profile "$BUILD/coverage.profdata" $OBJECTS \
  > "$BUILD/coverage.lcov" 2> "$BUILD/export.log" \
  || { cat "$BUILD/export.log"; echo "FAIL: $LLVMCOV export"; exit 1; }
grep -v 'functions have mismatched data$' "$BUILD/export.log" || true
rm -rf "$BUILD/html"
# shellcheck disable=SC2086
"$LLVMCOV" show -format=html -output-dir "$BUILD/html" -show-line-counts-or-regions \
  -instr-profile "$BUILD/coverage.profdata" $OBJECTS 2> /dev/null \
  || { echo "FAIL: $LLVMCOV show -format=html"; exit 1; }
[ -s "$BUILD/html/index.html" ] || { echo "FAIL: no $BUILD/html/index.html"; exit 1; }

python3 tools/coverage_report.py --lcov "$BUILD/coverage.lcov" \
  --compile-commands "$BUILD/compile_commands.json" --cxx clang++ --cxxfilt "$CXXFILT" \
  --out "$BUILD" ${UPDATE:+"$UPDATE"}
