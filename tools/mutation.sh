#!/usr/bin/env bash
# Mutation testing: when the code is made wrong on purpose, does a test fail?
#
# Coverage says which lines ran; this says whether anything checked what they
# did. Mull (C++) and mutmut (Python) change one operator, constant or call at
# a time -- a mutant -- and run the module's tests against each. A mutant no
# test notices is a test nobody wrote, or an equivalent change nobody can see.
#
# Each module is a floor file, tests/mutation/floors/<module>.txt: what is
# mutated, which tests must notice, the score that has to hold and the
# equivalent mutants with their reasons. tools/mutation_report.py reads what
# the tool wrote -- Mull's Elements JSON, mutmut's .meta files, never what
# they printed -- and holds it to that file. See it for the rules.
#
#   bash tools/mutation.sh modules            every module, as "<kind> <name>"
#   bash tools/mutation.sh run MODULE         mutate it, run its tests, check its floor
#   bash tools/mutation.sh check MODULE       the last run's report against the floor again
#   bash tools/mutation.sh update MODULE      the same, raising the floor to what it earned
#   bash tools/mutation.sh all                every module, one after another (hours)
#
# `rmp test mutation` is `all`. .github/workflows/deep.yml runs one module a
# job, every week.
#
# C++: Mull 0.34.1 for LLVM 18, in the pinned build image only -- its IR
# plugin has to match the clang that compiles, exactly. A build of its own per
# module (build/mutation/cxx-<module>), Debug and without the sanitizers, with
# the plugin mutating the module's sources and nothing else; mull-runner then
# runs each test program of the floor file once per mutant, filtered by
# doctest's -sf/-ts as the file says. Killed by any program is killed.
#
# Python: mutmut 3, on a copy of the tree in build/mutation/py-<module>/ --
# mutmut writes a mutants/ copy next to its configuration, and the tests read
# the repository through their own path, so the copy is what they read: the
# mutated file, not the checkout's. Code a test reaches only through a
# subprocess counts as reached by none (mutmut follows calls in-process), so a
# floor file selects what its tests call directly.
#
# ASCII only and no GNU-only constructs: tools/portable_check.sh covers this
# directory, and macOS ships bash 3.2.
set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD="build/mutation"
FLOORS="tests/mutation/floors"

report() { python3 tools/mutation_report.py "$@"; }

# Every program a mutant runs in is held to an address space of its own: a
# mutant that takes a bound away -- a loop that never stops growing a vector --
# otherwise takes the machine with it. On GitHub's runner the core and console
# shards of the weekly run lost their runner that way ("The hosted runner lost
# communication with the server", 38173980700), with no log and no score. Under
# the limit the mutant's program dies, which is a mutant detected. Per process:
# mull-runner's workers and mutmut's children are each held to it, and the
# builds are not sanitized (ASan reserves terabytes of address space).
MUTANT_MEMORY_KB=${RMP_MUTANT_MEMORY_KB:-3145728}  # 3 GiB
bounded() { (ulimit -v "$MUTANT_MEMORY_KB" && "$@"); }

usage() {
  echo "usage: bash tools/mutation.sh modules | run MODULE | check MODULE | update MODULE | all"
  exit 2
}

modules() {
  local f
  for f in "$FLOORS"/*.txt; do
    [ -e "$f" ] || continue
    echo "$(report "$(basename "$f" .txt)" --print kind) $(basename "$f" .txt)"
  done
}

known() {
  if [ ! -f "$FLOORS/$1.txt" ]; then
    echo "FAIL: no module called '$1': there is no $FLOORS/$1.txt"
    exit 2
  fi
}

# The reports of the last run of MODULE, as tools/mutation_report.py's
# arguments: one --mull per test program, or one --mutmut per source.
reports_of() {
  local module="$1" kind f
  kind=$(report "$module" --print kind)
  if [ "$kind" = cxx ]; then
    for f in "$BUILD/cxx-$module/report/$module"-*.json; do
      [ -f "$f" ] && printf -- '--mull\n%s\n' "$f"
    done
  else
    while IFS= read -r f; do
      [ -n "$f" ] && printf -- '--mutmut\n%s\n' "$BUILD/py-$module/mutants/$f.meta"
    done < <(report "$module" --print sources)
  fi
}

check() {
  local module="$1" update="${2:-}" args=() line
  while IFS= read -r line; do args+=("$line"); done < <(reports_of "$module")
  if [ "${#args[@]}" -eq 0 ]; then
    echo "FAIL: $module has no report: bash tools/mutation.sh run $module"
    return 1
  fi
  report "$module" "${args[@]}" ${update:+"$update"}
}

# --- C++: Mull ------------------------------------------------------------------
mull_tools() {
  local runner major
  runner=$(command -v mull-runner-18 || command -v mull-runner || true)
  if [ -z "$runner" ]; then
    echo "FAIL: no mull-runner here. Mull 0.34.1 for LLVM 18 is in the pinned build image:"
    echo "        tools/dev_shell.sh bash tools/mutation.sh run <module>"
    return 1
  fi
  major=$(clang++ -dumpversion 2>/dev/null | cut -d. -f1 || true)
  MULL_RUNNER="$runner"
  MULL_PLUGIN="/usr/lib/mull-ir-frontend-${major}"
  if [ ! -f "$MULL_PLUGIN" ]; then
    echo "FAIL: $MULL_PLUGIN is not here: Mull's IR plugin has to be the one for clang $major."
    return 1
  fi
}

run_cxx() {
  local module="$1" dir="$BUILD/cxx-$module" source regex programs=() line program
  mull_tools
  mkdir -p "$dir/report"
  # Only the module's sources are mutated: the plugin reads this when it compiles.
  {
    echo "mutators:"
    echo "  - cxx_all"
    echo "includePaths:"
    while IFS= read -r source; do
      regex=$(printf '%s' "$source" | sed 's/[.]/\\./g')
      echo "  - .*/$regex"
    done < <(report "$module" --print sources)
  } > "$dir/mull.yml"
  while IFS= read -r line; do programs+=("$line"); done < <(report "$module" --print tests)
  local targets=() ui=OFF
  for line in "${programs[@]}"; do
    program=${line%% *}
    targets+=("$program")
    if [ "$program" = ui_layout_test ]; then ui=ON; fi
  done
  echo "== mutation: build $module ($MULL_RUNNER) =="
  MULL_CONFIG="$PWD/$dir/mull.yml" cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DPRODUCTION_BUILD=OFF -DBUILD_TESTS=ON -DBUILD_UI_TESTS="$ui" -DPLATFORM=Memory \
        -DRMP_SANITIZE=OFF -DRMP_DEV_TOOLCHAIN=OFF -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        "-DCMAKE_CXX_FLAGS=-fpass-plugin=$MULL_PLUGIN -g -grecord-command-line" \
        > "$dir.configure.log" 2>&1 \
    || { tail -40 "$dir.configure.log"; echo "FAIL: $module's mutation build did not configure"; exit 1; }
  MULL_CONFIG="$PWD/$dir/mull.yml" cmake --build "$dir" --target "${targets[@]}" \
        > "$dir.build.log" 2>&1 \
    || { tail -40 "$dir.build.log"; echo "FAIL: $module's mutation build did not build"; exit 1; }
  rm -f "$dir/report/$module"-*.json
  local i=0
  for line in "${programs[@]}"; do
    i=$((i + 1))
    # shellcheck disable=SC2086  # the program's arguments are words on purpose
    set -- $line
    program="$1"
    shift
    echo "== mutation: $module, $program $* =="
    # --timeout bounds the warm-up run, the unmutated program (mull-runner's
    # default is 3 s, and property_test takes 5 at Mull's -O0: "Original test
    # failed (warmup run)"); each mutant's own limit stays the larger of ten
    # warm-ups and --minimum-timeout.
    bounded "$MULL_RUNNER" --reporters Elements --report-dir "$dir/report" --report-name "$module-$i" \
      --allow-surviving --workers "${CMAKE_BUILD_PARALLEL_LEVEL:-4}" --timeout 120000 --minimum-timeout 10000 \
      "$dir/$program" "$@" < /dev/null > "$dir/report/$module-$i.log" 2>&1 \
      || { tail -30 "$dir/report/$module-$i.log"; echo "FAIL: mull-runner did not finish"; exit 1; }
    grep -E 'Surviving mutants|Mutation score|Total execution' "$dir/report/$module-$i.log" \
      | sed 's/^/  /' || true
    [ -s "$dir/report/$module-$i.json" ] || { echo "FAIL: mull-runner wrote no report"; exit 1; }
  done
}

# --- Python: mutmut -------------------------------------------------------------------
run_py() {
  local module="$1" dir="$BUILD/py-$module" sources tests selects entry
  if ! command -v mutmut >/dev/null 2>&1; then
    echo "FAIL: no mutmut here. mutmut 3.8.0 is in the pinned build image:"
    echo "        tools/dev_shell.sh bash tools/mutation.sh run $module"
    exit 1
  fi
  # What git tracks, and nothing a build or an earlier run left: the tests
  # read the repository, and an old build/ under the copy is not part of it.
  if ! git ls-files --error-unmatch tools/mutation.sh > /dev/null 2>&1; then
    echo "FAIL: mutmut works on a copy of what git tracks, and git cannot read this"
    echo "      checkout (a worktree whose .git is not here, or a folder git does not trust:"
    echo "      git config --global --add safe.directory \"\$PWD\")."
    exit 1
  fi
  echo "== mutation: copy the tree for $module =="
  rm -rf "$dir"
  mkdir -p "$dir"
  git ls-files -z | tar --null -T - -cf - | tar -xf - -C "$dir"
  (cd "$dir"; python3 tools/configure.py > /dev/null)
  # mutmut keeps the mutant under test in its own process and mirrors it into
  # MUTANT_UNDER_TEST, which every program a test starts inherits. A tool a
  # test starts as a program, in a folder of its own -- MutationScriptTest
  # runs tools/mutation.sh so -- then went looking for mutmut's setup.cfg in
  # that folder while the stats were collected, and the run died there
  # ("failed to collect stats", every mutant without a verdict). Started as
  # a program its functions are __main__'s, which no mutant names, so without
  # the variable it runs exactly what it ran in every other phase.
  cat > "$dir/conftest.py" <<'PY'
# Written by tools/mutation.sh, in mutmut's copy of the tree only.
import os

import pytest


@pytest.hookimpl(tryfirst=True)
def pytest_runtest_setup(item):
    os.environ.pop("MUTANT_UNDER_TEST", None)
PY
  sources=$(report "$module" --print sources)
  tests=$(report "$module" --print tests)
  selects=$(report "$module" --print select)
  {
    echo "[mutmut]"
    echo "source_paths ="
    # shellcheck disable=SC2086  # one path a word
    printf '    %s\n' $sources
    echo "pytest_add_cli_args_test_selection ="
    # shellcheck disable=SC2086
    printf '    %s\n' $tests
    echo "pytest_add_cli_args ="
    printf '    %s\n' -q -p no:cacheprovider
    # Everything the tests read: they find the repository from their own path,
    # which inside mutants/ is the copy.
    echo "also_copy ="
    for entry in $(cd "$dir" && ls -A); do
      case "$entry" in mutants | setup.cfg) continue ;; esac
      echo "    $entry"
    done
  } > "$dir/setup.cfg"
  echo "== mutation: mutmut over $sources =="
  # Its exit status is not the verdict -- the .meta files it wrote are, and
  # mutation_report.py fails a mutant it never got to. It is printed.
  local status=0
  # shellcheck disable=SC2086  # selects are words on purpose, and none means all
  (cd "$dir"; bounded mutmut run --max-children "${CMAKE_BUILD_PARALLEL_LEVEL:-4}" $selects < /dev/null) \
    > "$dir.log" 2>&1 || status=$?
  tail -5 "$dir.log" | sed 's/^/  /'
  echo "  mutmut exited $status"
}

run() {
  local module="$1" kind
  known "$module"
  kind=$(report "$module" --print kind)
  if [ "$kind" = cxx ]; then run_cxx "$module"; else run_py "$module"; fi
  check "$module"
}

[ $# -ge 1 ] || usage
command="$1"
shift
case "$command" in
  modules) [ $# -eq 0 ] || usage; modules ;;
  run) [ $# -eq 1 ] || usage; run "$1" ;;
  check) [ $# -eq 1 ] || usage; known "$1"; check "$1" ;;
  update) [ $# -eq 1 ] || usage; known "$1"; check "$1" --update ;;
  all)
    [ $# -eq 0 ] || usage
    failed=""
    for module in $(modules | cut -d' ' -f2); do
      # A subshell each: a module whose build fails exits its own, not the loop.
      ( run "$module" ) || failed="$failed $module"
    done
    if [ -n "$failed" ]; then echo "FAIL: below or above their floors:$failed"; exit 1; fi
    echo "PASS: every module at its floor"
    ;;
  *) usage ;;
esac
