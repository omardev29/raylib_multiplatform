#!/usr/bin/env bash
# clang-tidy over src/, tests/ and examples/ -- the same run on a laptop and in
# the CI lint job, which both call this.
#
# The compile database is build/lint/, from the configure-only `lint` preset:
# tests and examples ON, nothing built. A separate directory, because turning
# RMP_BUILD_EXAMPLES on in build/ would make every `rmp test` after it compile
# twenty examples, and the debug and release presets already share build/.
#
# Every file linted must have a real entry in that database. Without one,
# clang-tidy GUESSES the flags from a neighbouring entry -- which guessed
# differently on a laptop and on the runner: green here, `clay.h file not found`
# there. So a file with no entry is a failure, not a guess.
#
# examples/ has its own .clang-tidy: the framework's names and every check that
# finds a bug, without the modernize/readability checks that would push a more
# elaborate spelling of correct code at someone learning raylib.
#
# Usage: tools/lint.sh [check|fix]

set -euo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mode=${1:-check}
case "$mode" in
    check | fix) ;;
    *) echo "unknown: $mode (check | fix)"; exit 1 ;;
esac

cmake --preset lint > /dev/null

# *_impl.cpp exist to compile a vendored header once, and unit_test.cpp to host
# doctest: its 9 000 lines of macros produce analyzer diagnostics that are not
# ours to fix, and its own logic is assertions. clang-format still covers them.
files=()
while IFS= read -r f; do files+=("$f"); done < <(
    find src tests examples -name '*.cpp' | grep -v _impl | grep -v 'tests/unit_test\.cpp' | sort)

# And the C, which this skipped for as long as it only looked for .cpp: the
# packer every desktop release runs (tools/rres_pack.c, tools/md5.c), the plain
# C example, and the sanitizer defaults linked into every Debug executable.
# The static analyzer had never read any of them, and tools/rres_pack.c had
# NULL dereferences waiting for the first allocation that failed.
# tools/.clang-tidy says what changes for the two tools. cjson_impl.c compiles
# a vendored library, like the *_impl.cpp above, and tests/fixtures/ holds
# files that are data for a test, not programs.
while IFS= read -r f; do files+=("$f"); done < <(
    find src tests examples tools cmake -name '*.c' | grep -v _impl | grep -v generated |
        grep -v '^tests/fixtures/' | sort)

python3 - "${files[@]}" <<'PY'
import json
import pathlib
import sys

db = json.loads(pathlib.Path("build/lint/compile_commands.json").read_text())
known = {str(pathlib.Path(e["directory"], e["file"]).resolve()) for e in db}
missing = [f for f in sys.argv[1:] if str(pathlib.Path(f).resolve()) not in known]
if missing:
    print("FAIL: no compile command for these, so clang-tidy would guess their flags:")
    for f in missing:
        print(f"  {f}")
    sys.exit(1)
print(f"  ok    {len(sys.argv) - 1} file(s), every one in build/lint/compile_commands.json")
PY

jobs=$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 2)
if [ "$mode" = fix ]; then
    # One process per file: two fixes landing in the same header from two
    # processes at once would corrupt it.
    for f in "${files[@]}"; do clang-tidy -p build/lint --quiet --fix --fix-errors "$f"; done
    exit 0
fi

# xargs exits non-zero when any one invocation did, which is the whole verdict.
if printf '%s\n' "${files[@]}" | xargs -P "$jobs" -n 4 clang-tidy -p build/lint --quiet --warnings-as-errors='*'; then
    echo "  ok    no warnings"
else
    echo "FAIL: clang-tidy has warnings above"
    exit 1
fi
