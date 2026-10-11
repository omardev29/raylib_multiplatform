#!/usr/bin/env bash
# The fuzz targets: build them, replay what is committed, fuzz, merge.
#
# A fuzz target is tests/fuzz/<name>.cpp (LLVMFuzzerTestOneInput, see
# tests/fuzz/harness.h), built by tests/deep.cmake as fuzz_<name>. Its inputs
# are committed: tests/fuzz/corpus/<name>/, what fuzzing found and merged,
# and tests/fuzz/regressions/<name>/, every input that once failed.
#
#   bash tools/fuzz.sh build                 every fuzz target, in build/fuzz
#   bash tools/fuzz.sh replay                every committed input of every target
#   bash tools/fuzz.sh replay NAME [FILE...] one target's, or exactly those files
#   bash tools/fuzz.sh run NAME SECONDS      libFuzzer, for that long
#   bash tools/fuzz.sh merge NAME            what `run` found, into the corpus
#   bash tools/fuzz.sh targets               the names, one a line
#
# `rmp test fuzz` is build and replay; the CI deep job runs the same on every
# push, and .github/workflows/deep.yml runs and merges every night.
#
# build: ASan, UBSan (float-cast-overflow included: clang counts it in
# "undefined", cmake/sanitize.cmake names it for gcc) and the library
# hardening, every report fatal -- the framework's Debug flags -- and libFuzzer
# when clang can link AND run a fuzzer: the pinned image, or a clang with
# compiler-rt. Without one the same targets are built with
# tests/fuzz/replay_main.cpp, which replays and cannot fuzz, and says so; by
# clang when its sanitizers run, else by gcc (a clang without compiler-rt
# compiles everything and links no sanitizer). RMP_REQUIRE_LIBFUZZER=1 (CI)
# makes a missing libFuzzer a failure instead.
#
# replay: every input once, and it FAILS UNLESS EVERY FILE IT NAMED RAN --
# libFuzzer and replay_main.cpp both print "Executed <file>" per input, and
# the count has to be the number of files. A replay that stopped early
# reporting nothing is the 105-test suite that quietly ran 72.
#
# Its own build directory, so it never disturbs build/debug. ASCII only and no
# GNU-only constructs: tools/portable_check.sh covers this directory, and
# macOS ships bash 3.2.
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD="build/fuzz"
FUZZ="tests/fuzz"

usage() {
  echo "usage: bash tools/fuzz.sh build | replay [NAME [FILE...]] | run NAME SECONDS | merge NAME | targets"
  exit 2
}

# The targets, from the harnesses: tests/fuzz/*.cpp but replay_main.cpp.
targets() {
  local f name
  for f in "$FUZZ"/*.cpp; do
    [ -e "$f" ] || continue
    name=$(basename "$f" .cpp)
    [ "$name" = replay_main ] && continue
    echo "$name"
  done
}

known_target() {
  if ! targets | grep -qx -- "$1"; then
    echo "FAIL: no fuzz target called '$1': there is no $FUZZ/$1.cpp. The targets:"
    targets | sed 's/^/        /'
    exit 2
  fi
}

# Called in the script's own shell, never inside $(...): an exit there would
# end only the subshell, and its message would land in a variable.
require_built() {
  if [ ! -f "$BUILD/engine" ]; then
    echo "FAIL: nothing is built in $BUILD: bash tools/fuzz.sh build first"
    exit 1
  fi
}

engine() { cat "$BUILD/engine"; }

# Whether clang can link a fuzzer with the sanitizers and run it: asked by
# doing it, never by finding a flag -- Ubuntu's clang accepts -fsanitize=fuzzer
# with no libclang_rt.fuzzer to link.
libfuzzer_runs() {
  command -v clang++ >/dev/null 2>&1 || return 1
  local probe="$BUILD/probe"
  rm -rf "$probe"
  mkdir -p "$probe"
  cat > "$probe/probe.cpp" <<'EOF'
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    return size > 0 && data[0] == 'x' ? 0 : 0;
}
EOF
  clang++ -fsanitize=fuzzer,address,undefined "$probe/probe.cpp" -o "$probe/probe" \
    > "$probe/log" 2>&1 || return 1
  "$probe/probe" -runs=16 > "$probe/log" 2>&1 < /dev/null || return 1
  grep -q 'Done 16 runs' "$probe/log"
}

# Whether clang can link a program with ASan and UBSan and run it -- the same
# question cmake/sanitize.cmake asks, and the build is configured with
# RMP_REQUIRE_SANITIZERS=1. A clang without compiler-rt compiles everything
# and links none of it, and gcc's libasan may well be there.
clang_sanitizes() {
  command -v clang++ >/dev/null 2>&1 || return 1
  command -v clang >/dev/null 2>&1 || return 1
  local probe="$BUILD/probe"
  mkdir -p "$probe"
  printf 'int main() { return 0; }\n' > "$probe/sanitized.cpp"
  clang++ -fsanitize=address,undefined "$probe/sanitized.cpp" -o "$probe/sanitized" \
    > "$probe/sanitized.log" 2>&1 || return 1
  "$probe/sanitized" > "$probe/sanitized.log" 2>&1 < /dev/null
}

build() {
  mkdir -p "$BUILD"
  # Written again only by a build that finished: a failed one leaves nothing
  # that replay or run would take for the targets of another engine.
  rm -f "$BUILD/engine"
  local engine cc cxx
  if libfuzzer_runs; then
    engine=libfuzzer
  elif [ "${RMP_REQUIRE_LIBFUZZER:-}" = 1 ]; then
    echo "FAIL: clang cannot link and run a libFuzzer program here, and RMP_REQUIRE_LIBFUZZER=1."
    if [ -s "$BUILD/probe/log" ]; then sed 's/^/      /' "$BUILD/probe/log" | head -12; fi
    echo "      The pinned build image has clang 18 with compiler-rt."
    exit 1
  else
    engine=replay
    echo "  note  no libFuzzer here (clang with compiler-rt): the targets replay and cannot fuzz."
    echo "        The pinned image can: tools/dev_shell.sh bash tools/fuzz.sh build"
  fi
  # libFuzzer is clang's. To replay, the compiler is whichever runs the
  # sanitizers: clang when it can, else gcc -- a clang without compiler-rt
  # picked here made the configure refuse a build gcc could have made.
  if [ "$engine" = libfuzzer ] || clang_sanitizes; then
    cc=clang
    cxx=clang++
  else
    cc=gcc
    cxx=g++
  fi
  echo "== fuzz: build ($engine, $($cxx --version | head -1)) =="
  # PLATFORM=Memory: raylib's headless platform, so nothing here asks for X11.
  # The three emptied flags are what a build directory would otherwise keep.
  RMP_REQUIRE_SANITIZERS=1 cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DPRODUCTION_BUILD=OFF -DBUILD_TESTS=ON -DPLATFORM=Memory -DRMP_SANITIZE=ON \
        -DRMP_WERROR=ON -DRMP_DEV_TOOLCHAIN=OFF -DRMP_FUZZ_ENGINE="$engine" \
        -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
        -DCMAKE_C_FLAGS= -DCMAKE_CXX_FLAGS= -DCMAKE_EXE_LINKER_FLAGS= \
        > "$BUILD.configure.log" 2>&1 \
    || { tail -40 "$BUILD.configure.log"; echo "FAIL: the fuzz build did not configure"; exit 1; }
  grep -q '=== SANITIZERS: address, undefined ===' "$BUILD.configure.log" || {
    grep -n 'SANITIZ' "$BUILD.configure.log" || true
    echo "FAIL: the fuzz build is not instrumented with address and undefined"; exit 1; }
  grep -q "=== FUZZ: $engine," "$BUILD.configure.log" || {
    grep -n 'FUZZ' "$BUILD.configure.log" || true
    echo "FAIL: tests/deep.cmake did not define the fuzz targets with the $engine engine"; exit 1; }
  cmake --build "$BUILD" --target fuzz_all > "$BUILD.build.log" 2>&1 \
    || { tail -60 "$BUILD.build.log"; echo "FAIL: the fuzz targets did not build"; exit 1; }
  local name missing=0
  for name in $(targets); do
    if [ ! -x "$BUILD/fuzz_$name" ]; then
      echo "FAIL: $BUILD/fuzz_$name was not built"
      missing=1
    fi
  done
  [ "$missing" -eq 0 ] || exit 1
  echo "$engine" > "$BUILD/engine"
  echo "  ok    $(targets | wc -l | tr -d ' ') target(s), $engine"
}

# Replay FILES on one target; fails unless it exits 0 having executed each.
replay_files() {
  local name="$1" status=0 out ran
  shift
  out=$("$BUILD/fuzz_$name" -timeout=60 -rss_limit_mb=2048 "$@" 2>&1 < /dev/null) || status=$?
  ran=$(printf '%s\n' "$out" | grep -c '^Executed ' || true)
  if [ "$status" -ne 0 ] || [ "$ran" -ne "$#" ]; then
    printf '%s\n' "$out" | grep -v -e '^Executed ' -e '^Running: ' | tail -60 | sed 's/^/      /'
    echo "FAIL: fuzz_$name ran $ran of $# input(s) and exited $status"
    return 1
  fi
  echo "  ok    $name: $ran input(s)"
}

replay() {
  local engine_used
  require_built
  engine_used=$(engine)
  echo "== fuzz: replay ($engine_used) =="
  if [ $# -ge 2 ]; then
    known_target "$1"
    replay_files "$@"
    return
  fi
  local names name failed=0 f
  if [ $# -eq 1 ]; then
    known_target "$1"
    names="$1"
  else
    names=$(targets)
  fi
  for name in $names; do
    local files=()
    for f in "$FUZZ/corpus/$name"/* "$FUZZ/regressions/$name"/*; do
      [ -f "$f" ] && files+=("$f")
    done
    if [ "${#files[@]}" -eq 0 ]; then
      echo "FAIL: $name has no committed input: $FUZZ/corpus/$name/ is empty"
      failed=1
      continue
    fi
    replay_files "$name" "${files[@]}" || failed=1
  done
  [ "$failed" -eq 0 ] || exit 1
  echo "PASS: every committed input replayed, under ASan and UBSan"
}

# A target's own libFuzzer flags: tests/fuzz/<name>.options, one a line, '#'
# for a comment -- -max_len, mostly, which keeps the merged corpus small.
options_of() {
  local file="$FUZZ/$1.options" line
  [ -f "$file" ] || return 0
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in '' | '#'*) continue ;; -*) echo "$line" ;;
      *) echo "FAIL: $file: '$line' is not a libFuzzer flag" >&2; return 1 ;;
    esac
  done < "$file"
}

need_libfuzzer() {
  require_built
  if [ "$(engine)" != libfuzzer ]; then
    echo "FAIL: $BUILD was built to replay, not to fuzz: there is no libFuzzer here."
    echo "      The pinned image has it: tools/dev_shell.sh bash tools/fuzz.sh build"
    exit 1
  fi
}

run() {
  [ $# -eq 2 ] || usage
  local name="$1" seconds="$2"
  known_target "$name"
  need_libfuzzer
  case "$seconds" in '' | *[!0-9]*) echo "FAIL: SECONDS is a whole number, not '$seconds'"; exit 2 ;; esac
  mkdir -p "$BUILD/new/$name" "$BUILD/crashes/$name"
  local args=(-max_total_time="$seconds" -timeout=10 -rss_limit_mb=1024 -print_final_stats=1
              -artifact_prefix="$BUILD/crashes/$name/") flag own
  own=$(options_of "$name") || exit 1
  for flag in $own; do args+=("$flag"); done
  if [ -f "$FUZZ/$name.dict" ]; then args+=(-dict="$FUZZ/$name.dict"); fi
  if [ -n "${RMP_FUZZ_SEED:-}" ]; then args+=(-seed="$RMP_FUZZ_SEED"); fi
  echo "== fuzz: $name for ${seconds}s =="
  # The first folder takes what is new; the committed ones are read.
  "$BUILD/fuzz_$name" "${args[@]}" "$BUILD/new/$name" "$FUZZ/corpus/$name" \
    "$FUZZ/regressions/$name" < /dev/null
}

merge() {
  [ $# -eq 1 ] || usage
  local name="$1"
  known_target "$name"
  need_libfuzzer
  mkdir -p "$BUILD/new/$name" "$FUZZ/corpus/$name"
  local args=(-merge=1 -timeout=10 -rss_limit_mb=1024) flag own
  own=$(options_of "$name") || exit 1
  for flag in $own; do args+=("$flag"); done
  echo "== fuzz: merge $name =="
  "$BUILD/fuzz_$name" "${args[@]}" "$FUZZ/corpus/$name" "$BUILD/new/$name" < /dev/null
  echo "  $FUZZ/corpus/$name: $(find "$FUZZ/corpus/$name" -type f | wc -l | tr -d ' ') file(s)"
}

[ $# -ge 1 ] || usage
command="$1"
shift
case "$command" in
  build) [ $# -eq 0 ] || usage; build ;;
  replay) replay "$@" ;;
  run) run "$@" ;;
  merge) merge "$@" ;;
  targets) [ $# -eq 0 ] || usage; targets ;;
  *) usage ;;
esac
