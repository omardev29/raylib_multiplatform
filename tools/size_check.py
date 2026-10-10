#!/usr/bin/env python3
"""The size of what a release ships, held to tools/size_budget.txt.

A release is kept as small as it can be -- the hardening block in
CMakeLists.txt measured every flag before it went in -- and nothing said so
after the measuring was done: a binary could grow by a third between two
releases and every job would stay green. This is the number written down, and
the step in every release job that holds the binary to it.

    size_check.py TARGET BINARY [SHIPPED]
        BINARY is the file as the linker wrote it (build/release/<name>, the
        .wasm), read before UPX can touch it. SHIPPED is the copy that goes
        into the archive -- packed, where [upx] applies; the same bytes, or
        left out, where it does not. Both sizes are held to TARGET's line.

    size_check.py android BUNDLE
        Every native library in an .aab or .apk, per ABI, as it sits in the
        bundle (the uncompressed size of its entry: what a device installs),
        each held to its android/<abi> line.

    size_check.py --check-file
        The file itself, with no binary: every line well-formed, one for each
        binary an enabled target ships, no name that is not one, the packed
        column exactly where [upx] packs. The lint job runs it on every push.

    size_check.py --update LOG...
        Rewrite the lines a CI log measured -- every check prints its line
        ending in "# measured at <commit>" -- so one run's log is one commit.
        "-" reads standard input:
            gh run view <run> --log | python3 tools/size_check.py --update -

Every check prints the measured line in the file's own format, pass or fail,
so a CI log is all it takes to write one.

THE FRAMEWORK HAS A BUDGET AND A GAME STARTS WITHOUT ONE. `rmp new` does not
copy tools/size_budget.txt: those are the demo's sizes, and a game's are its
own. A game's CI runs the same steps, prints its measured lines and says it has
no budget; the day it commits a tools/size_budget.txt, every target it builds
is held to it exactly as the framework's are. In the framework a missing file
is a failure.

Standard library only: it runs in the build image, on the Windows, macOS and
BSD runners, and on a laptop.
Exit: 0 = within budget (or a game with none), 1 = not, or the file is wrong,
2 = a usage error.
"""

from __future__ import annotations

import math
import os
import re
import subprocess
import sys
import zipfile
from fractions import Fraction
from pathlib import Path
from typing import NamedTuple

REPO = Path(__file__).resolve().parent.parent
BUDGET = "tools/size_budget.txt"

# The rule, and the reason for the two numbers is in the file's header
# (Omar, 2026-10-11): within WARN_AT of its line, either way, a binary passes
# and nothing is said; past it, and up to FAIL_AT, it passes with a warning
# that names the new number; past FAIL_AT it fails. Fractions, so the edges are
# exact: 100000 * 1.015 is 101499.99999999999 in a float, and "one byte over"
# has to mean one byte.
WARN_AT = Fraction(15, 1000)
FAIL_AT = Fraction(10, 100)

KINDS = ("elf", "pe", "macho", "wasm", "so")

# What AGP builds when nothing filters the ABIs, which nothing here does
# (raymob/app/build.gradle has no abiFilters): one library per ABI, each its
# own line, because each is a different download.
ANDROID_ABIS = ("arm64-v8a", "armeabi-v7a", "x86", "x86_64")

# Targets that are built and ship no release binary to measure, and why.
NOT_SHIPPED = {
    "ios": "CI builds it for the simulator, in Debug and unsigned: there is no "
           "release binary to measure until a game signs one",
}

# The kind each family's binary is, by tools/configure.py's TARGETS.
FAMILY_KIND = {"linux": "elf", "bsd": "elf", "windows": "pe", "apple": "macho",
               "web": "wasm", "android": "so"}

NOT_PACKED = "-"
UNKNOWN = "?"


class BudgetError(Exception):
    """The budget file is not what this reads. Exit code 1."""


class Usage(Exception):
    """A command line this cannot run. Exit code 2."""


class Line(NamedTuple):
    number: int            # in the file, 1-based
    key: str               # a target, or android/<abi>
    kind: str
    unpacked: int | None   # None: "?", nobody has measured it yet
    packed: int | str | None  # an int; NOT_PACKED where UPX does not apply; None: "?"
    comment: str


class Measured(NamedTuple):
    key: str
    kind: str
    unpacked: int
    packed: int | str      # an int, or NOT_PACKED


# ---------------------------------------------------------------------------
# The file
# ---------------------------------------------------------------------------

def parse(text: str, where: str = BUDGET) -> dict[str, Line]:
    """Every line of the budget, by key. A line that is not exactly four
    fields -- and an optional # comment -- is an error with its number."""
    out: dict[str, Line] = {}
    for n, raw in enumerate(text.splitlines(), 1):
        body, _, comment = raw.partition("#")
        fields = body.split()
        if not fields:
            continue
        say = f"{where}:{n}: "
        if len(fields) != 4:
            raise BudgetError(say + f"{len(fields)} fields where there are four -- "
                              f"binary, kind, unpacked bytes, packed bytes or -: {raw.strip()!r}")
        key, kind, unpacked, packed = fields
        if not re.fullmatch(r"[a-z0-9][a-z0-9_.-]*(/[a-z0-9_.-]+)?", key):
            raise BudgetError(say + f"{key!r} is not a target name, nor android/<abi>")
        if kind not in KINDS:
            raise BudgetError(say + f"kind {kind!r} is not one of {', '.join(KINDS)}")
        if key in out:
            raise BudgetError(say + f"{key} has a line already, at line {out[key].number}")
        out[key] = Line(n, key, kind, number(unpacked, say, "unpacked", allow_dash=False),
                        number(packed, say, "packed", allow_dash=True), comment.strip())
    return out


def number(field: str, say: str, column: str, allow_dash: bool):
    if field == UNKNOWN:
        return None
    if field == NOT_PACKED:
        if allow_dash:
            return NOT_PACKED
        raise BudgetError(say + f"{column} cannot be -: every binary has a size. "
                          "? is a size nobody has measured yet")
    if not re.fullmatch(r"[1-9][0-9]*", field):
        raise BudgetError(say + f"{column} = {field!r}: a number of bytes, a whole one and "
                          "more than nothing" + (", ? or -" if allow_dash else ", or ?"))
    return int(field)


def load(path: Path) -> dict[str, Line]:
    return parse(path.read_text(encoding="utf-8"), str(path.relative_to(REPO))
                 if path.is_relative_to(REPO) else str(path))


def render(m: Measured, sha: str) -> str:
    """A line in the file's own format, with the commit it was measured on --
    or "measured here", in a checkout with no commit to name."""
    where = f"measured at {sha}" if sha else "measured here"
    return f"{m.key:<23}{m.kind:<6}{m.unpacked:>10}{m.packed:>10}  # {where}"


def commit() -> str:
    """The commit a measurement belongs to: CI's, or this checkout's."""
    sha = os.environ.get("GITHUB_SHA", "")
    if not sha:
        got = subprocess.run(["git", "-C", str(REPO), "rev-parse", "HEAD"],
                             capture_output=True, text=True, stdin=subprocess.DEVNULL)
        sha = got.stdout.strip() if got.returncode == 0 else ""
    return sha[:7] if re.fullmatch(r"[0-9a-f]{7,40}", sha) else ""


def bounds(n: int, share: Fraction = WARN_AT) -> tuple[int, int]:
    """The sizes within `share` of `n`, either way: WARN_AT, the ones that
    pass in silence; FAIL_AT, the ones that pass at all."""
    return math.ceil(n * (1 - share)), math.floor(n * (1 + share))


# ---------------------------------------------------------------------------
# What a file is
# ---------------------------------------------------------------------------

def kind_of(head: bytes) -> str | None:
    """elf, pe, macho or wasm, from a file's first bytes; None for anything else."""
    if head[:4] == b"\x7fELF":
        return "elf"
    if head[:4] == b"\0asm":
        return "wasm"
    if head[:4] in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe",
                    b"\xca\xfe\xba\xbf", b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce"):
        return "macho"
    if head[:2] == b"MZ" and len(head) >= 0x40:
        at = int.from_bytes(head[0x3C:0x40], "little")
        if head[at:at + 4] == b"PE\0\0":
            return "pe"
    return None


def upx_packed(head: bytes) -> bool:
    """Whether UPX packed this file. Its stub writes "UPX!" into the first
    few hundred bytes of an ELF (offset 324 on linux-x64-glibc), and names a
    PE's sections UPX0 and UPX1."""
    if b"UPX!" in head:
        return True
    return head[:2] == b"MZ" and b"UPX0" in head and b"UPX1" in head


def head_of(path: Path) -> bytes:
    with path.open("rb") as fh:
        return fh.read(4096)


# ---------------------------------------------------------------------------
# Measuring
# ---------------------------------------------------------------------------

def measure(target: str, binary: Path, shipped: Path | None,
            problems: list[str]) -> Measured | None:
    for p in (binary, shipped):
        if p is not None and not p.is_file():
            problems.append(f"{p} does not exist")
    if problems:
        return None
    head = head_of(binary)
    found = kind_of(head)
    if found is None:
        problems.append(f"{binary} is not an executable this knows: it starts {head[:4]!r}")
        return None
    if upx_packed(head):
        problems.append(f"{binary} is packed by UPX already. The unpacked size is read before "
                        "UPX runs: the binary as the linker wrote it first, then the copy "
                        "that ships")
        return None
    unpacked = binary.stat().st_size
    packed: int | str = NOT_PACKED
    if shipped is not None and shipped.resolve() != binary.resolve():
        size = shipped.stat().st_size
        if upx_packed(head_of(shipped)):
            packed = size
        elif size != unpacked:
            problems.append(f"{shipped} is {size} bytes and {binary} is {unpacked}, and UPX did "
                            "not pack it: the file that ships is neither the binary nor a "
                            "packed copy of it")
            return None
    return Measured(target, found, unpacked, packed)


def measure_bundle(bundle: Path, problems: list[str]) -> list[Measured]:
    """Every native library in an .aab (base/lib/<abi>/) or an .apk (lib/<abi>/),
    summed per ABI: all of it is what a device of that ABI installs."""
    if not bundle.is_file():
        problems.append(f"{bundle} does not exist")
        return []
    try:
        zf = zipfile.ZipFile(bundle)
    except zipfile.BadZipFile:
        problems.append(f"{bundle} is not a zip, so not an .aab or an .apk")
        return []
    sizes: dict[str, int] = {}
    with zf:
        for info in zf.infolist():
            m = re.fullmatch(r"(?:base/)?lib/([^/]+)/[^/]+\.so", info.filename)
            if not m:
                continue
            with zf.open(info) as fh:
                if fh.read(4) != b"\x7fELF":
                    problems.append(f"{info.filename} in {bundle.name} is not an ELF library")
            sizes[m.group(1)] = sizes.get(m.group(1), 0) + info.file_size
    if not sizes and not problems:
        problems.append(f"{bundle} carries no native library at all")
    return [Measured(f"android/{abi}", "so", size, NOT_PACKED)
            for abi, size in sorted(sizes.items())]


# ---------------------------------------------------------------------------
# Holding a measurement to its line
# ---------------------------------------------------------------------------

def judge(m: Measured, line: Line | None, budget: str) -> tuple[list[str], list[str]]:
    """(what is wrong with `m` against its line, what is worth saying): both
    empty when it is within WARN_AT."""
    if line is None:
        return [f"{m.key} has no line in {budget}. A binary that ships has a budget: add the "
                "measured line below"], []
    out, said = [], []
    if line.kind != m.kind:
        out.append(f"{m.key}: its line says {line.kind} and the file is {m.kind}")
    wrong, warned = held(m.key, "unpacked", m.unpacked, line.unpacked)
    out += wrong
    said += warned
    if line.packed == NOT_PACKED and m.packed != NOT_PACKED:
        out.append(f"{m.key}: the file that ships is packed by UPX, and its line says UPX does "
                   "not apply (-). Did [upx] enabled change? Then so does the line")
    elif line.packed != NOT_PACKED and m.packed == NOT_PACKED:
        out.append(f"{m.key}: its line has a packed size and the file that ships is not packed. "
                   "Either [upx] changed and the line goes with it, or UPX declined -- its "
                   "own step says why")
    elif m.packed != NOT_PACKED:
        wrong, warned = held(m.key, "packed", m.packed, line.packed)
        out += wrong
        said += warned
    return out, said


def held(key: str, column: str, got: int, want: int | None) -> tuple[list[str], list[str]]:
    """(the failure, the warning) of one column against its line."""
    if want is None:
        return [f"{key}: {column} is ? -- nobody has measured it yet. This run has: "
                "paste the measured line below over it"], []
    quiet_low, quiet_high = bounds(want, WARN_AT)
    low, high = bounds(want, FAIL_AT)
    pct = f"{(got - want) * 100 / want:+.2f}%"
    rule = f"{float(WARN_AT):.1%} is said, {float(FAIL_AT):.0%} fails"
    if got > high:
        return [f"{key}: {column} {got} bytes is over its budget of {high} ({pct} against its "
                f"line's {want}; {rule}). Growing is allowed too: it is a decision, and the "
                "line is where it is written down -- in the commit that grows it"], []
    if got < low:
        return [f"{key}: {column} {got} bytes is under {low}, the least its line allows ({pct} "
                f"against {want}; {rule}). The line is stale, and a stale line lets the next "
                "growth through without a word: write the smaller number down"], []
    if got > quiet_high:
        return [], [f"{key}: {column} {got} bytes is {pct} against its line's {want}, more "
                    f"than {float(WARN_AT):.1%}; at {float(FAIL_AT):.0%} it fails. If the "
                    "growth is meant, write the new number down in the commit that makes it"]
    if got < quiet_low:
        return [], [f"{key}: {column} {got} bytes is {pct} against its line's {want}, more "
                    f"than {float(WARN_AT):.1%} under it. The line is going stale, and a stale "
                    "line hides the next growth: write the smaller number down"]
    return [], []


def verdict(measured: list[Measured], problems: list[str], path: Path, mode: str,
            bundle: bool = False) -> int:
    """Print what was measured and what is wrong with it; the exit code.
    `bundle`: the measurements are an Android bundle's, so every android/
    line of the file has to have been measured too."""
    budget = str(path.relative_to(REPO)) if path.is_relative_to(REPO) else str(path)
    if not problems and not path.is_file():
        print_measured(measured, budget)
        if mode == "game":
            print(f"  skip  this game has no {budget}, so nothing is held to a size. To "
                  "have it held, commit that file with the lines above in it")
            return 0
        print(f"FAIL: {budget} is missing, and the framework's every release is held to it")
        return 1
    if not problems:
        try:
            lines = load(path)
        except BudgetError as e:
            problems.append(str(e))
    warnings: list[str] = []
    if not problems:
        for m in measured:
            wrong, warned = judge(m, lines.get(m.key), budget)
            problems += wrong
            warnings += warned
            if not wrong and not warned:
                print(f"  ok    {m.key}: {within(m, lines[m.key])}")
        if bundle:
            seen = {m.key for m in measured}
            problems += [f"{key}: the bundle carries no {key.split('/', 1)[1]} library, and "
                         f"{budget} has a line for one" for key in lines
                         if key.startswith("android/") and key not in seen]
    for w in warnings:
        print(f"WARN: {w}")
        # On the run's summary page too, where a passing job's log is never read.
        if os.environ.get("GITHUB_ACTIONS") == "true":
            print(f"::warning title=Size budget::{w}")
    for p in problems:
        print(f"FAIL: {p}")
    print_measured(measured, budget)
    return 1 if problems else 0


def within(m: Measured, line: Line) -> str:
    low, high = bounds(line.unpacked)
    said = f"{m.unpacked} bytes, within {low}..{high}"
    if m.packed != NOT_PACKED:
        plow, phigh = bounds(line.packed)
        said += f"; packed {m.packed}, within {plow}..{phigh}"
    return said


def print_measured(measured: list[Measured], budget: str) -> None:
    if not measured:
        return
    sha = commit()
    print(f"measured, in {budget}'s own format:")
    for m in measured:
        print(render(m, sha))


# ---------------------------------------------------------------------------
# The file on its own
# ---------------------------------------------------------------------------

def required_keys(targets: list[str]) -> list[str]:
    keys = []
    for t in targets:
        if t in NOT_SHIPPED:
            continue
        keys += [f"android/{abi}" for abi in ANDROID_ABIS] if t == "android" else [t]
    return keys


def check_file(lines: dict[str, Line], known: dict[str, str], enabled: list[str],
               packed: list[str]) -> tuple[list[str], list[str]]:
    """(problems, notes) of a parsed budget against the project: `known` maps
    every target to its family, `enabled` is what [targets] builds and `packed`
    what [upx] packs."""
    problems, notes = [], []
    family = {**known, **{f"android/{abi}": "android" for abi in ANDROID_ABIS}}
    for key, line in lines.items():
        if key not in family or key in NOT_SHIPPED:
            why = f" -- {NOT_SHIPPED[key]}" if key in NOT_SHIPPED else ""
            problems.append(f"line {line.number}: {key} is not a binary any target ships{why}")
            continue
        want = FAMILY_KIND[family[key]]
        if line.kind != want:
            problems.append(f"line {line.number}: {key} is {want}, and its line says {line.kind}")
        target = "android" if key.startswith("android/") else key
        if target in packed and line.packed == NOT_PACKED:
            problems.append(f"line {line.number}: [upx] packs {target}, and its line has no "
                            "packed size (-). A number, or ? until CI measures it")
        if target not in packed and line.packed != NOT_PACKED:
            problems.append(f"line {line.number}: {target} is not in [upx] enabled, and its "
                            "line has a packed size. - where UPX does not apply")
        if line.unpacked is None or line.packed is None:
            notes.append(key)
    for key in required_keys(enabled):
        if key not in lines:
            problems.append(f"{key} has no line, and it ships. Its release job prints the line "
                            "to add; ? holds the place until it runs")
    return problems, notes


def project() -> tuple[dict[str, str], list[str], list[str]]:
    """Every target with its family, the enabled ones, and the packed ones,
    from tools/configure.py and this project's .toml."""
    sys.path.insert(0, str(REPO / "tools"))
    try:
        import configure
    finally:
        sys.path.remove(str(REPO / "tools"))
    cfg = configure.load_config()
    configure.validate(cfg, strict_release=False)
    targets = configure.expand_targets(cfg["targets"]["enabled"], cfg["targets"]["disabled"])
    return ({t: fam for t, (fam, _) in configure.TARGETS.items()}, targets,
            configure.expand_upx(cfg, targets))


def run_check_file(path: Path, mode: str) -> int:
    budget = str(path.relative_to(REPO)) if path.is_relative_to(REPO) else str(path)
    if not path.is_file():
        if mode == "game":
            print(f"  skip  this game has no {budget}: its release jobs print the lines that "
                  "would start one")
            return 0
        print(f"FAIL: {budget} is missing, and the framework's every release is held to it")
        return 1
    try:
        lines = load(path)
    except BudgetError as e:
        print(f"FAIL: {e}")
        return 1
    known, enabled, packed = project()
    problems, notes = check_file(lines, known, enabled, packed)
    for p in problems:
        print(f"FAIL: {budget}: {p}")
    if notes:
        print(f"  note  not measured yet (?), and failing the job that builds each until "
              f"its line is pasted: {', '.join(notes)}")
    if not problems:
        print(f"  ok    {budget}: {len(lines)} lines, one for each binary that ships")
    return 1 if problems else 0


# ---------------------------------------------------------------------------
# Updating from a CI log
# ---------------------------------------------------------------------------

# A line render() wrote, anywhere in a log line: `gh run view --log` puts the
# job, the step and a timestamp in front of it.
MEASURED_RE = re.compile(
    r"(?:^|\s)(?P<key>[a-z0-9][a-z0-9_.-]*(?:/[a-z0-9_.-]+)?)\s+(?P<kind>" + "|".join(KINDS)
    + r")\s+(?P<unpacked>[0-9]+)\s+(?P<packed>[0-9]+|-)"
    r"\s+# measured (?:at (?P<sha>[0-9a-f]{7})|here)\s*$")


def measured_in(text: str) -> tuple[dict[str, Measured], str]:
    """Every measured line in a log, by key, and the commit they were measured
    on ("" for a checkout with none)."""
    found: dict[str, Measured] = {}
    shas: set[str] = set()
    for raw in text.splitlines():
        m = MEASURED_RE.search(raw.rstrip())
        if not m:
            continue
        packed = m["packed"] if m["packed"] == NOT_PACKED else int(m["packed"])
        got = Measured(m["key"], m["kind"], int(m["unpacked"]), packed)
        if m["key"] in found and found[m["key"]] != got:
            raise BudgetError(f"the log measured {m['key']} twice, differently: "
                              f"{found[m['key']]} and {got}")
        found[m["key"]] = got
        shas.add(m["sha"] or "")
    if len(shas) > 1:
        named = ", ".join(sorted(sha or "here" for sha in shas))
        raise BudgetError(f"the log holds measurements of {len(shas)} commits ({named}): "
                          "one run, one commit")
    return found, (shas.pop() if shas else "")


def update(path: Path, logs: list[str]) -> int:
    text = "".join(sys.stdin.read() if log == "-" else Path(log).read_text(
        encoding="utf-8", errors="replace") for log in logs)
    try:
        lines = load(path)
        found, sha = measured_in(text)
    except BudgetError as e:
        print(f"FAIL: {e}")
        return 1
    if not found:
        print("FAIL: no measured line in the log. Each one ends in \"# measured at <commit>\"")
        return 1
    out = path.read_text(encoding="utf-8").splitlines()
    changed = []
    for key, m in found.items():
        new = render(m, sha)
        if key in lines:
            at = lines[key].number - 1
            if out[at] != new:
                changed.append(key)
            out[at] = new
        else:
            out.append(new)
            changed.append(key + " (new)")
    path.write_text("\n".join(out) + "\n", encoding="utf-8")
    left = [k for k in lines if k not in found]
    print(f"  ok    {len(found)} lines from the log, {len(changed)} of them changed: "
          + (", ".join(changed) or "none"))
    if left:
        print(f"  note  the log measured nothing for: {', '.join(left)} -- a fast-lane run "
              "builds only some targets")
    return 0


# ---------------------------------------------------------------------------
# The command line
# ---------------------------------------------------------------------------

def mode_of() -> str:
    """framework or game, by the rule tools/rmp.py uses."""
    sys.path.insert(0, str(REPO / "tools"))
    try:
        import rmp
    finally:
        sys.path.remove(str(REPO / "tools"))
    return rmp.mode_of(REPO)


def main(argv: list[str]) -> int:
    path = REPO / BUDGET
    if argv[:1] == ["--budget"] and len(argv) >= 2:
        path, argv = Path(argv[1]).resolve(), argv[2:]
    if argv[:1] == ["--check-file"] and len(argv) == 1:
        return run_check_file(path, mode_of())
    if argv[:1] == ["--update"] and len(argv) >= 2:
        return update(path, argv[1:])
    if len(argv) == 2 and argv[0] == "android":
        problems: list[str] = []
        measured = measure_bundle(Path(argv[1]), problems)
        return verdict(measured, problems, path, mode_of(), bundle=True)
    if len(argv) in (2, 3) and not argv[0].startswith("-") and argv[0] != "android":
        problems = []
        binary = Path(argv[1])
        shipped = Path(argv[2]) if len(argv) == 3 else None
        m = measure(argv[0], binary, shipped, problems)
        return verdict([m] if m else [], problems, path, mode_of())
    raise Usage("usage: size_check.py [--budget FILE] TARGET BINARY [SHIPPED]\n"
                "       size_check.py [--budget FILE] android BUNDLE\n"
                "       size_check.py [--budget FILE] --check-file\n"
                "       size_check.py [--budget FILE] --update LOG...")


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except Usage as e:
        print(e, file=sys.stderr)
        sys.exit(2)
