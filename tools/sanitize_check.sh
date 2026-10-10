#!/usr/bin/env bash
# The unit tests again, under AddressSanitizer and UndefinedBehaviorSanitizer,
# built by GCC.
#
# A test asserts what it can see, and undefined behaviour is exactly what a
# test cannot see: a signed overflow that wraps to a harmless number, a float
# cast to int that lands somewhere in range, a read one past a vector that
# finds a zero. The LDtk reader shipped all three to its first review, every
# one of them under a test that passed, and the hostile-input test that was
# meant to find them could not -- the code it exercised behaved by luck. This
# build makes luck fail: every report is fatal, and the run is red.
#
# GCC, always. The framework's every Debug build is already sanitized --
# cmake/sanitize.cmake applies [dev] sanitize, and [dev] compiler is clang --
# so `rmp test unit` is the clang run. This is the second compiler: gcc's
# ASan and UBSan instrument differently (float-cast-overflow is not in its
# "undefined" and is named), and its warnings are not clang's, so the build is
# RMP_WERROR too. It used to pick clang when clang could link a sanitized
# program; that is the run `rmp test` does now.
#
# The flags are not here: they are cmake/sanitize.cmake's, from the
# framework's [dev] sanitize, and RMP_REQUIRE_SANITIZERS=1 makes the configure
# fail if gcc cannot run them, instead of building an uninstrumented binary
# and passing. (The three flags emptied below are what this script used to
# pass itself, and a build directory keeps them in its cache.)
# The runtimes' defaults -- every report fatal, the stack printed -- are in
# cmake/sanitizer_hooks.c, linked into the test binary like into every other.
#
# Its own build directory, so it never disturbs build/debug.
#
#   bash tools/sanitize_check.sh        or   rmp test sanitize
#
# ASCII only and no GNU-only constructs: tools/portable_check.sh covers this
# directory, and macOS ships bash 3.2.
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD="build/sanitize"
if ! command -v gcc >/dev/null 2>&1 || ! command -v g++ >/dev/null 2>&1; then
    echo "FAIL: this is the GCC run of the sanitized unit tests, and there is no gcc/g++ here."
    echo "      The clang run is \`rmp test unit\`: the framework's Debug build is sanitized."
    exit 1
fi
echo "  compiler: $(g++ --version | head -1)"

echo "== sanitize =="
mkdir -p "$BUILD"
RMP_REQUIRE_SANITIZERS=1 cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
      -DRMP_DEV_TOOLCHAIN=OFF -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
      -DRMP_SANITIZE=ON -DRMP_WERROR=ON \
      -DCMAKE_C_FLAGS= -DCMAKE_CXX_FLAGS= -DCMAKE_EXE_LINKER_FLAGS= > "$BUILD.configure.log" 2>&1 \
  || { tail -40 "$BUILD.configure.log"; echo "FAIL: the sanitized build did not configure"; exit 1; }
grep -q '=== SANITIZERS: address, undefined ===' "$BUILD.configure.log" || {
    grep -n 'SANITIZ' "$BUILD.configure.log" || true
    echo "FAIL: the build is not instrumented with address and undefined"; exit 1; }
cmake --build "$BUILD" --target unit_test > "$BUILD.build.log" 2>&1 \
  || { tail -60 "$BUILD.build.log"; echo "FAIL: the sanitized unit tests did not build"; exit 1; }

# -fno-sanitize-recover and the hooks' halt_on_error make the first report end
# the run with a non-zero status; the summary line is checked as well, because
# a run that dies before doctest prints anything is not a pass either.
# RMP_REQUIRE_SANITIZERS makes tests/sanitize_test.cpp fail rather than skip
# on a binary with no sanitizer in it.
if [ -d build/test-locales ]; then export LOCPATH="$PWD/build/test-locales"; fi
status=0
out=$(RMP_REQUIRE_SANITIZERS=1 "$BUILD/unit_test" --test-suite-exclude="audio: device" 2>&1 < /dev/null) || status=$?
if [ "$status" -ne 0 ] || ! printf '%s\n' "$out" | grep -q 'Status: SUCCESS'; then
  printf '%s\n' "$out" | grep -E 'runtime error|ERROR: AddressSanitizer|ERROR: LeakSanitizer|SUMMARY|#[0-9]+ |Status|ERROR:' | head -60
  echo "FAIL: the unit tests are not clean under ASan and UBSan (exit $status)"
  exit 1
fi
printf '%s\n' "$out" | grep -E '^\[doctest\] test cases' | sed 's/^/  /'
echo "PASS: no AddressSanitizer or UndefinedBehaviorSanitizer report"
