#!/usr/bin/env python3
"""What each public header costs to include, and a gate on it growing.

    python3 tools/header_cost.py            report: lines and milliseconds per header
    python3 tools/header_cost.py --check    the gate: preprocessed lines vs tools/header_budget.txt

Every header under include/rmp/ is something a game has to know exists, and
every include it carries is paid by every translation unit that includes it.
The project measures that rather than arguing about it. Two numbers per
header:

  lines   the lines of CODE the preprocessor emits for a TU that includes
          only that header: `-E`, without blank lines and line markers.
          Deterministic for a given toolchain, so it is what the gate compares:
          a header that starts pulling <functional> in shows up as thousands of
          lines, on every machine, every time. Not the raw `-E | wc -l`, which
          also counted the blank line every comment leaves behind -- so ten
          lines of documentation put rmp/ads.h 23% over its budget.
  ms      wall time of `-fsyntax-only`, best of five. Machine-dependent, so it
          is reported and never gated; it is the number people actually feel.

The budget in tools/header_budget.txt is in lines. --check fails when a header
is more than BUDGET_TOLERANCE over its budget (it grew), and also when it is
more than that UNDER it (the budget is stale and should come down, so that the
next growth is caught against the real number). Only inside the pinned build
image is the toolchain the one the budget was written against, so on a laptop
those two report and exit 0 unless --strict is given.

What does NOT depend on the toolchain fails everywhere: a header with no line
in the budget, a line in the budget for a header that no longer exists, and
a header that does not compile with the include list below (what the
compiler said is printed under its name, and --write-budget writes nothing).
rmp/audio.h reached CI with no budget, because `rmp test` on a laptop only
reported it -- and whether a name is in a file is the same on every machine.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUDGET = REPO / "tools" / "header_budget.txt"
IMAGE_MANIFEST = Path("/etc/raylib-build-image.json")
BUDGET_TOLERANCE = 0.20

INCLUDES = [
    "include", "thirdparty/raylib/src", "thirdparty/rres", "thirdparty/cute_aseprite",
    "thirdparty/cute_tiled", "thirdparty/raymob", "thirdparty/clay", "thirdparty", "tests",
]
DEFINES = ['-DRMP_RESOURCES_PATH="./resources/"', "-DRMP_PRODUCTION_BUILD=0"]


def compiler() -> str:
    return os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++") or "c++"


def headers() -> list[Path]:
    return sorted(p for p in (REPO / "include" / "rmp").glob("*.h") if p.name != "config.h")


def measure(cxx: str, header: Path, runs: int = 5) -> tuple[int, float]:
    """(preprocessed lines, best-of-`runs` syntax-only milliseconds)."""
    flags = [f"-I{REPO / d}" for d in INCLUDES] + DEFINES + ["-std=c++20"]
    with tempfile.TemporaryDirectory() as tmp:
        tu = Path(tmp) / "one.cpp"
        tu.write_text(f"#include <rmp/{header.name}>\nint main() {{ return 0; }}\n")
        out = subprocess.run([cxx, "-E", *flags, str(tu)], capture_output=True, text=True, check=True)
        lines = sum(1 for line in out.stdout.splitlines()
                    if line.strip() and not line.startswith("#"))
        best = None
        for _ in range(runs):
            t = time.perf_counter()
            subprocess.run([cxx, "-fsyntax-only", *flags, str(tu)], check=True, capture_output=True)
            d = (time.perf_counter() - t) * 1000
            best = d if best is None else min(best, d)
    return lines, best


class BudgetError(Exception):
    """tools/header_budget.txt says something that is not a budget. The
    message names the line."""


HEADER = "# Preprocessed lines of code per public header (blank lines and line\n" \
         "# markers not counted), measured by tools/header_cost.py\n" \
         "# inside the pinned build image. --check fails when a header grows past\n" \
         f"# this by more than {int(BUDGET_TOLERANCE * 100)}%, or shrinks under it by as much.\n"


def format_budget(rows) -> str:
    """The file --write-budget writes: a header comment, then `name lines`."""
    return HEADER + "".join(f"{name:16} {lines}\n" for name, lines in rows)


def parse_budget(text: str) -> dict[str, int]:
    """name -> lines for every `name lines` line; blank lines and `#` lines
    are ignored, and anything else is a BudgetError naming its line -- a
    note after the number included, which is how a hand edit annotates it."""
    budget: dict[str, int] = {}
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        words = line.split()
        if len(words) != 2 or not words[1].isascii() or not words[1].isdigit():
            raise BudgetError(f"tools/header_budget.txt:{number}: {raw.strip()!r} is not "
                              "`<header> <lines>` -- a name and a count, and nothing after")
        if words[0] in budget:
            raise BudgetError(f"tools/header_budget.txt:{number}: {words[0]} has a budget "
                              "already, on an earlier line: one line a header")
        budget[words[0]] = int(words[1])
    return budget


def read_budget() -> dict[str, int]:
    if not BUDGET.is_file():
        return {}
    return parse_budget(BUDGET.read_text(encoding="utf-8"))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="compare against the budget")
    ap.add_argument("--strict", action="store_true", help="fail outside the build image too")
    ap.add_argument("--write-budget", action="store_true", help="rewrite tools/header_budget.txt from this run")
    args = ap.parse_args(argv)

    cxx = compiler()
    try:
        budget = read_budget()
    except BudgetError as e:
        # Before any compiler runs: nothing measured can be compared with it.
        print(f"FAIL: {e}")
        return 1
    in_image = IMAGE_MANIFEST.is_file()
    gate = args.check and (in_image or args.strict)
    fails = 0
    broken = 0
    rows = []
    for header in headers():
        try:
            lines, ms = measure(cxx, header)
        except OSError as e:
            # No compiler to start: nothing can be measured, once.
            print(f"FAIL: {cxx} could not be started: {e}")
            return 1
        except subprocess.CalledProcessError as e:
            # A header the compiler refuses with INCLUDES and DEFINES above --
            # a vendored folder CMake has and this list lacks -- measures
            # nothing, on every machine; what the compiler said is the
            # reason, and a traceback left it out.
            said = e.stderr.decode(errors="replace") if isinstance(e.stderr, bytes) \
                else (e.stderr or "")
            print(f"  FAIL  rmp/{header.name:14} does not compile with {cxx} and the include "
                  "list of tools/header_cost.py:")
            for line in said.strip().splitlines()[:8]:
                print(f"          {line}")
            fails += 1
            broken += 1
            continue
        rows.append((header.name, lines, ms))
        want = budget.get(header.name)
        verdict = ""
        if args.check and want is not None:
            if lines > want * (1 + BUDGET_TOLERANCE):
                verdict = f"GREW: {lines} lines against a budget of {want}"
            elif lines < want * (1 - BUDGET_TOLERANCE):
                verdict = f"STALE BUDGET: {lines} lines against a budget of {want}; lower it"
        elif args.check:
            verdict = "NO BUDGET: add it to tools/header_budget.txt"
        # A missing budget line is the same mistake on every machine.
        fatal = bool(verdict) and (gate or want is None)
        flag = "  FAIL " if fatal else ("  note " if verdict else "  ok   ")
        print(f"{flag} rmp/{header.name:14} {lines:7} lines {ms:8.1f} ms  {verdict}")
        if fatal:
            fails += 1
    if args.check:
        present = {h.name for h in headers()}
        for name in sorted(set(budget) - present):
            print(f"  FAIL  rmp/{name:14} is in tools/header_budget.txt but not in include/rmp/: "
                  "remove the line")
            fails += 1

    if args.write_budget and broken:
        # Written without them, the next --check would say NO BUDGET of a
        # header that has one; the file stays as it was.
        print(f"not written: {BUDGET.relative_to(REPO)} keeps its lines while a header "
              "does not compile")
    elif args.write_budget:
        BUDGET.write_text(format_budget((name, lines) for name, lines, _ in rows))
        print(f"wrote {BUDGET.relative_to(REPO)}")

    if args.check and not gate:
        print("note: not inside the build image; the budget is compared for information only")
    if fails:
        print(f"FAIL: {fails} header(s) off budget")
        return 1
    print(f"PASS: {len(rows)} headers measured" + (" and within budget" if args.check else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
