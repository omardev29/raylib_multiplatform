#!/usr/bin/env bash
# cppcheck over src/rmp: the second static analyser, beside clang-tidy's.
#
# It reads the code differently -- its own value-flow analysis, not clang's --
# and on its first run here it found what clang-tidy had let through: a struct
# nothing used, `if (a) x(); if (a) y();` written twice, an unsigned count
# tested for `<= 0`, and parameters that could be const where misc-const-
# correctness does not look (it reads no parameter at all).
#
# The rule is zero findings in src/rmp and in the headers its files include.
# A finding that is not a bug is suppressed INLINE, on the line before it, and
# says why after a `;`:
#
#     // cppcheck-suppress constParameterReference ; a hook's signature is the API's
#
# A suppression with no reason fails here, and so does one that no longer
# matches anything (cppcheck's unmatchedSuppression): an excuse cannot outlive
# what it excused. The few that cannot be inline are listed below by id and
# file, each with its reason; and an id suppressed everywhere is a check this
# repository decided the other way, or one clang-tidy already holds.
#
# It reads the compile database of the `lint` preset (build/lint, configure
# only), so it sees the -D and -I the build uses -- with -isystem read as -I,
# because cppcheck 2.13 ignores -isystem and would otherwise not see clay.h,
# raylib.h or cJSON's macros, and reason about code that is not there.
# __cplusplus is defined because cppcheck otherwise also explores the branch
# where it is not, and raylib.h's C `bool` makes C++ unparseable there.
#
# cppcheck ships in the build image. On a machine without it this says so and
# exits 0; RMP_REQUIRE_CPPCHECK=1, which CI sets, makes that a failure. The
# reasons are checked either way.
#
# Usage:
#   tools/cppcheck_check.sh            src/rmp, through the compile database
#   tools/cppcheck_check.sh FILE...    exactly those files, with the same flags
#                                      and include paths -- the tests' probes

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Suppressed everywhere, or for one file, and why. THE LIST ONLY SHRINKS.
SUPPRESS=(
  # What cppcheck says about itself, not about the code.
  "checkersReport"
  # The C library and the system headers: cppcheck knows the standard library
  # through its own configuration and is not given /usr/include.
  "missingIncludeSystem"
  # A raw loop that says what it does is the style here, and Google's too; an
  # algorithm for a five-line loop is not clearer.
  "useStlAlgorithm"
  # A local named like a function somewhere else (`data`, `row`): the naming
  # table makes both snake_case on purpose, and -Wshadow in clang-tidy is the
  # rule for a name that hides another.
  "shadowFunction"
  # google-explicit-constructor in clang-tidy holds this rule, and the API's
  # deliberate conversions are written down there.
  "noExplicitConstructor"
  # Clay's structs, passed by value the way Clay's own API passes them:
  # trivially copied, 32 bytes at most. clang-tidy's
  # performance-unnecessary-value-param holds the rule for a copy that costs.
  "passedByValue"
  "passedByValueCallback"
  # The vendored headers are read for their declarations; their own code is
  # frozen, and not ours to fix.
  "*:*thirdparty/*"
  # tests/smoke_test.h is C99 -- examples/plain_c compiles it -- and a C cast
  # is the language there.
  "cstyleCast:*tests/smoke_test.h"
  # Three false positives in public headers, which take no comment without a
  # review of the header; each would be an inline suppression there.
  #   spawn<T>(): `T &ref = *made` refers to the object the scene keeps, not
  #   to the local unique_ptr it came out of.
  "returnReference:*include/rmp/scene.h"
  #   Ref::operator*: stand_in<T>() returns a static, not a temporary.
  "returnTempReference:*include/rmp/object.h"
  #   Scene::camera: every member of rmp::Camera has its initialiser.
  "uninitMemberVar:*include/rmp/scene.h"
)

# Every suppression in our code says why. Text, so it runs with no cppcheck.
reasons_fail=0
while IFS= read -r hit; do
  echo "  FAIL  $hit"
  echo "        -- a cppcheck-suppress says why after a \`;\`: \`// cppcheck-suppress id ; why\`"
  reasons_fail=$((reasons_fail + 1))
done < <(
  if [ $# -gt 0 ]; then grep -nH 'cppcheck-suppress' "$@" 2> /dev/null; else
    grep -rnH --include='*.h' --include='*.cpp' --include='*.c' 'cppcheck-suppress' \
      include/rmp src/rmp tests 2> /dev/null; fi |
    grep -vE 'cppcheck-suppress[A-Za-z]*[[:space:]]+[A-Za-z0-9_,]+[[:space:]]*;[[:space:]]*[^[:space:]]+[[:space:]]+[^[:space:]]' ||
    true)
if [ "$reasons_fail" -ne 0 ]; then
  echo "FAIL: $reasons_fail cppcheck suppression(s) with no reason"
  exit 1
fi

if ! command -v cppcheck > /dev/null 2>&1; then
  if [ "${RMP_REQUIRE_CPPCHECK:-}" = 1 ]; then
    echo "FAIL: cppcheck is not on PATH, and this job requires it (RMP_REQUIRE_CPPCHECK=1)."
    echo "      It ships in the build image, from the same apt snapshot as everything"
    echo "      else; if this fired in CI, the image digest predates it or it drifted."
    exit 1
  fi
  echo "  skip  cppcheck is not installed (the build image has it;"
  echo "        RMP_REQUIRE_CPPCHECK=1 makes this a failure)"
  exit 0
fi
cppcheck --version

jobs=$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 2)
flags=(--std=c++20 -D__cplusplus=202002L --inline-suppr -q "-j$jobs"
       --enable=warning,style,performance,portability,information
       --template='{file}:{line}:{column}: {severity}: {message} [{id}]')
for s in "${SUPPRESS[@]}"; do flags+=("--suppress=$s"); done

out=$(mktemp)
trap 'rm -f "$out"' EXIT
if [ $# -gt 0 ]; then
  # The include paths of the `rmp` target, as -I.
  cppcheck "${flags[@]}" --language=c++ -Iinclude -Ithirdparty -Ithirdparty/raylib/src \
    -Ithirdparty/clay -Ithirdparty/cJSON -Ithirdparty/rres -Ithirdparty/cute_tiled \
    -Ithirdparty/cute_aseprite "$@" 2> "$out" > /dev/null
  status=$?
else
  if ! cmake --preset lint > "$out" 2>&1; then
    cat "$out"
    echo "FAIL: cmake --preset lint"
    exit 1
  fi
  python3 - <<'PY' || exit 1
import json
import pathlib
db = json.loads(pathlib.Path("build/lint/compile_commands.json").read_text())
for entry in db:
    if "command" in entry:
        entry["command"] = entry["command"].replace(" -isystem ", " -I")
    else:
        entry["arguments"] = ["-I" if a == "-isystem" else a for a in entry["arguments"]]
pathlib.Path("build/lint/cppcheck_commands.json").write_text(json.dumps(db))
PY
  cppcheck "${flags[@]}" --project=build/lint/cppcheck_commands.json --file-filter='src/rmp/*' \
    2> "$out" > /dev/null
  status=$?
fi

python3 - "$out" "$status" "$#" <<'PY'
import re
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
status = int(sys.argv[2])
files_given = sys.argv[3] != "0"
FINDING = re.compile(r"^(?P<file>.*?):(?P<line>-?\d+):(?P<col>\d+): (?P<sev>\w+): (?P<msg>.*) "
                     r"\[(?P<id>\w+)\]$")
findings = []
other = []
for line in text.splitlines():
    m = FINDING.match(line)
    if not m:
        if line.strip():
            other.append(line)
        continue
    # An id suppressed everywhere that this run had nothing to say about is
    # not stale, only quiet; the vendored headers may or may not be reached;
    # and a run over given files does not reach the files the list names. A
    # suppression scoped to a file of the tree, or inline, that matched
    # nothing IS stale, and fails like a finding.
    if m["id"] == "unmatchedSuppression" and (
            m["file"] == "nofile" or "thirdparty" in m["file"]
            or (files_given and m["file"].startswith("*"))):
        continue
    findings.append(line)
for line in findings:
    print(f"  FAIL  {line}")
if status != 0 or other:
    for line in other:
        print(f"  FAIL  cppcheck: {line}")
    print(f"FAIL: cppcheck exited {status}" if status else "FAIL: cppcheck said the above")
    sys.exit(1)
if findings:
    print()
    print(f"FAIL: {len(findings)} cppcheck finding(s). Fix it; if it is not a bug, suppress it")
    print("      on the line before with the reason: `// cppcheck-suppress id ; why`.")
    sys.exit(1)
print("  ok    cppcheck: no finding, and every suppression still matches and says why")
PY
