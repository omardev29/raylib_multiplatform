#!/usr/bin/env python3
"""What each public header costs to include, and a gate on it growing.

    python3 tools/header_cost.py            report: lines and milliseconds per header
    python3 tools/header_cost.py --check    the gate: preprocessed lines vs tools/header_budget.txt

Every header under include/rmp/ is something a game has to know exists, and
every include it carries is paid by every translation unit that includes it.
The project measures that rather than arguing about it. Two numbers per
header:

  lines   what the preprocessor emits for a TU that includes only that header
          (`-E | wc -l`). Deterministic for a given toolchain, so it is what
          the gate compares: a header that starts pulling <functional> in shows
          up as thousands of lines, on every machine, every time.
  ms      wall time of `-fsyntax-only`, best of five. Machine-dependent, so it
          is reported and never gated; it is the number people actually feel.

The budget in tools/header_budget.txt is in lines. --check fails when a header
is more than BUDGET_TOLERANCE over its budget (it grew), and also when it is
more than that UNDER it (the budget is stale and should come down, so that the
next growth is caught against the real number). Only inside the pinned build
image is the toolchain the one the budget was written against, so on a laptop
--check reports and exits 0 unless --strict is given.
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
DEFINES = ['-DRESOURCES_PATH="./resources/"', "-DPRODUCTION_BUILD=0"]


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
        lines = out.stdout.count("\n")
        best = None
        for _ in range(runs):
            t = time.perf_counter()
            subprocess.run([cxx, "-fsyntax-only", *flags, str(tu)], check=True, capture_output=True)
            d = (time.perf_counter() - t) * 1000
            best = d if best is None else min(best, d)
    return lines, best


def read_budget() -> dict[str, int]:
    budget: dict[str, int] = {}
    if not BUDGET.is_file():
        return budget
    for line in BUDGET.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, value = line.split()
        budget[name] = int(value)
    return budget


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="compare against the budget")
    ap.add_argument("--strict", action="store_true", help="fail outside the build image too")
    ap.add_argument("--write-budget", action="store_true", help="rewrite tools/header_budget.txt from this run")
    args = ap.parse_args(argv)

    cxx = compiler()
    budget = read_budget()
    in_image = IMAGE_MANIFEST.is_file()
    gate = args.check and (in_image or args.strict)
    fails = 0
    rows = []
    for header in headers():
        lines, ms = measure(cxx, header)
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
        flag = "  FAIL " if verdict and gate else ("  note " if verdict else "  ok   ")
        print(f"{flag} rmp/{header.name:14} {lines:7} lines {ms:8.1f} ms  {verdict}")
        if verdict and gate:
            fails += 1

    if args.write_budget:
        text = "# Preprocessed lines per public header, measured by tools/header_cost.py\n"
        text += "# inside the pinned build image. --check fails when a header grows past\n"
        text += f"# this by more than {int(BUDGET_TOLERANCE * 100)}%, or shrinks under it by as much.\n"
        text += "".join(f"{name:16} {lines}\n" for name, lines, _ in rows)
        BUDGET.write_text(text)
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
