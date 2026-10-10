#!/usr/bin/env bash
# Fail if our C++ breaks a style rule that clang-tidy cannot see.
#
# clang-tidy parses ONE branch of every #if, so the web, iOS, Android and
# Windows code is invisible to a lint run on Linux -- a C cast sat in
# examples/platform/02_mobile_raymob for that reason. And some rules are not
# a clang-tidy check at all. So they are restated here as text, over every
# branch, the way tools/naming_check.sh restates the names:
#
#   S1  no C cast to an arithmetic type: `(int)x` is `static_cast<int>(x)`,
#       which says which conversion was meant and is the one a reader can grep
#   S2  no `NULL`: `nullptr` has a type, NULL is an integer that a call can
#       pick the wrong overload with
#   S3  no `TODO`, `FIXME` or `XXX`: a comment says what IS; a plan goes in
#       the plan, where somebody reads it
#   S4  nothing newer than the floor compiler, NetBSD's GCC 10.5, in the code a
#       game compiles: <format>, <source_location>, std::bit_cast and
#       `using enum` arrived in GCC 11-13, and the C++23 headers later still
#   S5  no user-defined literal, ours or the standard library's (Google's
#       rule): `operator""` and the `using namespace std::...literals` that
#       a `"x"sv` or a `10ms` needs
#   S6  `++i`, not `i++`, where the value is not used: as a statement and in
#       a for loop's increment (and `--i` alike). Google's rule; the postfix
#       form promises a copy of the old value that nothing reads
#
# Comments, strings and character literals do not count (S3 reads only the
# comments): they are blanked before any rule runs, so a comment may say what
# is forbidden.
#
# C is exempt from S1, S2 and S6: tests/smoke_test.h and the .c files are
# compiled as C, where a cast, NULL and `i++` are the language.
#
# Usage:
#   tools/style_check.sh            the tree: include/, src/, tests/, examples/
#   tools/style_check.sh FILE...    exactly those files, with every rule --
#                                   which is how the tests watch it go red

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$@" <<'PY'
import pathlib
import re
import sys

sys.path.insert(0, "tools")
from cpp_text import Lines, blank, tree  # noqa: E402  (the shared scanner)

RULES = {
    "S1": "a C cast to an arithmetic type: static_cast<T>(x)",
    "S2": "NULL: nullptr",
    "S3": "a TODO, FIXME or XXX: say what is, and put the plan in the plan",
    "S4": "newer than GCC 10.5, the floor compiler (NetBSD), in code a game compiles",
    "S5": "a user-defined literal: Google's style has none, the standard library's included",
    "S6": "postfix ++/-- whose value is not used: ++i",
}

# Compiled as C: tests/smoke_test.h is included by examples/plain_c/main.c.
C_HEADERS = {"tests/smoke_test.h"}

# S4 covers what a game's build compiles on every target, NetBSD's GCC 10.5
# among them. tests/ build on the CI image's compilers only.
FLOOR_EXEMPT = ("tests/",)

ARITHMETIC = (r"(?:(?:un)?signed\s+)?(?:char|short|int|long\s+long|long|float|double|bool"
              r"|(?:std::)?u?int(?:8|16|32|64)_t|(?:std::)?size_t|(?:std::)?ptrdiff_t"
              r"|unsigned|signed|char8_t|char16_t|char32_t|wchar_t)")
# `(int)x`: an arithmetic type alone in parentheses, not after a name (a
# declaration `f(int)`, `sizeof(int)`, `function<void(int)>`), a `)` or a `]`
# (`void (*fn)(int)`, a lambda `[](int)`), and followed by an operand.
C_CAST = re.compile(r"(?<![\w)\]>])(\(\s*(?:const\s+)?" + ARITHMETIC + r"\s*\))\s*(?=[\w(.\-+!~*&])")

FLOOR = re.compile(r"#\s*include\s*<(format|source_location|expected|print|stacktrace|generator"
                   r"|flat_map|flat_set|mdspan|spanstream)>|\bstd::bit_cast\b|\busing\s+enum\b")

UDL_DEFINITION = re.compile(r"\boperator\s*\"\"")
# std::literals, std::string_view_literals, std::chrono_literals and the rest.
UDL_IMPORT = re.compile(r"\busing\s+(?:namespace\s+)?[\w:]*literals\b")

# An lvalue the way it is written before ++: a name -- qualified, with
# template arguments, called (`rmp::global<Progress>().runs`,
# `reinterpret_cast<T *>(p)->refs`) -- or `(*p)`, then any chain of `.x`,
# `->x`, `[i]` and `(args)`.
NAME = r"(?:::\s*)?(?:\w+\s*(?:<[^<>;{}()]*>\s*)?::\s*)*\w+(?:\s*<[^<>;{}()]*>)?"
LVALUE = (r"(?:" + NAME + r"|\(\s*\*\s*\w+\s*\))"
          r"(?:\s*\([^();{}]*\)|\s*(?:\.|->)\s*\w+|\s*\[[^\[\]\n]*\])*")
POSTFIX = re.compile(r"(" + LVALUE + r")\s*(\+\+|--)")
# A statement starts after one of these, or after `else`/`do`.
STATEMENT_START = re.compile(r"(?:[;{}]|\)|(?<!:):|\belse|\bdo)$")


def for_increments(code):
    """(start, end) of each for loop's third clause."""
    out = []
    for m in re.finditer(r"\bfor\s*\(", code):
        depth, k, semis = 1, m.end(), []
        while k < len(code) and depth:
            ch = code[k]
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
            elif ch == ";" and depth == 1:
                semis.append(k)
            k += 1
        if len(semis) == 2:  # a range-for has none
            out.append((semis[1] + 1, k - 1))
    return out


def postfix_sites(code):
    """(offset, lvalue, op) of every postfix ++/-- whose value is not used."""
    sites = []
    clauses = for_increments(code)
    for m in POSTFIX.finditer(code):
        start, end = m.start(1), m.end()
        before = code[:start]
        after = code[end:end + 40].lstrip()
        in_clause = next(((a, b) for a, b in clauses if a <= start < b), None)
        if in_clause is not None:
            # One item of the increment clause, alone between commas.
            a, b = in_clause
            left = code[a:start].rstrip()
            right = code[end:b].strip()
            if (left == "" or left.endswith(",")) and (right == "" or right.startswith(",")):
                sites.append((start, m.group(1), m.group(2)))
            continue
        if after.startswith(";") and STATEMENT_START.search(before.rstrip()[-8:] or ";"):
            sites.append((start, m.group(1), m.group(2)))
    return sites


def check(path, rules):
    posix = path.as_posix()
    raw = path.read_text(encoding="utf-8", errors="replace")
    code = blank(raw)
    line_of = Lines(code).of
    is_c = path.suffix == ".c" or posix in C_HEADERS or posix.endswith("/smoke_test.h")
    found = []

    def hit(rule, offset, what):
        if rule in rules:
            found.append((rule, line_of(offset), what))

    if not is_c:
        for m in C_CAST.finditer(code):
            hit("S1", m.start(1), " ".join(m.group(1).split()))
        for m in re.finditer(r"\bNULL\b", code):
            hit("S2", m.start(), "NULL")
        for offset, lvalue, op in postfix_sites(code):
            hit("S6", offset, f"{' '.join(lvalue.split())}{op}")
    comments = blank(raw, comments=False)
    for m in re.finditer(r"\b(TODO|FIXME|XXX)\b", comments):
        hit("S3", m.start(), m.group(1))
    if not posix.startswith(FLOOR_EXEMPT):
        for m in FLOOR.finditer(code):
            hit("S4", m.start(), " ".join(m.group(0).split()))
    strings_kept = blank(raw, strings=False)
    for m in UDL_DEFINITION.finditer(strings_kept):
        hit("S5", m.start(), "operator\"\"")
    for m in UDL_IMPORT.finditer(code):
        hit("S5", m.start(), " ".join(m.group(0).split()))
    return found


args = sys.argv[1:]
files = [pathlib.Path(a) for a in args] if args else tree()
fails = 0
for path in files:
    if not path.is_file():
        print(f"  FAIL  {path} does not exist")
        fails += 1
        continue
    for rule, line, what in check(path, set(RULES)):
        print(f"  FAIL  {path}:{line}: {rule} {what} -- {RULES[rule]}")
        fails += 1

if fails:
    print()
    print(f"FAIL: {fails} line(s) break the style rules. The rule says what to write")
    print("      instead; tools/style_check.sh says why.")
    sys.exit(1)
print(f"  ok    style holds in {len(files)} file(s) (rules: {' '.join(sorted(RULES))})")
PY
