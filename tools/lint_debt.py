#!/usr/bin/env python3
"""clang-tidy over the files tools/lint.sh lists, held to tools/lint_debt.txt.

    python3 tools/lint_debt.py check FILE...   the gate (`rmp lint`, the CI lint job)
    python3 tools/lint_debt.py lower FILE...   the same, and tools/lint_debt.txt
                                               rewritten with what is left

The style guide of 2026-10-10 turned on checks the tree did not yet pass --
misc-const-correctness, std::array, no raw pointer -- and a check that is on
but failing is a check nobody can turn on. So what is left is WRITTEN DOWN, one
line per check and file, and nothing else gets in:

    misc-const-correctness  src/rmp/ldtk.cpp  3

  - a warning in a file and check with no line fails;
  - more warnings than a line owes fails (the new one cannot be told from the
    old ones, so all of them are printed);
  - FEWER than a line owes fails too: the debt was paid, and the line must come
    down to what is left so that the next one is caught. `lower` writes that.
    A line can only ever come down, never go up, and `lower` adds none.

A header reached from several translation units is counted once: every
warning is keyed by the file it is in (normalised, so tests/../src/rmp/x.h is
src/rmp/x.h), its line, column and check.

Every file runs in its own clang-tidy process, its output kept whole, with
--experimental-custom-checks: without that flag the CustomChecks in
.clang-tidy are not registered, and clang-tidy does not say so. A process that
exits non-zero could not compile its file, and that fails whatever it owes.
So does a warning with no file in front of it, which is clang-tidy talking
about its configuration: a CustomChecks query that does not parse is reported
exactly that way -- `warning: 1:1: Error parsing argument 4 for matcher
varDecl.` -- and the check is then simply not run, exit status 0.

Exit: 0 clean, 1 a finding that is not owed, a stale line, a malformed line,
or a file clang-tidy could not compile.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEBT = REPO / "tools" / "lint_debt.txt"

WARNING = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+): (?:warning|error): "
                     r"(?P<message>.*) \[(?P<check>[\w.,-]+)\]$")
# clang-tidy about itself: no file, no line, no check.
CONFIG = re.compile(r"^(?:warning|error): ")

HEADER = """\
# What clang-tidy still finds in src/, tests/ and examples/, written down so
# that it can only shrink: `check file count`, one line per check and file.
# tools/lint_debt.py fails a warning that is not owed here, a count that grew,
# and a count that is higher than what is left -- `bash tools/lint.sh debt`
# lowers those. Nothing is ever added: a new warning is fixed, or it carries a
# NOLINT(check) with the reason on the line it is about.
"""


def relative(path: str) -> str:
    """A path clang-tidy printed, as the repository names it."""
    full = os.path.normpath(path if os.path.isabs(path) else os.path.join(REPO, path))
    try:
        return Path(full).relative_to(REPO).as_posix()
    except ValueError:
        return Path(full).as_posix()


def parse(output: str) -> dict[tuple[str, str], dict[tuple[int, int], str]]:
    """{(check, file): {(line, col): message}} for every warning in clang-tidy's
    output. The same location twice -- a header two files include -- is one."""
    found: dict[tuple[str, str], dict[tuple[int, int], str]] = {}
    for raw in output.splitlines():
        m = WARNING.match(raw.strip())
        if not m:
            continue
        where = (int(m["line"]), int(m["col"]))
        found.setdefault((m["check"], relative(m["file"])), {})[where] = m["message"]
    return found


def config_problems(output: str) -> list[str]:
    """What clang-tidy said about its own configuration, once each."""
    seen = []
    for raw in output.splitlines():
        line = raw.strip()
        if CONFIG.match(line) and line not in seen:
            seen.append(line)
    return seen


def read_debt(text: str) -> tuple[dict[tuple[str, str], int], list[str]]:
    """({(check, file): count}, [problems]) from the text of lint_debt.txt."""
    debt: dict[tuple[str, str], int] = {}
    problems = []
    for number, line in enumerate(text.splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        if len(parts) != 3 or not parts[2].isdigit() or int(parts[2]) == 0:
            problems.append(f"tools/lint_debt.txt:{number}: `{line}` is not `check file count` "
                            "with a count above zero")
            continue
        key = (parts[0], parts[1])
        if key in debt:
            problems.append(f"tools/lint_debt.txt:{number}: {parts[0]} {parts[1]} is owed twice")
            continue
        debt[key] = int(parts[2])
    return debt, problems


def compare(found, debt) -> tuple[list[str], dict[tuple[str, str], int]]:
    """(failures, the debt lowered to what is left)."""
    failures = []
    lowered = {}
    for key in sorted(set(found) | set(debt)):
        check, file = key
        have = len(found.get(key, {}))
        owed = debt.get(key, 0)
        if have > owed:
            said = "not owed" if owed == 0 else f"{owed} owed"
            lines = [f"  FAIL  {file}: {have} {check}, {said}"]
            for (line, col), message in sorted(found[key].items()):
                lines.append(f"          {file}:{line}:{col}: {message}")
            failures.append("\n".join(lines))
        elif have < owed:
            failures.append(f"  FAIL  tools/lint_debt.txt owes {owed} {check} in {file}, and "
                            f"{have} are left: lower the line (`bash tools/lint.sh debt`)")
        if owed:
            lowered[key] = min(owed, have)
    return failures, {k: v for k, v in lowered.items() if v}


def write_debt(debt: dict[tuple[str, str], int]) -> None:
    width = max((len(c) for c, _ in debt), default=0)
    fwidth = max((len(f) for _, f in debt), default=0)
    body = "".join(f"{c:<{width}}  {f:<{fwidth}}  {n}\n"
                   for (c, f), n in sorted(debt.items(), key=lambda kv: (kv[0][1], kv[0][0])))
    DEBT.write_text(HEADER + "\n" + body)


def tidy(file: str) -> tuple[str, int, str]:
    got = subprocess.run(["clang-tidy", "-p", "build/lint", "--quiet",
                          "--experimental-custom-checks", file],
                         cwd=REPO, capture_output=True, text=True)
    return file, got.returncode, got.stdout + got.stderr


def main(argv: list[str]) -> int:
    if not argv or argv[0] not in ("check", "lower"):
        print("usage: lint_debt.py check|lower FILE...")
        return 1
    mode, files = argv[0], argv[1:]
    jobs = os.cpu_count() or 2
    output = []
    broken = []
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for file, code, text in pool.map(tidy, files):
            output.append(text)
            if code != 0:
                broken.append((file, text))
    for file, text in broken:
        sys.stdout.write(text)
        print(f"  FAIL  clang-tidy could not read {file} (exit status above)")
    found = parse("\n".join(output))
    for said in config_problems("\n".join(output)):
        broken.append(("the configuration", said))
        print(f"  FAIL  clang-tidy about its configuration: {said}")
    debt, problems = read_debt(DEBT.read_text() if DEBT.is_file() else "")
    failures, lowered = compare(found, debt)
    for line in problems + failures:
        print(line)
    if mode == "lower" and lowered != debt:
        write_debt(lowered)
        print(f"  wrote tools/lint_debt.txt: {sum(lowered.values())} warning(s) owed "
              f"in {len(lowered)} line(s)")
        failures = [f for f in failures if "lower the line" not in f]
    if broken or problems or failures:
        print()
        print("FAIL: clang-tidy found what tools/lint_debt.txt does not owe, or the debt is")
        print("      stale. Fix what is new -- or NOLINT(check) it with the reason --")
        print("      and lower what was paid.")
        return 1
    owed = sum(debt.values())
    print(f"  ok    {len(files)} file(s): nothing new, and the {owed} warning(s) still owed "
          f"in {len(debt)} line(s) of tools/lint_debt.txt are all there")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
