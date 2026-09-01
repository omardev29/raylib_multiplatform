#!/usr/bin/env bash
# examples/ is documentation with code in it, and -fsyntax-only cannot read it.
#
# Three examples kept their own `int main()` with `while (!WindowShouldClose())`
# long after RMP_ENTRY_POINT and RMP_GAME replaced it. They compiled -- that
# loop is perfectly good C++ -- so the CI job that compiles every example said
# `ok` on all three for months. What it could not say is that the pattern they
# taught DOES NOT WORK on two of the sixteen targets: the browser owns the frame
# loop and calls you back through emscripten_set_main_loop, and iOS never calls
# main() at all. A newcomer copying the file a folder called examples/ handed
# them would have found that out on the first web build.
#
# So this checks the shape rather than the syntax. It is the same argument as
# tools/portable_check.sh: the compiler here is the wrong instrument, because
# everything it is being shown is valid.
#
# ASCII only and no GNU-only constructs -- tools/portable_check.sh covers this
# directory, and macOS ships bash 3.2.
set -euo pipefail

cd "$(dirname "$0")/.."

# The one file allowed to write its own main() and its own loop: it IS the
# opt-out, and its header comment says in full which two targets it therefore
# does not run on.
OPT_OUT="examples/plain_c/main.c"

FAILED=0

fail() {
  echo "FALLA: $1"
  echo "       $2"
  FAILED=1
}

for f in $(find examples -name '*.cpp' | sort); do
  if grep -qE '^[[:space:]]*int[[:space:]]+main[[:space:]]*\(' "$f"; then
    fail "$f writes its own int main()." \
         "Use RMP_ENTRY_POINT(on_ready, on_frame, on_exit) or RMP_GAME(Scene). It is the same three hooks, and it is what makes the example true on iOS and Web."
  fi
  if grep -q 'WindowShouldClose' "$f"; then
    fail "$f drives its own frame loop with WindowShouldClose()." \
         "The browser owns the loop (emscripten_set_main_loop) and a while loop there needs -s ASYNCIFY, which this project does not build with. RMP_ENTRY_POINT writes the right loop per target."
  fi
  # An on_frame that takes no delta is the signature from before the entry
  # point passed one, and it does not match RMP_ENTRY_POINT.
  if grep -qE 'void[[:space:]]+on_frame[[:space:]]*\([[:space:]]*\)' "$f"; then
    fail "$f declares on_frame() with no delta." \
         "The hook is void on_frame(float delta) -- that is what the entry point calls."
  fi
done

# The opt-out has to keep saying what it costs, or it becomes the same trap
# with a licence.
for phrase in "emscripten_set_main_loop" "ASYNCIFY" "ios_ready"; do
  if ! grep -q "$phrase" "$OPT_OUT"; then
    fail "$OPT_OUT no longer mentions $phrase." \
         "It writes its own main(), so it is the one file that must say which targets that rules out and why. Do not delete that paragraph."
  fi
done

# Every example is reachable from the README, or nobody reads it.
for f in $(find examples -name '*.cpp' -o -name '*.c' | sort); do
  base=$(basename "$f")
  if ! grep -q "$base" examples/README.md; then
    fail "$base is not mentioned in examples/README.md." \
         "An example nobody is pointed at is a file that rots unread."
  fi
done

# And the other direction: the README must not point at a file that is gone.
# 01_lifecycle.cpp was deleted and its row would have stayed, which is a broken
# link in the first document a newcomer opens.
# Markdown link targets only -- `]( ... )`. Matching bare filenames anywhere in
# the prose finds "raylib.c" inside "raylib.com", which is a false alarm in a
# gate, and a gate that cries wolf gets switched off.
for name in $(grep -oE '\]\([^)]*\.(cpp|c)\)' examples/README.md | sed 's/^](//; s/)$//' | sort -u); do
  if [ ! -f "examples/$name" ]; then
    fail "examples/README.md points at $name, which does not exist." \
         "Delete the row, or restore the file."
  fi
done

[ "$FAILED" -eq 0 ] || exit 1
echo "  ok    $(find examples -name '*.cpp' | wc -l | tr -d ' ') examples, each using the framework's entry point"
