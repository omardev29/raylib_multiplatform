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
#
# It scans src/rmp AND include/rmp. It used to scan src/rmp only, and
# include/rmp/app.h carries the RMP_ENTRY_POINT / RMP_GAME macro bodies and the
# runner functions -- inline code that compiles into EVERY translation unit and
# was invisible to this.
#
# The list grew with it. GetScreenWidth/GetScreenHeight are here because the
# viewport is a thing a headless test has to be able to set; the gamepad, the
# character queue and the *Released half of the key and mouse API are here
# because they are the same seam as their *Down and *Pressed twins and were
# simply missed; IsWindowReady because "is there a window" is the question
# every headless path has to be able to answer without one.
PATTERN='GetFrameTime|GetTime\(\)|GetMousePosition|GetMouseWheelMove|GetTouch[A-Za-z]*|IsKeyDown|IsKeyPressed|IsKeyReleased|IsMouseButtonDown|IsMouseButtonPressed|IsMouseButtonReleased|GetRandomValue|SetRandomSeed|GetScreenWidth|GetScreenHeight|GetGamepad[A-Za-z]*|IsGamepadButton[A-Za-z]*|GetCharPressed|IsWindowReady'

# Comments do not count. Naming a call in the note that explains why it is not
# used is not using it, and the alternative is that the explanation cannot be
# written next to the code it explains -- four public headers and random.cpp
# do exactly that, and before this they were hits. `// ...` to end of line and
# a leading `*` continuation line are stripped; line numbers survive, because
# sed substitutes rather than deletes.
strip_comments() { sed -e 's|//.*||' -e 's|^[[:space:]]*\*.*||' -- "$1"; }

# `grep -c` and not `grep -q`, and it is not a style choice: `grep -q` exits the
# moment it matches, sed on the other side of the pipe takes SIGPIPE, and with
# `set -o pipefail` the PIPELINE then reports 141 even though the match
# succeeded. Whether it happens depends on whether sed had finished writing --
# so the check passed on small files and failed on large ones, which is the
# worst kind of intermittent. -c reads all of its input.
count_hits() { strip_comments "$1" | grep -cE "$PATTERN"; }

# Files allowed to call them, and why. Two kinds:
#
#   THE SEAM ITSELF — these are the functions a test replaces. They have to
#   touch the real thing; that is their job.
#   DEBT — the UI reads input and the clock directly here. It works because the
#   layout test forces test mode, but it is not the seam and it is why the UI
#   cannot be tested for behaviour, only for layout. Phase 5 (rmp::input) is
#   where these move, and this list is how we notice if they do not.
ALLOWED=(
  "src/rmp/app.cpp"           # THE seam for time: step_delta() is the single
                             # GetFrameTime() in the whole framework, and every
                             # runner in rmp/app.h goes through it so that
                             # [app] max_delta applies to all of them. Nothing
                             # downstream reads the clock -- they are handed a
                             # delta, which is what makes the object and scene
                             # tests able to run a frame of exactly 1/30.
  "src/rmp/input.cpp"         # THE seam: sample_with_raylib() is the provider a
                             # test replaces, and the only place the devices are
                             # read at all. Everything else asks rmp::input.
  "src/rmp/ui/context.cpp"    # the seam: read_pointer() and frame_time(), and
                             # the frame boundary that samples both once
  "src/rmp/ui/style.cpp"      # the seam: anim_begin_frame(), 0 in test mode
  "src/rmp/object.cpp"        # THE seam for the viewport: view_rect() falls back
                             # to the screen for an object with empty bounds, and
                             # GetScreenWidth()/GetScreenHeight() are what "the
                             # screen" means. It is two calls in one function and
                             # it returns 0 with no window, which the callers
                             # already handle -- see the note at the site.
  "src/rmp/scene.cpp"         # THE seam for "is there a window": one
                             # IsWindowReady() guard so the scene stack can run
                             # its transitions headless. This is the call the
                             # RAY_TEST_BOOT_OK gate was missing, not one to
                             # route away.
  "src/rmp/ui/focus.cpp"      # DEBT: keyboard and gamepad navigation -> phase 14
  "src/rmp/ui/controls.cpp"   # DEBT: slider repeat, the text caret and the
                             # character queue -> phase 14
)

# src/rmp/random.cpp was on this list for naming GetRandomValue in a COMMENT.
# Comments are stripped now, so it stopped matching and the ratchet's other
# half asked for the entry back. That is the list working in both directions.

# The two DEBT entries said "-> phase 5" until phase 5 arrived and did not take
# them. That was the honest outcome rather than a slip: rmp::input samples
# devices for the GAME, and the UI's focus navigation and caret blink are the
# UI reading input for itself, one layer below where actions live. Moving them
# means giving rmp::ui a dependency on rmp::input, which is the wrong direction
# — input asks the UI whether it wants the pointer, not the other way round.
# They move in phase 14, with the rest of the UI's own input handling.

# widgets.cpp was on this list until phase 4. Moving the scroll wheel and the
# frame time out of begin() and into the frame boundary took its last two
# direct reads with them, and the ratchet is what noticed — it failed the build
# asking for this entry to be deleted. That is the list working: it shrank.

fails=0
found_any=0
scanned=0

# A scan that found no FILES is not a clean tree, it is a broken scan -- and it
# prints the same "ok" as a clean tree. Renaming a directory was enough to do
# it: `find src/rmp` writes to stderr and carries on, and the loop below then
# runs zero times.
if [ "$(find src/rmp include/rmp \( -name '*.cpp' -o -name '*.h' \) 2>/dev/null | grep -c .)" -lt 10 ]; then
  echo "  FAIL  fewer than 10 sources found under src/rmp/ and include/rmp/."
  echo "        Something moved; this check was about to pass by scanning nothing."
  exit 1
fi

while IFS= read -r file; do
  scanned=$((scanned + 1))
  if [ "$(count_hits "$file")" -eq 0 ]; then continue; fi
  found_any=1
  allowed=0
  for ok in "${ALLOWED[@]}"; do
    if [ "$file" = "$ok" ]; then allowed=1; break; fi
  done
  if [ "$allowed" -eq 0 ]; then
    echo "  FAIL  $file reads time, input, randomness or the screen directly:"
    strip_comments "$file" | grep -nE "$PATTERN" | sed 's/^/          /'
    fails=$((fails + 1))
  fi
done < <(find src/rmp include/rmp \( -name '*.cpp' -o -name '*.h' \) | sort)

if [ "$found_any" -eq 0 ]; then
  echo "  ok    nothing under src/rmp/ or include/rmp/ reads time, input or randomness"
  exit 0
fi

# The ratchet's other half: an entry that no longer matches is an entry to
# delete, or the list stops meaning anything.
for ok in "${ALLOWED[@]}"; do
  if [ ! -f "$ok" ]; then
    echo "  FAIL  the allowlist names $ok, which does not exist. Remove the entry."
    fails=$((fails + 1))
  elif [ "$(count_hits "$ok")" -eq 0 ]; then
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
echo "  ok    the seam holds in $scanned file(s) (${#ALLOWED[@]} known exceptions, all still needed)"

# --- rule two: shadowed raylib types ---------------------------------------
#
# rmp::Image, rmp::Font, rmp::Sound, rmp::Music and rmp::Shader are counted
# handles that TAKE THE NAME of a raylib struct. Inside namespace rmp, the
# unqualified name therefore means ours — and which one a header means depends
# on whether that translation unit happened to include rmp/assets.h.
#
# That is not a style question. `Image pack_read_image(const char *)` in a
# shared internal header meant rmp::Image in one .cpp and ::Image in another:
# two different functions, one missing symbol, and it linked on sixteen of the
# seventeen targets. Windows ARM64 was the one that noticed.
#
# include/rmp/ is scanned as well as src/rmp/, for the same reason rule one is:
# a declaration in a public header is exactly where this bites, because the
# header is what the two .cpp files disagree about.
SHADOWED='Image|Font|Sound|Music|Shader'
shadow_fails=0
while IFS= read -r hit; do
  [ -z "$hit" ] && continue
  echo "  FAIL  a shadowed raylib type is written without ::"
  echo "          $hit"
  shadow_fails=$((shadow_fails + 1))
done < <(grep -rnE "(^|[^:_[:alnum:]])($SHADOWED) +\*?[a-zA-Z_][a-zA-Z_0-9]*" \
           src/rmp include/rmp --include='*.cpp' --include='*.h' \
         | grep -vE "::($SHADOWED)|rmp::|^[^:]*:[0-9]+: *(//|\*)" || true)

if [ "$shadow_fails" -ne 0 ]; then
  echo
  echo "FALLA: inside rmp::, write ::Image, ::Font, ::Sound, ::Music or ::Shader when you"
  echo "       mean raylib's. The unqualified name is ours, and which one a header means"
  echo "       depends on what the .cpp including it happened to include first."
  exit 1
fi
echo "  ok    no shadowed raylib type is written without ::"
