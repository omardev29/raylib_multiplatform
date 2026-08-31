#!/usr/bin/env bash
# Fail if a file under src/rmp/ reads the clock, the input devices or the global
# random state on its own.
#
# WHY THIS EXISTS. Every headless test in this repository is possible because
# the things that change between two runs — time, input, randomness — enter
# through a seam that a test can replace. A single new `GetFrameTime()` in the
# wrong file is enough to make a test flaky, and a flaky test teaches everyone
# to ignore the colour red. This is that rule, enforced.
#
# HOW IT IS ENFORCED. Not as "zero occurrences", because that would be a lie
# today: the UI layer still reads several of these directly, and routing them
# through the seam is phase 5's work, not something to fake now. So this is a
# RATCHET. The files below are the ones that already do it; they are debt, they
# are listed, and the list may only get shorter. Any OTHER file that starts
# doing it fails the build.
#
# It also checks a SECOND rule, added after it cost a link error: inside rmp::,
# a raylib type whose name we have taken must be written with a leading `::`.
#
# Usage: tools/seam_check.sh          (from the repo root)

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The calls that make a run differ from the next one.
PATTERN='GetFrameTime|GetTime\(\)|GetMousePosition|GetMouseWheelMove|GetTouchPosition|IsKeyDown|IsKeyPressed|IsMouseButtonDown|IsMouseButtonPressed|GetRandomValue|SetRandomSeed'

# Files allowed to call them, and why. Two kinds:
#
#   THE SEAM ITSELF — these are the functions a test replaces. They have to
#   touch the real thing; that is their job.
#   DEBT — the UI reads input and the clock directly here. It works because the
#   layout test forces test mode, but it is not the seam and it is why the UI
#   cannot be tested for behaviour, only for layout. Phase 5 (rmp::input) is
#   where these move, and this list is how we notice if they do not.
ALLOWED=(
  "src/rmp/ui/context.cpp"    # the seam: read_pointer(), behind a provider
  "src/rmp/ui/style.cpp"      # the seam: anim_begin_frame(), 0 in test mode
  "src/rmp/random.cpp"        # names GetRandomValue in a comment, explaining why not
  "src/rmp/ui/focus.cpp"      # DEBT: keyboard and gamepad navigation -> phase 5
  "src/rmp/ui/controls.cpp"   # DEBT: slider repeat and the caret blink -> phase 5
  "src/rmp/ui/widgets.cpp"    # DEBT: scroll wheel and Clay's frame time -> phase 5
)

fails=0
found_any=0
while IFS= read -r file; do
  found_any=1
  allowed=0
  for ok in "${ALLOWED[@]}"; do
    if [ "$file" = "$ok" ]; then allowed=1; break; fi
  done
  if [ "$allowed" -eq 0 ]; then
    echo "  FAIL  $file reads time, input or randomness directly:"
    grep -nE "$PATTERN" "$file" | sed 's/^/          /'
    fails=$((fails + 1))
  fi
done < <(grep -rlE "$PATTERN" src/rmp --include='*.cpp' --include='*.h' | sort)

if [ "$found_any" -eq 0 ]; then
  echo "  ok    nothing under src/rmp/ reads time, input or randomness"
  exit 0
fi

# The ratchet's other half: an entry that no longer matches is an entry to
# delete, or the list stops meaning anything.
for ok in "${ALLOWED[@]}"; do
  if [ ! -f "$ok" ]; then
    echo "  FAIL  the allowlist names $ok, which does not exist. Remove the entry."
    fails=$((fails + 1))
  elif ! grep -qE "$PATTERN" "$ok"; then
    echo "  FAIL  $ok is on the allowlist but no longer needs to be. Remove the entry —"
    echo "        the list may only get shorter."
    fails=$((fails + 1))
  fi
done

if [ "$fails" -ne 0 ]; then
  echo
  echo "FALLA: $fails file(s). Time, input and randomness enter through a seam so that"
  echo "       tests can replace them. Use the delta you were given, rmp::input, or"
  echo "       rmp::random — see next_architecture/12-testing.md."
  exit 1
fi
echo "  ok    the seam holds (${#ALLOWED[@]} known exceptions, all still needed)"

# --- rule two: shadowed raylib types ---------------------------------------
#
# rmp::Image, rmp::Font, rmp::Sound, rmp::Music and rmp::Shader are counted
# handles that TAKE THE NAME of a raylib struct. Inside namespace rmp, the
# unqualified name therefore means ours — and which one a header means depends
# on whether that translation unit happened to include rmp/assets.h.
#
# That is not a style question. `Image pack_read_image(const char *)` in a
# shared internal header meant rmp::Image in one .cpp and ::Image in another:
# two different functions, one missing symbol, and it linked on thirteen of the
# fourteen targets. Windows ARM64 was the one that noticed.
SHADOWED='Image|Font|Sound|Music|Shader'
shadow_fails=0
while IFS= read -r hit; do
  [ -z "$hit" ] && continue
  echo "  FAIL  a shadowed raylib type is written without ::"
  echo "          $hit"
  shadow_fails=$((shadow_fails + 1))
done < <(grep -rnE "(^|[^:_[:alnum:]])($SHADOWED) +\*?[a-zA-Z_][a-zA-Z_0-9]*" \
           src/rmp --include='*.cpp' --include='*.h' \
         | grep -vE "::($SHADOWED)|rmp::|^[^:]*:[0-9]+: *(//|\*)" || true)

if [ "$shadow_fails" -ne 0 ]; then
  echo
  echo "FALLA: inside rmp::, write ::Image, ::Font, ::Sound, ::Music or ::Shader when you"
  echo "       mean raylib's. The unqualified name is ours, and which one a header means"
  echo "       depends on what the .cpp including it happened to include first."
  exit 1
fi
echo "  ok    no shadowed raylib type is written without ::"
