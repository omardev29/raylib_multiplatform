#!/usr/bin/env bash
# The unit tests again, under AddressSanitizer and UndefinedBehaviorSanitizer.
#
# A test asserts what it can see, and undefined behaviour is exactly what a
# test cannot see: a signed overflow that wraps to a harmless number, a float
# cast to int that lands somewhere in range, a read one past a vector that
# finds a zero. The LDtk reader shipped all three to its first review, every
# one of them under a test that passed, and the hostile-input test that was
# meant to find them could not -- the code it exercised behaved by luck. This
# build makes luck fail: every report is fatal, and the run is red.
#
# Its own build directory, so it never disturbs build/ (the debug and release
# presets already share that one, see CLAUDE.md).
#
#   bash tools/sanitize_check.sh        or   rmp test sanitize
#
# ASCII only and no GNU-only constructs: tools/portable_check.sh covers this
# directory, and macOS ships bash 3.2.
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD="build/sanitize"
# float-cast-overflow by name: clang counts it in "undefined", gcc does not,
# and a float cast to int is exactly the UB the LDtk reader had.
FLAGS="-fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer -DRMP_SANITIZE"

# Clang when it can link a sanitized program, gcc otherwise. The CI image has
# clang without its compiler-rt (no libclang_rt.asan), and gcc with libasan and
# libubsan: one probe decides, instead of a job that fails on the linker.
probe_dir=$(mktemp -d)
trap 'rm -rf "$probe_dir"' EXIT
printf 'int main() { return 0; }\n' > "$probe_dir/p.cpp"
CXX_PICK=g++
CC_PICK=gcc
if command -v clang++ >/dev/null 2>&1 &&
   clang++ -fsanitize=address,undefined "$probe_dir/p.cpp" -o "$probe_dir/p" >/dev/null 2>&1; then
  CXX_PICK=clang++
  CC_PICK=clang
  # Third-party code only, each line with its reason: see the file. gcc has
  # no ignorelist, and does not instrument that idiom anyway: it folds the
  # null-pointer offset into a constant.
  FLAGS="$FLAGS -fsanitize-ignorelist=$PWD/tools/sanitize_ignore.txt"
fi
echo "  compiler: $CXX_PICK"

echo "== sanitize =="
mkdir -p "$BUILD"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON \
      -DRMP_DEV_TOOLCHAIN=OFF -DCMAKE_C_COMPILER="$CC_PICK" -DCMAKE_CXX_COMPILER="$CXX_PICK" \
      -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" > "$BUILD.configure.log" 2>&1 \
  || { tail -40 "$BUILD.configure.log"; echo "FAIL: the sanitized build did not configure"; exit 1; }
cmake --build "$BUILD" --target unit_test > "$BUILD.build.log" 2>&1 \
  || { tail -60 "$BUILD.build.log"; echo "FAIL: the sanitized unit tests did not build"; exit 1; }

# halt_on_error and -fno-sanitize-recover make the first report end the run
# with a non-zero status; the summary line is checked as well, because a run
# that dies before doctest prints anything is not a pass either.
export ASAN_OPTIONS="detect_leaks=1:abort_on_error=0:halt_on_error=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
if [ -d build/test-locales ]; then export LOCPATH="$PWD/build/test-locales"; fi
status=0
out=$("$BUILD/unit_test" --test-suite-exclude="audio: device" 2>&1) || status=$?
if [ "$status" -ne 0 ] || ! printf '%s\n' "$out" | grep -q 'Status: SUCCESS'; then
  printf '%s\n' "$out" | grep -E 'runtime error|ERROR: AddressSanitizer|ERROR: LeakSanitizer|SUMMARY|#[0-9]+ |Status|ERROR:' | head -60
  echo "FAIL: the unit tests are not clean under ASan and UBSan (exit $status)"
  exit 1
fi
printf '%s\n' "$out" | grep -E '^\[doctest\] test cases' | sed 's/^/  /'
echo "PASS: no AddressSanitizer or UndefinedBehaviorSanitizer report"
