#!/usr/bin/env python3
"""The mutation scores, held to their floors.

tools/mutation.sh runs Mull (C++) or mutmut (Python) over one module and
hands this the report the tool wrote -- read as data, never from what it
printed: Mull's Mutation Testing Elements JSON, mutmut's mutants/<file>.meta
files. This compares it with tests/mutation/floors/<module>.txt:

    # one line saying what the module is
    kind        cxx | py
    sources     the files mutated, repository paths
    tests       cxx: a test program and its arguments, one line a program
                py:  the test files pytest runs
    select      py only, optional: mutmut's mutant names, globs allowed
    floor       the score that has to hold, 0-100, or `-` for not measured
    equivalent  <mutant id> <why no test can kill it>    (any number)

The score is the mutants a test detected -- killed, timed out, crashed --
over every mutant but the equivalent ones, in percent. A mutant no test
reaches counts against it: it is a test nobody wrote.

  - Below the floor fails.
  - A floor below the score rounded down to 5 fails too: raise it in the
    commit that earned it (--update writes it). The floor only rises; a lower
    one is a hand edit a reviewer reads.
  - An `equivalent` line whose mutant a test now kills, or that is gone (the
    code moved: Mull's ids carry the line), fails until the line goes. The
    list only shrinks, and every line says why.
  - `-` is a module nobody has measured yet: printed, never failed. The
    weekly run measures each one; its package sets the floor.

Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FLOORS = REPO / "tests" / "mutation" / "floors"
KINDS = ("cxx", "py")
KEYS = ("kind", "sources", "tests", "select", "floor", "equivalent")

# What a mutant came to, in Mull's words and mutmut's exit codes, as one of
# DETECTED, UNDETECTED or NOT_COUNTED.
DETECTED, UNDETECTED, NOT_COUNTED, UNFINISHED = "detected", "undetected", "not counted", "unfinished"
MULL_STATUS = {
    "Killed": DETECTED, "Timeout": DETECTED, "RuntimeError": DETECTED,
    "Survived": UNDETECTED, "NoCoverage": UNDETECTED,
    "CompileError": NOT_COUNTED, "Ignored": NOT_COUNTED, "Pending": UNFINISHED,
}
# mutmut 3's status_by_exit_code (mutmut/stats.py), read as above.
MUTMUT_EXIT = {
    1: DETECTED, 3: DETECTED, 36: DETECTED, 24: DETECTED, -24: DETECTED, 152: DETECTED,
    255: DETECTED, -11: DETECTED, -9: DETECTED,
    0: UNDETECTED, 5: UNDETECTED, 33: UNDETECTED,
    34: NOT_COUNTED, 37: NOT_COUNTED,
    None: UNFINISHED, 2: UNFINISHED,
}


class FloorError(Exception):
    """A floor file that does not say what it must."""


@dataclass
class Floor:
    name: str
    kind: str = ""
    sources: list[str] = field(default_factory=list)
    tests: list[list[str]] = field(default_factory=list)
    select: list[str] = field(default_factory=list)
    floor: int | None = None
    equivalent: dict[str, str] = field(default_factory=dict)
    floor_line: int = 0


def parse_floor(text: str, name: str) -> Floor:
    """A floor file, every mistake refused with its line."""
    out = Floor(name=name)
    seen: set[str] = set()
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, _, rest = line.partition(" ")
        rest = rest.strip()
        where = f"{name}:{number}"
        if key not in KEYS:
            raise FloorError(f"{where}: unknown key {key!r}; the keys are {', '.join(KEYS)}")
        if not rest:
            raise FloorError(f"{where}: {key} says nothing")
        if key in ("kind", "sources", "floor", "select") and key in seen:
            raise FloorError(f"{where}: a second {key} line")
        seen.add(key)
        if key == "kind":
            if rest not in KINDS:
                raise FloorError(f"{where}: kind is {' or '.join(KINDS)}, not {rest!r}")
            out.kind = rest
        elif key == "sources":
            out.sources = rest.split()
        elif key == "tests":
            out.tests.append(rest.split())
        elif key == "select":
            out.select = rest.split()
        elif key == "floor":
            if rest == "-":
                out.floor = None
            elif re.fullmatch(r"\d{1,3}", rest) and 0 <= int(rest) <= 100:
                out.floor = int(rest)
            else:
                raise FloorError(f"{where}: floor is a whole number from 0 to 100, or -; not {rest!r}")
            out.floor_line = number
        elif key == "equivalent":
            mutant, _, why = rest.partition(" ")
            if len(why.strip()) < 10:
                raise FloorError(f"{where}: an equivalent mutant says why no test can kill it")
            if mutant in out.equivalent:
                raise FloorError(f"{where}: {mutant} is listed twice")
            out.equivalent[mutant] = why.strip()
    for key in ("kind", "sources", "tests", "floor"):
        if key not in seen:
            raise FloorError(f"{name}: no {key} line")
    if out.select and out.kind != "py":
        raise FloorError(f"{name}: select is mutmut's, for kind py")
    return out


def load_floors(folder: Path = FLOORS) -> dict[str, Floor]:
    return {p.stem: parse_floor(p.read_text(encoding="utf-8"), p.name)
            for p in sorted(folder.glob("*.txt"))}


# --- reading what the tools wrote -------------------------------------------

def mull_mutants(report: dict, sources: list[str]) -> dict[str, str]:
    """Mutant id -> verdict, from Mull's Elements JSON. The id is Mull's
    `mutator:path:line:column`, with the path cut back to the repository's:
    Mull writes it absolute, and a checkout's folder is not part of a mutant."""
    out: dict[str, str] = {}
    for path, entry in (report.get("files") or {}).items():
        rel = next((s for s in sources if path.endswith("/" + s) or path == s), None)
        if rel is None:
            continue
        for mutant in entry.get("mutants") or []:
            start = mutant["location"]["start"]
            mid = f"{mutant['mutatorName']}:{rel}:{start['line']}:{start['column']}"
            out[mid] = MULL_STATUS.get(mutant.get("status"), UNFINISHED)
    return out


def mutmut_mutants(meta: dict) -> dict[str, str]:
    """Mutant name -> verdict, from one of mutmut's mutants/<file>.meta."""
    return {name: MUTMUT_EXIT.get(code, UNDETECTED)
            for name, code in (meta.get("exit_code_by_key") or {}).items()}


# --- the verdict -------------------------------------------------------------------

def floor5(score: float) -> int:
    return int(score // 5) * 5


def check(floor: Floor, mutants: dict[str, str]) -> tuple[list[str], list[str], float | None]:
    """(ok lines, failures, score) of one module."""
    ok: list[str] = []
    fails: list[str] = []
    name = floor.name.removesuffix(".txt")
    if not mutants:
        return ok, [f"{name}: the report holds no mutant of {' '.join(floor.sources)}: "
                    "nothing was mutated, or the report is of something else"], None
    unfinished = sorted(m for m, v in mutants.items() if v == UNFINISHED)
    if unfinished:
        fails.append(f"{name}: {len(unfinished)} mutant(s) never got a verdict, the run did "
                     f"not finish: {', '.join(unfinished[:5])}")
    for mutant in sorted(floor.equivalent):
        verdict = mutants.get(mutant)
        if verdict is None:
            fails.append(f"{name}: equivalent {mutant} is not in the report: the code moved "
                         "or it is gone. Take its line out (or list it again with its new id).")
        elif verdict == DETECTED:
            fails.append(f"{name}: equivalent {mutant} is killed now: it was not equivalent. "
                         "Take its line out.")
    counted = {m: v for m, v in mutants.items()
               if v in (DETECTED, UNDETECTED) and m not in floor.equivalent}
    if not counted:
        fails.append(f"{name}: every mutant is excluded; there is no score")
        return ok, fails, None
    detected = sum(1 for v in counted.values() if v == DETECTED)
    score = 100.0 * detected / len(counted)
    line = (f"{name:20} {score:6.2f}%  {detected}/{len(counted)} detected, "
            f"{len(floor.equivalent)} equivalent")
    if floor.floor is None:
        ok.append(f"{line}  (no floor yet: {floor5(score)} would be it)")
    elif score < floor.floor:
        fails.append(f"{line} -- below its floor of {floor.floor}")
    elif floor5(score) > floor.floor:
        fails.append(f"{line} -- the floor is {floor.floor}: raise it to {floor5(score)} "
                     f"(bash tools/mutation.sh update {name})")
    else:
        ok.append(f"{line}  (floor {floor.floor})")
    return ok, fails, score


def raised(text: str, floor: Floor, score: float) -> str:
    """The floor file with its floor raised to the score rounded down to 5 --
    never lowered, and a `-` is a hand edit."""
    if floor.floor is None or floor5(score) <= floor.floor:
        return text
    lines = text.splitlines(keepends=True)
    lines[floor.floor_line - 1] = re.sub(r"\d+", str(floor5(score)), lines[floor.floor_line - 1], count=1)
    return "".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("module", help="tests/mutation/floors/<module>.txt")
    parser.add_argument("--mull", type=Path, action="append", default=[],
                        help="Mull's Elements JSON (one per test program)")
    parser.add_argument("--mutmut", type=Path, action="append", default=[],
                        help="mutmut's mutants/<file>.meta (one per source)")
    parser.add_argument("--floors", type=Path, default=FLOORS)
    parser.add_argument("--update", action="store_true",
                        help="raise the floor to the score rounded down to 5")
    parser.add_argument("--print", dest="show", choices=("kind", "sources", "tests", "select"),
                        help="one field of the floor file, for tools/mutation.sh")
    args = parser.parse_args(argv)

    path = args.floors / f"{args.module}.txt"
    if not path.is_file():
        print(f"FAIL: no {path}")
        return 1
    try:
        floor = parse_floor(path.read_text(encoding="utf-8"), path.name)
    except FloorError as e:
        print(f"FAIL: {e}")
        return 1
    if args.show:
        if args.show == "kind":
            print(floor.kind)
        elif args.show == "sources":
            print("\n".join(floor.sources))
        elif args.show == "tests":
            print("\n".join(" ".join(program) for program in floor.tests))
        else:
            print(" ".join(floor.select))
        return 0
    mutants: dict[str, str] = {}
    try:
        for report in args.mull:
            for mid, verdict in mull_mutants(json.loads(report.read_text()), floor.sources).items():
                # Killed by any program is killed.
                if mutants.get(mid) != DETECTED:
                    mutants[mid] = verdict
        for meta in args.mutmut:
            mutants.update(mutmut_mutants(json.loads(meta.read_text())))
    except (OSError, ValueError, KeyError, TypeError) as e:
        print(f"FAIL: {args.module}: the report cannot be read: {e}")
        return 1
    ok, fails, score = check(floor, mutants)
    if args.update and score is not None:
        # Raised first, then judged again: what --update cannot mend still fails.
        text = path.read_text(encoding="utf-8")
        new = raised(text, floor, score)
        if new != text:
            path.write_text(new, encoding="utf-8")
            shown = path.relative_to(REPO) if path.is_relative_to(REPO) else path
            print(f"  raised {shown} to {floor5(score)}")
            floor = parse_floor(new, path.name)
            ok, fails, score = check(floor, mutants)
    for line in ok:
        print(f"  ok    {line}")
    for line in fails:
        print(f"  FAIL  {line}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
