#!/usr/bin/env bash
# Fail if a line of our shell uses `A && B || C` as if it were if/then/else.
#
# WHY THIS EXISTS. It is not if/then/else. When A succeeds and B FAILS, C runs
# too -- so the error branch fires on the success path and the exit status is
# C's. It reads like a conditional and behaves like a trap.
#
# CLAUDE.md said actionlint catches this. It does not: shellcheck's SC2015 only
# fires on a couple of degenerate shapes (a literal `true`/`false`, an obvious
# constant), and every occurrence this repository has actually shipped was of a
# shape SC2015 says nothing about. It was written once, fixed, and then written
# again in SEVEN more places two rounds later. That is the definition of a thing
# that needs a gate rather than a note.
#
# WHAT TO WRITE INSTEAD:
#
#   if A; then B; else C; fi          the conditional, spelled as one
#   A || { C; exit 1; }               the guard -- no `&&`, so it is not this
#   A; rc=$?                          when you want the status, keep the status
#
# Strings and comments do not count: a rule that cannot be written down next to
# the code it forbids is a rule people delete. GitHub's own `${{ a && b || c }}`
# does not count either -- that is an expression in GitHub's language, where it
# really is a ternary, and it never reaches a shell.
#
# Usage: tools/shell_pattern_check.sh [file ...]
#
# With no arguments: tools/*.sh, the Justfile, and the `run:` blocks of every
# workflow. With arguments, exactly those files -- which is how the test suite
# points it at a fixture and watches it go red.

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$@" <<'PY'
import pathlib
import re
import sys

SELF = "tools/shell_pattern_check.sh"

# An explicit, reasoned escape hatch. Nothing in the tree uses it today; it
# exists so that the one legitimate case nobody has thought of yet does not
# arrive as a reason to delete the whole check.
ALLOW_MARKER = "shell-pattern-ok:"


def shell_code(line):
    """The part of the line a shell would treat as syntax.

    Comments, single- and double-quoted strings and GitHub `${{ }}` expressions
    are blanked out, because an `&&` inside any of them is text, not an
    operator. Written as a scanner rather than a regex because quoting nests:
    portable_check.sh's own advice string contains `&& pwd` inside double
    quotes on a line that really does end in `|| fails=...`, and any regex that
    flags that is a regex that gets switched off within a week.
    """
    line = re.sub(r"\$\{\{.*?\}\}", " ", line)
    out = []
    quote = None
    i = 0
    while i < len(line):
        c = line[i]
        if quote == "'":
            if c == "'":
                quote = None
            out.append(" ")
        elif quote == '"':
            if c == "\\":
                out.append("  ")
                i += 2
                continue
            if c == '"':
                quote = None
            out.append(" ")
        elif c in "'\"":
            quote = c
            out.append(" ")
        elif c == "#" and (not out or out[-1].isspace()):
            break  # a comment runs to end of line
        else:
            out.append(c)
        i += 1
    return "".join(out)


def offending(line):
    code = shell_code(line)
    a = code.find("&&")
    if a < 0:
        return False
    return code.find("||", a + 2) >= 0


def run_block_lines(path):
    """(lineno, text) for every line inside a `run:` block of a workflow.

    A textual scan and not a YAML parse, deliberately: this has to work on a
    laptop without PyYAML, and it has to report the line number in the FILE,
    which a parsed scalar has already thrown away.
    """
    lines = path.read_text(encoding="utf-8").splitlines()
    out = []
    i = 0
    while i < len(lines):
        m = re.match(r"^(\s*)-?\s*run:\s*(.*)$", lines[i])
        if not m:
            i += 1
            continue
        indent, rest = len(m.group(1)), m.group(2).strip()
        if rest and rest not in ("|", ">", "|-", ">-", "|+", ">+"):
            out.append((i + 1, rest))  # one-line form
            i += 1
            continue
        i += 1
        while i < len(lines):
            line = lines[i]
            if line.strip() and (len(line) - len(line.lstrip())) <= indent:
                break
            out.append((i + 1, line))
            i += 1
    return out


def default_sources():
    files = sorted(p for p in pathlib.Path("tools").glob("*.sh")
                   if p.as_posix() != SELF)
    files.append(pathlib.Path("Justfile"))
    files.extend(sorted(pathlib.Path(".github/workflows").glob("*.yml")))
    return files


args = [pathlib.Path(a) for a in sys.argv[1:]]
sources = args or default_sources()

fails = 0
scanned = 0
for path in sources:
    if not path.is_file():
        print(f"  FAIL  {path} does not exist")
        fails += 1
        continue
    if path.suffix in (".yml", ".yaml"):
        numbered = run_block_lines(path)
    else:
        numbered = list(enumerate(path.read_text(encoding="utf-8").splitlines(), 1))
    scanned += 1
    for lineno, text in numbered:
        if ALLOW_MARKER in text:
            continue
        if offending(text):
            print(f"  FAIL  {path}:{lineno}: `A && B || C` is not if/then/else")
            print(f"          {text.strip()}")
            fails += 1

if fails:
    print()
    print(f"FALLA: {fails} line(s). When A succeeds and B fails, C runs as well, so")
    print("       the error branch fires on the success path. Write it as")
    print("         if A; then B; else C; fi      or      A || { C; exit 1; }")
    print(f"       If one is genuinely right, say so with a `# {ALLOW_MARKER} <why>` comment.")
    sys.exit(1)
print(f"  ok    no `A && B || C` in {scanned} file(s)")
PY
