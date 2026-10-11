#!/usr/bin/env python3
"""What the tests run of the framework, held by two ratchets.

    python3 tools/coverage_report.py --lcov build/coverage/coverage.lcov \\
            --compile-commands build/coverage/compile_commands.json --out build/coverage
    ... --update      raise the floors to this run and drop what is called now

tools/coverage_check.sh builds the unit tests, the UI layout test and
tests/input_play.cpp with clang's source-based coverage, runs them, and hands
llvm-cov's per-line export (LCOV) to this. Two things are read out of it.

LINES, per file of src/rmp/, against tools/coverage_floor.txt: `path percent`,
the line coverage measured inside the pinned build image, rounded down. The
floor only rises. A file below its line fails; a file two points or more above
it fails too, asking for the line to be raised -- a ratchet that nobody has to
remember to turn is one that does not stall. A src/rmp file with lines and no
floor fails, and so does a line for a file that has none. A source file the
report does not hold at all fails, unless it has no code of its own
(cjson_impl.c: a #define and an #include of a vendored library, whose headers
clang never maps). Code that cannot run under a test -- a GPU, a store -- goes
between two markers, each a line of its own:

    // RMP_COVERAGE_OFF(<why>)
    ...
    // RMP_COVERAGE_ON

and is counted apart instead of against the file. A marker with no reason, or
an OFF with no ON, fails.

FUNCTIONS: every public function of include/rmp/ -- in namespace rmp, outside
every `detail`, not private or protected, not implicit, defaulted or deleted --
that no test calls, against tools/untested_api.txt, which only shrinks. clang
reads the headers (their AST, with a src/rmp file's flags from the compile
database) and llvm-cxxfilt demangles both sides, so a constructor's two
symbols are one name; a template is called when any instantiation of it is. A
public function no test calls and the list does not name fails: test it. A
listed function a test calls now, or that is gone, fails: remove the line.

WHERE IT GATES. Which lines a compiler maps, and which tests a machine can run
(the audio device, the test locales), differ between machines, and the numbers
were written inside the pinned image. There the floors and the list are gates;
anywhere else they are printed, and --strict makes them gates. What is the
same on every machine fails everywhere: a marker, a missing file, a missing or
stale floor line, a malformed file.

--update (image only, or --strict) rewrites the floors from this run and drops
from the list what is called now or gone. It never lowers a floor and never
adds a function: a floor that has to come down, or a function that may stay
untested, is a line somebody writes by hand and a reviewer reads.

Standard library only. Exit: 0 pass, 1 a gate failed, 2 a usage error.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pointer_check import Walker, documents  # noqa: E402  -- the AST reader it shares

REPO = Path(__file__).resolve().parent.parent
FLOOR = REPO / "tools" / "coverage_floor.txt"
UNTESTED = REPO / "tools" / "untested_api.txt"
IMAGE_MANIFEST = Path("/etc/raylib-build-image.json")
STALE = 2  # points over the floor that make it stale

SOURCES = (".c", ".cc", ".cpp")
MARKER = re.compile(r"RMP_COVERAGE_(OFF|ON)\b")
MARKER_OFF = re.compile(r"^\s*//\s*RMP_COVERAGE_OFF\((.*)\)\s*$")
MARKER_ON = re.compile(r"^\s*//\s*RMP_COVERAGE_ON\s*$")
FLOOR_LINE = re.compile(r"^(\S+)\s+(\d{1,3})$")

FUNCTIONS = {"FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl", "CXXConversionDecl",
             "CXXDestructorDecl"}
RECORDS = {"CXXRecordDecl", "ClassTemplatePartialSpecializationDecl"}
TEMPLATE = " [template]"


# ---------------------------------------------------------------------------
# The report
# ---------------------------------------------------------------------------

def read_lcov(text: str):
    """({path: {line: count}}, {function symbol: count}) out of llvm-cov's LCOV."""
    lines: dict[str, dict[int, int]] = {}
    functions: dict[str, int] = {}
    current = None
    for raw in text.splitlines():
        tag, _, value = raw.partition(":")
        if tag == "SF":
            current = lines.setdefault(value, {})
        elif tag == "DA" and current is not None:
            number, count = value.split(",")[:2]
            current[int(number)] = max(current.get(int(number), 0), int(count))
        elif tag == "FNDA":
            count, name = value.split(",", 1)
            functions[name] = max(functions.get(name, 0), int(count))
        elif tag == "end_of_record":
            current = None
    return lines, functions


def relative(path: str, root: Path) -> str | None:
    """`path` from `root`, with forward slashes, or None when it is elsewhere."""
    p = Path(path)
    if not p.is_absolute():
        p = root / p
    try:
        return Path(os.path.realpath(p)).relative_to(Path(os.path.realpath(root))).as_posix()
    except ValueError:
        return None


# ---------------------------------------------------------------------------
# Markers, and the files with no code of their own
# ---------------------------------------------------------------------------

def markers(text: str, name: str):
    """(the line numbers between OFF and ON, the problems)."""
    excluded: set[int] = set()
    problems: list[str] = []
    opened = None
    for number, line in enumerate(text.splitlines(), 1):
        found = MARKER.search(line)
        if not found:
            if opened is not None:
                excluded.add(number)
            continue
        if found.group(1) == "OFF":
            m = MARKER_OFF.match(line)
            if m is None:
                problems.append(f"{name}:{number}: a marker is a comment line of its own: "
                                "`// RMP_COVERAGE_OFF(<why>)`")
            elif not m.group(1).strip():
                problems.append(f"{name}:{number}: RMP_COVERAGE_OFF() without a reason: "
                                "say why no test can run these lines")
            if opened is not None:
                problems.append(f"{name}:{number}: RMP_COVERAGE_OFF inside the one opened at "
                                f"line {opened}: close it with RMP_COVERAGE_ON first")
            else:
                opened = number
        else:
            if MARKER_ON.match(line) is None:
                problems.append(f"{name}:{number}: a marker is a comment line of its own: "
                                "`// RMP_COVERAGE_ON`")
            if opened is None:
                problems.append(f"{name}:{number}: RMP_COVERAGE_ON with no RMP_COVERAGE_OFF "
                                "before it")
            opened = None
    if opened is not None:
        problems.append(f"{name}:{opened}: RMP_COVERAGE_OFF is never closed: end the "
                        "lines it takes out with `// RMP_COVERAGE_ON`")
    return excluded, problems


COMMENTS = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def has_code(text: str) -> bool:
    """Anything but comments and preprocessor lines: a file that only #defines
    and #includes a vendored library has no line of its own to cover."""
    for line in COMMENTS.sub("", text).splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            return True
    return False


# ---------------------------------------------------------------------------
# Lines, against the floor
# ---------------------------------------------------------------------------

@dataclass
class Measure:
    lines: int = 0
    covered: int = 0
    excluded: int = 0

    def percent(self) -> float:
        return 100.0 * self.covered / self.lines if self.lines else 100.0

    def floor(self) -> int:
        """What --update writes: the percentage, rounded down, exactly."""
        return self.covered * 100 // self.lines if self.lines else 100


def read_list_file(path: Path, parse):
    """Lines of `path` without comments, through `parse(line) -> (key, value) | None`;
    (entries, problems). A key twice, or a line `parse` refuses, is a problem."""
    entries: dict = {}
    problems: list[str] = []
    if not path.is_file():
        return entries, problems
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        got = parse(line)
        if got is None:
            problems.append(f"{path.name}:{number}: not understood: `{line}`")
            continue
        key, value = got
        if key in entries:
            problems.append(f"{path.name}:{number}: `{key}` is listed twice")
        entries[key] = value
    return entries, problems


def parse_floor(line: str):
    m = FLOOR_LINE.match(line)
    if m is None or not m.group(1).startswith("src/rmp/") or int(m.group(2)) > 100:
        return None
    return m.group(1), int(m.group(2))


def measure_files(root: Path, lines_by_file: dict[str, dict[int, int]]):
    """({src/rmp file: Measure}, the problems that hold on every machine)."""
    problems: list[str] = []
    excluded: dict[str, set[int]] = {}
    on_disk = sorted(p for p in (root / "src" / "rmp").rglob("*") if p.is_file())
    for path in on_disk:
        rel = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        lines, found = markers(text, rel)
        excluded[rel] = lines
        problems.extend(found)

    measured: dict[str, Measure] = {}
    for sf, lines in lines_by_file.items():
        rel = relative(sf, root)
        if rel is None or not rel.startswith("src/rmp/"):
            continue
        m = measured.setdefault(rel, Measure())
        out = excluded.get(rel, set())
        for number, count in lines.items():
            if number in out:
                m.excluded += 1
            else:
                m.lines += 1
                m.covered += 1 if count > 0 else 0

    for path in on_disk:
        rel = path.relative_to(root).as_posix()
        if path.suffix in SOURCES and rel not in measured and has_code(path.read_text(
                encoding="utf-8", errors="replace")):
            problems.append(f"{rel} is not in the coverage report: no test binary holds its "
                            "code. The tests link librmp.a whole under RMP_COVERAGE, so a "
                            "file of src/rmp that is not there is not in the library")
    return measured, problems


@dataclass
class Verdicts:
    """What a run found: `fatal` fails wherever it runs, `gated` only where the
    numbers mean something (the image, or --strict)."""
    fatal: list[str] = field(default_factory=list)
    gated: list[str] = field(default_factory=list)
    below: list[str] = field(default_factory=list)  # the gated ones --update cannot mend


def judge_floors(measured: dict[str, Measure], floors: dict[str, int], out: Verdicts,
                 rows: list):
    for rel in sorted(set(measured) | set(floors)):
        m = measured.get(rel)
        want = floors.get(rel)
        if m is None or m.lines == 0:
            if want is not None:
                why = ("is not in the coverage report" if m is None
                       else "has no line left to cover")
                out.fatal.append(f"{rel} {why}, and tools/coverage_floor.txt has a line for "
                                 "it: remove the line")
            continue
        verdict = "ok"
        if want is None:
            verdict = "no floor"
            out.fatal.append(f"{rel} has {m.lines} lines and no floor: add "
                             f"`{rel} {m.floor()}` to tools/coverage_floor.txt")
        elif m.covered * 100 < want * m.lines:
            verdict = "below"
            out.below.append(f"{rel}: {m.percent():.2f}% of its lines run, below its floor "
                             f"of {want}%. Test what the change left untested")
            out.gated.append(out.below[-1])
        elif m.covered * 100 >= (want + STALE) * m.lines:
            verdict = "stale"
            out.gated.append(f"{rel}: {m.percent():.2f}% is {STALE} points or more over its "
                             f"floor of {want}%: raise the line to `{rel} {m.floor()}`")
        rows.append((rel, m, want, verdict))


# ---------------------------------------------------------------------------
# The public functions, out of clang's AST
# ---------------------------------------------------------------------------

@dataclass
class Api:
    key: str  # what tools/untested_api.txt says
    name: str  # qualified, no signature
    mangled: str | None  # None: matched by name (a template)
    where: str


def signature(qual_type: str) -> str:
    """`bool (std::string_view, const Options &) const noexcept` -> the part a
    reader tells overloads apart by: `(std::string_view, const Options &) const`."""
    depth = 0
    start = None
    for i, ch in enumerate(qual_type):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == "(" and depth == 0:
            start = i
            break
    if start is None:
        return "()"
    depth = 0
    for i in range(start, len(qual_type)):
        if qual_type[i] == "(":
            depth += 1
        elif qual_type[i] == ")":
            depth -= 1
            if depth == 0:
                params = qual_type[start:i + 1]
                tail = qual_type[i + 1:]
                tail = re.sub(r"\s*->.*$", "", tail)  # a trailing return type
                tail = re.sub(r"\s*noexcept(\(.*\))?", "", tail)
                return params + tail.rstrip()
    return qual_type[start:]


class ApiWalker(Walker):
    """Every public function the headers declare, in the order clang printed
    them -- the location state is pointer_check's Walker's."""

    def __init__(self, include: Path):
        super().__init__(include, {})
        self.found: dict[str, Api] = {}

    def own(self, node):
        """The node's own locations, not its children's: (where, ours)."""
        at, ours = None, False
        for key, value in node.items():
            if key == "loc":
                self.location(value)
                at, ours = f"{self.shown()}:{self.line}", self.ours()
            elif key != "inner":
                self.track(value)
        return at or f"{self.shown()}:{self.line}", ours

    def visit(self, node, scope: list[str], public: bool, template: bool = False):
        if not isinstance(node, dict):
            return
        where, ours = self.own(node)
        kind = node.get("kind", "")
        name = node.get("name", "")
        if not name.startswith("operator"):
            name = re.sub(r"<.*>$", "", name)  # a class template's constructor
        inner = node.get("inner", [])
        outside = self.contexts.get(node.get("parentDeclContextId", ""))
        if outside is not None:
            scope = outside[0].split("::") if outside[0] else []
            if (kind in FUNCTIONS or kind == "FunctionTemplateDecl") and outside[2]:
                self.track(inner)  # a member defined outside its class: read there
                return
        qualified = "::".join([*scope, name] if name else scope)

        if kind == "NamespaceDecl":
            inside = public and name not in ("detail", "")
            self.contexts[node.get("id", "")] = (qualified, inside, False)
            for child in inner:
                self.visit(child, [*scope, name or "(anonymous)"], inside, template)
            return
        if kind in ("LinkageSpecDecl", "ExportDecl"):
            for child in inner:
                self.visit(child, scope, public, template)
            return
        if kind in RECORDS and not node.get("isImplicit", False):
            access = "private" if node.get("tagUsed") == "class" else "public"
            inner_scope = [*scope, name or "(anonymous)"]
            self.contexts[node.get("id", "")] = ("::".join(inner_scope), public, True)
            nested = template or kind == "ClassTemplatePartialSpecializationDecl"
            for child in inner:
                if child.get("kind") == "AccessSpecDecl":
                    self.track(child)
                    access = child.get("access", access)
                elif child.get("kind") == "FriendDecl":
                    # A friend declared in a class is callable by anyone: it
                    # belongs to the namespace around the class.
                    self.own(child)
                    for friend in child.get("inner", []):
                        self.visit(friend, scope, public, nested)
                else:
                    self.visit(child, inner_scope, public and access == "public", nested)
            return
        if kind == "ClassTemplateDecl":
            for child in inner:
                self.visit(child, scope, public, True)
            return
        if kind == "FunctionTemplateDecl":
            # The pattern comes first; what follows it is instantiations.
            seen = False
            for child in inner:
                if child.get("kind") in FUNCTIONS and not seen:
                    self.visit(child, scope, public, True)
                    seen = True
                else:
                    self.track(child)
            return
        if kind in FUNCTIONS:
            if (ours and public and not node.get("isImplicit", False)
                    and not node.get("explicitlyDefaulted") and not node.get("explicitlyDeleted")
                    and not node.get("pure", False)):
                self.record(node, qualified, where, template)
            self.track(inner)  # the parameters and the body
            return
        self.track(inner)

    def record(self, node, qualified: str, where: str, template: bool):
        mangled = None if template else node.get("mangledName")
        if mangled is None:
            key = qualified + TEMPLATE
        else:
            key = qualified + signature(node.get("type", {}).get("qualType", ""))
        if key not in self.found:
            self.found[key] = Api(key, qualified, mangled, where)


def api_flags(compile_commands: Path | None, root: Path, include: Path) -> list[str]:
    """The include paths, macros and standard of a src/rmp file's compile: the
    same list the framework is built with, and no second copy of it here."""
    if compile_commands is None:
        return ["-std=c++20", f"-I{include}"]
    entries = json.loads(compile_commands.read_text())
    for entry in entries:
        rel = relative(entry["file"], root)
        if rel and rel.startswith("src/rmp/") and rel.endswith(".cpp"):
            args = entry.get("arguments") or shlex.split(entry["command"])
            keep: list[str] = []
            i = 1
            while i < len(args):
                a = args[i]
                if a in ("-I", "-isystem", "-iquote", "-D", "-U", "-include") and i + 1 < len(args):
                    keep += [a, args[i + 1]]
                    i += 2
                    continue
                if a.startswith(("-I", "-D", "-U", "-std=", "-isystem", "-iquote")):
                    keep.append(a)
                i += 1
            return keep
    raise SystemExit(f"FAIL: {compile_commands} has no src/rmp/*.cpp to take the flags from")


def public_api(cxx: str, include: Path, flags: list[str]) -> dict[str, Api]:
    headers = sorted(include.glob("rmp/*.h"))
    if not headers:
        raise SystemExit(f"FAIL: no headers under {include}/rmp")
    with tempfile.TemporaryDirectory() as tmp:
        unit = Path(tmp) / "api.cpp"
        unit.write_text("".join(f"#include <rmp/{h.name}>\n" for h in headers))
        got = subprocess.run([cxx, "-fsyntax-only", "-Xclang", "-ast-dump=json", "-Xclang",
                              "-ast-dump-filter=rmp", *flags, str(unit)],
                             capture_output=True, text=True, cwd=include.parent)
    if got.returncode != 0:
        print("\n".join("      " + line for line in got.stderr.splitlines()[:12]))
        raise SystemExit(f"FAIL: {cxx} could not read the public headers")
    walker = ApiWalker(include)
    for doc in documents(got.stdout):
        walker.visit(doc, [], True)
    return walker.found


# ---------------------------------------------------------------------------
# Who is called
# ---------------------------------------------------------------------------

def demangle(cxxfilt: str, names: list[str]) -> dict[str, str]:
    if not names:
        return {}
    got = subprocess.run([cxxfilt], input="\n".join(names) + "\n", capture_output=True,
                         text=True, check=True)
    out = got.stdout.splitlines()
    if len(out) != len(names):
        raise SystemExit(f"FAIL: {cxxfilt} gave {len(out)} names for {len(names)}")
    return dict(zip(names, out))


OPERATOR = re.compile(r"\boperator(\(\)|\[\]|->\*?|<=>|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||\+\+|--"
                      r"|[-+*/%^&|~!=<>,]=?|\s+(?:new|delete)(?:\[\])?|\s+[^(]+?(?=\())")


def plain_name(demangled: str) -> str:
    """`Player& rmp::Scene::spawn<Player>(int) const` -> `rmp::Scene::spawn`: the
    qualified name with every template argument list and the return type gone."""
    s = re.sub(r"\[abi:[^\]]*\]", "", demangled)
    s = re.sub(r" \[clone [^\]]*\]$", "", s)
    # Not a parameter list: `(anonymous namespace)::Marker& rmp::global<...>()`.
    s = re.sub(r"\(anonymous [a-z]+\)", "anonymous", s)
    saved: list[str] = []

    def keep(m):
        saved.append(m.group(0))
        return f"operator@{len(saved) - 1}@"

    s = OPERATOR.sub(keep, s)
    while True:
        shorter = re.sub(r"<[^<>]*>", "", s)
        if shorter == s:
            break
        s = shorter
    cut = s.find("(")
    head = s if cut < 0 else s[:cut]
    m = re.search(r"([\w:~@]+)\s*$", head)
    name = m.group(1) if m else head.strip()
    return re.sub(r"operator@(\d+)@", lambda m: saved[int(m.group(1))], name)


def symbol(name: str) -> str:
    """A profile name without the `file:` llvm puts before a local function's."""
    head, sep, tail = name.rpartition(":")
    return tail if sep and tail.startswith("_Z") else name


def uncalled(api: dict[str, Api], functions: dict[str, int], cxxfilt: str) -> list[Api]:
    called = [symbol(n) for n, count in functions.items() if count > 0]
    mangled = [a.mangled for a in api.values() if a.mangled]
    names = demangle(cxxfilt, sorted(set(called) | set(mangled)))
    called_exact = {names[n] for n in called}
    called_plain = {plain_name(names[n]) for n in called}
    out = []
    for a in api.values():
        hit = (names[a.mangled] in called_exact) if a.mangled else (a.name in called_plain)
        if not hit:
            out.append(a)
    return sorted(out, key=lambda a: a.key)


def judge_untested(api: dict[str, Api], missing: list[Api], listed: dict[str, None],
                   out: Verdicts):
    missing_keys = {a.key for a in missing}
    for a in missing:
        if a.key not in listed:
            out.gated.append(f"{a.key} ({a.where}) is public and no test calls it: test it "
                             "(tools/untested_api.txt only shrinks)")
    for key in sorted(listed):
        if key not in api:
            out.fatal.append(f"tools/untested_api.txt lists `{key}`, and it is no public "
                             "function any more: remove the line")
        elif key not in missing_keys:
            out.gated.append(f"tools/untested_api.txt lists `{key}`, and a test calls it "
                             "now: remove the line")


# ---------------------------------------------------------------------------
# Writing it down
# ---------------------------------------------------------------------------

FLOOR_HEADER = """\
# Line coverage per file of src/rmp/, in percent, measured inside the pinned
# build image by tools/coverage_check.sh and rounded down. The floor only rises:
# tools/coverage_report.py fails a file below its line, and one STALE points or
# more above it. `bash tools/coverage_check.sh --update` in the image raises the
# lines to what it measured; lowering one is an edit somebody has to explain.
"""

UNTESTED_HEADER = """\
# Public functions of include/rmp/ that no test calls: the debt, one per line,
# exactly as tools/coverage_report.py prints them. It only shrinks -- a new
# public function arrives with a test, and a line whose function a test now
# calls, or that is gone, fails until it is removed (--update removes it).
"""


def write_floor(path: Path, measured: dict[str, Measure]):
    body = "".join(f"{rel:32} {m.floor()}\n" for rel, m in sorted(measured.items()) if m.lines)
    path.write_text(FLOOR_HEADER.replace("STALE", str(STALE)) + body)


def write_untested(path: Path, keys):
    path.write_text(UNTESTED_HEADER + "".join(f"{k}\n" for k in sorted(keys)))


def summary(rows, total: Measure, api, missing, gate: bool, failed: list[str]) -> str:
    out = ["## Coverage of src/rmp\n",
           f"**{total.percent():.2f}%** of {total.lines} lines "
           f"({total.covered} run{f', {total.excluded} marked out' if total.excluded else ''}); "
           f"{len(missing)} of {len(api)} public functions are called by no test.",
           "" if gate else "\n_Outside the pinned image: the floors and the list were printed, "
           "not enforced._", "",
           "| file | lines | run | % | floor | |", "|---|---:|---:|---:|---:|---|"]
    for rel, m, want, verdict in rows:
        out.append(f"| `{rel}` | {m.lines} | {m.covered} | {m.percent():.2f} | "
                   f"{'' if want is None else want} | {'' if verdict == 'ok' else verdict} |")
    if failed:
        out += ["", "### Failed", ""] + [f"- {f}" for f in failed]
    out += ["", f"### Public functions no test calls ({len(missing)})", ""]
    out += [f"- `{a.key}` -- {a.where}" for a in missing]
    return "\n".join(out) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lcov", type=Path, required=True)
    ap.add_argument("--compile-commands", type=Path)
    ap.add_argument("--root", type=Path, default=REPO, help="the tree whose src/rmp is measured")
    ap.add_argument("--include", type=Path, help="the folder holding rmp/*.h (default ROOT/include)")
    ap.add_argument("--floor", type=Path, default=FLOOR)
    ap.add_argument("--untested", type=Path, default=UNTESTED)
    ap.add_argument("--cxx", default=os.environ.get("RMP_CLANGXX") or "clang++")
    ap.add_argument("--cxxfilt", default="llvm-cxxfilt")
    ap.add_argument("--out", type=Path, help="where coverage.json, summary.md and the measured "
                                             "untested_api.txt go")
    ap.add_argument("--strict", action="store_true", help="gate outside the build image too")
    ap.add_argument("--update", action="store_true", help="raise the floors, shrink the list")
    args = ap.parse_args(argv)

    root = args.root.resolve()
    include = (args.include or root / "include").resolve()
    for tool in (args.cxx, args.cxxfilt):
        if shutil.which(tool) is None:
            print(f"FAIL: {tool} is not on PATH")
            return 1
    gate = IMAGE_MANIFEST.is_file() or args.strict

    lines_by_file, functions = read_lcov(args.lcov.read_text())
    verdicts = Verdicts()
    # What --update cannot mend: it would write numbers out of a run that is wrong.
    measured, structural = measure_files(root, lines_by_file)
    floors, problems = read_list_file(args.floor, parse_floor)
    structural += problems
    listed, problems = read_list_file(args.untested, lambda line: (line, None))
    structural += problems
    verdicts.fatal += structural
    if not args.floor.is_file():
        verdicts.fatal.append(f"{args.floor} does not exist: --update in the image writes it")

    rows: list = []
    judge_floors(measured, floors, verdicts, rows)
    api = public_api(args.cxx, include, api_flags(args.compile_commands, root, include))
    missing = uncalled(api, functions, args.cxxfilt)
    judge_untested(api, missing, listed, verdicts)

    total = Measure()
    for m in measured.values():
        total.lines += m.lines
        total.covered += m.covered
        total.excluded += m.excluded

    def label(verdict: str) -> str:
        if verdict == "ok":
            return "ok"
        if args.update and verdict != "below":
            return "note"  # what --update is about to write
        return "FAIL" if gate or verdict == "no floor" else "note"

    print("== coverage: lines of src/rmp ==")
    for rel, m, want, verdict in rows:
        mark = f"  {label(verdict):5}"
        extra = f" ({m.excluded} marked out)" if m.excluded else ""
        print(f"{mark} {rel:32} {m.covered:5}/{m.lines:<5} {m.percent():6.2f}%  floor "
              f"{'-' if want is None else want}{extra}")
    print(f"  TOTAL {'':33}{total.covered:5}/{total.lines:<5} {total.percent():6.2f}%"
          + (f"  ({total.excluded} lines marked out)" if total.excluded else ""))
    print(f"== coverage: {len(missing)} of {len(api)} public functions no test calls "
          f"({len(listed)} listed in {args.untested.name}) ==")

    failed = verdicts.fatal + (verdicts.gated if gate else [])
    # Under --update what it is about to mend is a note; what it cannot, a FAIL.
    for line in verdicts.fatal:
        print(f"{'note' if args.update and line not in structural else 'FAIL'}: {line}")
    for line in verdicts.gated:
        mends = args.update and line not in verdicts.below
        print(f"{'FAIL' if gate and not mends else 'note'}: {line}")

    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / "coverage.json").write_text(json.dumps({
            "gated": gate,
            "total": {"lines": total.lines, "covered": total.covered,
                      "excluded": total.excluded, "percent": round(total.percent(), 2)},
            "files": {rel: {"lines": m.lines, "covered": m.covered, "excluded": m.excluded,
                            "percent": round(m.percent(), 2), "floor": want}
                      for rel, m, want, _ in rows},
            "uncalled": [{"function": a.key, "where": a.where} for a in missing],
            "failed": failed,
        }, indent=1) + "\n")
        (args.out / "summary.md").write_text(summary(rows, total, api, missing, gate, failed))
        write_untested(args.out / "untested_api.txt", [a.key for a in missing])

    if args.update:
        if not gate:
            print("FAIL: --update writes numbers measured in the pinned build image, and this "
                  "is not it (--strict to write them anyway)")
            return 1
        if structural:
            print("FAIL: nothing was written: the problems above make the numbers wrong")
            return 1
        if verdicts.below:
            print("FAIL: nothing was written: --update raises floors and never lowers one")
            return 1
        write_floor(args.floor, measured)
        keep = listed.keys() & {a.key for a in missing} if args.untested.is_file() else \
            {a.key for a in missing}
        write_untested(args.untested, keep)
        print(f"  wrote {args.floor.name} and {args.untested.name}")
        return 0

    if failed:
        print(f"FAIL: {len(failed)} problem(s) above")
        return 1
    print(f"PASS: {total.percent():.2f}% of src/rmp's lines run"
          + ("" if gate else " (not the pinned image: the floors were printed, not enforced)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
