#!/usr/bin/env python3
"""The deep test campaign's own gates.

The campaign is tests/deep.cmake (property_test and the fuzz targets),
tools/fuzz.sh, tools/mutation.sh with tools/mutation_report.py, and the
floors under tests/mutation/floors/. Each of these is a promise that rots
quietly if nothing holds it, so this file holds every one, and sees each
check go red on the mistake it is there for:

  - every fuzz target has committed inputs -- a corpus and its regressions --
    and nothing committed belongs to no target; the whole corpus stays under
    2 MB, because every clone carries it;
  - tools/fuzz.sh replay fails unless every file it named ran;
  - every file of src/rmp/ is in exactly one mutation floor file (or in the
    short list of files that only compile a vendored library), and every
    floor file names sources and tests that exist;
  - the floor checker fails below a floor, fails a floor the score has left
    behind, and fails an equivalent mutant that a test kills or that is gone;
  - every public function of include/rmp/ is held by a property
    (`// covers: <function>` in a tests/property/*_prop.cpp whose code calls
    it) or waived in tests/property/waivers.txt, a list that only shrinks;
  - every rmp::test::check() description is a string literal, and no two
    alike: it is the key RC_PARAMS="reproduce=..." finds a property by; and
    no property file calls RapidCheck's rc::check(), which fails nothing;
  - Hypothesis is a skip that says so where it is not installed, and a
    failure where RMP_REQUIRE_HYPOTHESIS=1;
  - nothing under src/rmp/ or include/rmp/ throws or includes RapidCheck:
    exceptions belong to the test binaries that link it.
  - deep.yml runs on this repository only, and its Sunday cron is the one
    its plan reads as weekly; tools/finding.py makes one issue of one failure.

Run it with `rmp test property` (its first step), or alone:

    python3 -m unittest discover -s tests -p campaign_test.py -v

Standard library only; the resilience list reads the headers with clang++
(RMP_REQUIRE_CLANG=1 makes a missing one a failure, as CI does).
"""

from __future__ import annotations

import fnmatch
import functools
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / "tools"
sys.path.insert(0, str(TOOLS))


def load_tool(name: str):
    """tools/<name>.py, by its path, as `tools.<name>`: the name mutmut gives
    the file (tools/mutation.sh), which records the tests that reach a
    function under its module's name -- a test that imported it as plain
    `<name>` would reach nothing it can mutate."""
    spec = importlib.util.spec_from_file_location(f"tools.{name}", TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


cpp_text = load_tool("cpp_text")
mr = load_tool("mutation_report")

FUZZ = REPO / "tests" / "fuzz"
PROPERTY = REPO / "tests" / "property"
WAIVERS = PROPERTY / "waivers.txt"

# Every clone carries the corpus: what fuzzing finds is merged with -max_len,
# and the whole of it stays under this.
CORPUS_LIMIT = 2 * 1024 * 1024


# ---------------------------------------------------------------------------
# The fuzz targets and their inputs
# ---------------------------------------------------------------------------

def harnesses(root: Path) -> list[str]:
    return sorted(p.stem for p in root.glob("*.cpp") if p.stem != "replay_main")


def files_in(folder: Path) -> list[Path]:
    return sorted(p for p in folder.iterdir() if p.is_file()) if folder.is_dir() else []


def fuzz_problems(root: Path) -> list[str]:
    """What is wrong with a tests/fuzz/ tree, one sentence each."""
    out = []
    names = harnesses(root)
    for name in names:
        text = (root / f"{name}.cpp").read_text(encoding="utf-8")
        if not files_in(root / "corpus" / name):
            out.append(f"{name}: tests/fuzz/corpus/{name}/ has no input: seed it")
        if not files_in(root / "regressions" / name):
            out.append(f"{name}: tests/fuzz/regressions/{name}/ has no input: an input that "
                       "once failed, or one at the edge of what the parser takes")
        for needed, why in (('extern "C" int LLVMFuzzerTestOneInput(', "libFuzzer's entry"),
                            ('extern "C" int LLVMFuzzerInitialize(', "replay_main.cpp calls it"),
                            ('#include "harness.h"', "the shared start of every target")):
            if needed not in text:
                out.append(f"{name}: no `{needed}` ({why})")
    for kind in ("corpus", "regressions"):
        for folder in sorted((root / kind).iterdir()) if (root / kind).is_dir() else []:
            if folder.name not in names:
                out.append(f"tests/fuzz/{kind}/{folder.name}/: no tests/fuzz/{folder.name}.cpp "
                           "to run it")
    for extra in sorted([*root.glob("*.dict"), *root.glob("*.options")]):
        if extra.stem not in names:
            out.append(f"tests/fuzz/{extra.name}: no tests/fuzz/{extra.stem}.cpp it is for")
    for options in sorted(root.glob("*.options")):
        for number, line in enumerate(options.read_text(encoding="utf-8").splitlines(), 1):
            if line.strip() and not line.startswith("#") and not line.startswith("-"):
                out.append(f"tests/fuzz/{options.name}:{number}: {line!r} is not a libFuzzer flag")
    return out


def corpus_bytes(root: Path) -> int:
    """Bytes, not blocks: what a clone carries."""
    return sum(p.stat().st_size for kind in ("corpus", "regressions")
               for p in (root / kind).rglob("*") if p.is_file())


class FuzzTargetsTest(unittest.TestCase):

    def test_every_target_has_its_inputs(self):
        self.assertTrue(harnesses(FUZZ), "no fuzz target at all: the check would pass on nothing")
        self.assertEqual(fuzz_problems(FUZZ), [])

    def tree(self, tmp: str) -> Path:
        root = Path(tmp) / "fuzz"
        (root / "corpus" / "x").mkdir(parents=True)
        (root / "regressions" / "x").mkdir(parents=True)
        (root / "x.cpp").write_text('#include "harness.h"\n'
                                    'extern "C" int LLVMFuzzerInitialize(int *, char ***);\n'
                                    'extern "C" int LLVMFuzzerTestOneInput(const T *, S);\n')
        (root / "replay_main.cpp").write_text("int main() {}\n")
        (root / "corpus" / "x" / "seed").write_text("1 end\n")
        (root / "regressions" / "x" / "edge").write_text("")
        return root

    def test_the_check_sees_each_mistake(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.tree(tmp)
            self.assertEqual(fuzz_problems(root), [])
            cases = [
                (lambda: (root / "corpus" / "x" / "seed").unlink(), "corpus/x/ has no input"),
                (lambda: (root / "regressions" / "x" / "edge").unlink(),
                 "regressions/x/ has no input"),
                (lambda: (root / "corpus" / "gone").mkdir(), "corpus/gone/: no tests/fuzz/gone.cpp"),
                (lambda: (root / "x.cpp").write_text('#include "harness.h"\n'
                                                     'extern "C" int LLVMFuzzerTestOneInput(\n'),
                 "LLVMFuzzerInitialize"),
                (lambda: (root / "y.dict").write_text('a="b"\n'), "y.dict: no tests/fuzz/y.cpp"),
                (lambda: (root / "x.options").write_text("# fine\n-max_len=9\nmax_len=9\n"),
                 "x.options:3: 'max_len=9' is not a libFuzzer flag"),
            ]
            for break_it, says in cases:
                with self.subTest(says=says):
                    shutil.rmtree(root)
                    root = self.tree(tmp)
                    break_it()
                    found = fuzz_problems(root)
                    self.assertEqual(len(found), 1, found)
                    self.assertIn(says, found[0])

    def test_the_committed_inputs_stay_small(self):
        self.assertLess(corpus_bytes(FUZZ), CORPUS_LIMIT)
        with tempfile.TemporaryDirectory() as tmp:
            root = self.tree(tmp)
            (root / "corpus" / "x" / "big").write_bytes(b"\0" * 5000)
            # 6 + 0 + 5000: bytes, not the blocks a filesystem rounds them to.
            self.assertEqual(corpus_bytes(root), 5006)


class FuzzReplayCountsTest(unittest.TestCase):
    """tools/fuzz.sh, run on a tree where the fuzz target is a stand-in that
    says what it ran: a replay that skipped an input, or died, is a failure,
    and only every file run is a pass."""

    def setUp(self):
        if shutil.which("bash") is None:
            self.skipTest("no bash")
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "tools").mkdir()
        shutil.copy(TOOLS / "fuzz.sh", self.root / "tools" / "fuzz.sh")
        fuzz = self.root / "tests" / "fuzz"
        for kind, name in (("corpus", "a"), ("corpus", "b"), ("regressions", "c")):
            (fuzz / kind / "x").mkdir(parents=True, exist_ok=True)
            (fuzz / kind / "x" / name).write_text(name)
        (fuzz / "x.cpp").write_text("// a harness\n")
        (fuzz / "replay_main.cpp").write_text("// not a target\n")
        (self.root / "build" / "fuzz").mkdir(parents=True)

    def tearDown(self):
        self._tmp.cleanup()

    def target(self, body: str, engine: str = "replay"):
        (self.root / "build" / "fuzz" / "engine").write_text(engine + "\n")
        stub = self.root / "build" / "fuzz" / "fuzz_x"
        stub.write_text("#!/bin/sh\n" + body)
        stub.chmod(0o755)

    def fuzz(self, *argv):
        got = subprocess.run(["bash", "tools/fuzz.sh", *argv], cwd=self.root,
                             capture_output=True, text=True)
        return got.returncode, got.stdout + got.stderr

    EVERY = ('for f in "$@"; do case "$f" in -*) ;; *) echo "Executed $f" >&2 ;; esac; done\n'
             'echo ran=3\n')

    def test_every_file_run_passes(self):
        self.target(self.EVERY)
        code, out = self.fuzz("replay")
        self.assertEqual(code, 0, out)
        self.assertIn("ok    x: 3 input(s)", out)
        self.assertIn("PASS: every committed input replayed", out)

    def test_an_input_not_run_fails(self):
        self.target('n=0; for f in "$@"; do case "$f" in -*) ;; *) n=$((n+1)); '
                    '[ $n -eq 2 ] || echo "Executed $f" >&2 ;; esac; done\n')
        code, out = self.fuzz("replay")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: fuzz_x ran 2 of 3 input(s) and exited 0", out)

    def test_a_crash_fails_even_when_every_file_ran(self):
        self.target(self.EVERY + "echo '==1==ERROR: AddressSanitizer: heap-buffer-overflow'\nexit 1\n")
        code, out = self.fuzz("replay")
        self.assertEqual(code, 1, out)
        self.assertIn("AddressSanitizer", out)
        self.assertIn("ran 3 of 3 input(s) and exited 1", out)

    def test_named_files_and_targets(self):
        self.target(self.EVERY)
        code, out = self.fuzz("replay", "x", "tests/fuzz/corpus/x/a")
        self.assertEqual(code, 0, out)
        self.assertIn("ok    x: 1 input(s)", out)
        code, out = self.fuzz("replay", "nope")
        self.assertEqual(code, 2, out)
        self.assertIn("no fuzz target called 'nope'", out)

    def test_a_target_that_is_not_the_last_is_known(self):
        """The name is looked for in the whole list of targets. Asked of a
        `targets | grep -q` under pipefail, grep stopped at the first match
        and the next name was written into a closed pipe: every target but
        the last read as "no fuzz target called ..." -- the nightly job's
        run and merge with them (seen: 200 of 200 with ten harnesses)."""
        fuzz = self.root / "tests" / "fuzz"
        for other in ("y", "z"):
            (fuzz / f"{other}.cpp").write_text("// another harness\n")
        self.target(self.EVERY)
        for _ in range(3):
            code, out = self.fuzz("replay", "x")
            self.assertEqual(code, 0, out)
            self.assertIn("ok    x: 3 input(s)", out)
        self.target('echo "$@"\n', engine="libfuzzer")
        code, out = self.fuzz("run", "x", "1")
        self.assertEqual(code, 0, out)
        self.assertIn("-max_total_time=1", out)
        code, out = self.fuzz("replay", "w")
        self.assertEqual(code, 2, out)
        self.assertIn("no fuzz target called 'w'", out)

    def test_a_target_with_nothing_to_replay_fails(self):
        for f in (self.root / "tests" / "fuzz").rglob("*"):
            if f.is_file() and f.parent.name == "x":
                f.unlink()
        self.target(self.EVERY)
        code, out = self.fuzz("replay")
        self.assertEqual(code, 1, out)
        self.assertIn("x has no committed input", out)

    def test_nothing_built_and_nothing_to_fuzz_with(self):
        code, out = self.fuzz("replay")
        self.assertEqual(code, 1, out)
        self.assertIn("nothing is built in build/fuzz", out)
        self.target(self.EVERY, engine="replay")
        code, out = self.fuzz("run", "x", "5")
        self.assertEqual(code, 1, out)
        self.assertIn("built to replay, not to fuzz", out)

    def test_a_targets_options_are_flags(self):
        self.target('echo "$@"\n', engine="libfuzzer")
        (self.root / "tests" / "fuzz" / "x.options").write_text("# ok\n-max_len=64\n")
        code, out = self.fuzz("run", "x", "1")
        self.assertEqual(code, 0, out)
        self.assertIn("-max_len=64", out)
        (self.root / "tests" / "fuzz" / "x.options").write_text("max_len=64\n")
        code, out = self.fuzz("run", "x", "1")
        self.assertEqual(code, 1, out)
        self.assertIn("is not a libFuzzer flag", out)
        code, out = self.fuzz("run", "x", "soon")
        self.assertEqual(code, 2, out)


class FuzzBuildCompilerTest(unittest.TestCase):
    """tools/fuzz.sh build on a machine of stand-ins: which compiler and which
    engine it picks. The build is configured with RMP_REQUIRE_SANITIZERS=1, so
    a compiler that cannot run ASan is a refused configure -- and a clang
    without compiler-rt, which compiles everything and links no sanitizer,
    was picked over a gcc whose sanitizers run (seen on a clang made to look
    for its runtimes in an empty folder: "FAIL: the fuzz build did not
    configure")."""

    # A compiler driver that writes a program for -o: libFuzzer's probe prints
    # what fuzz.sh waits for, anything else exits 0. What it can link is the
    # mode file's: nothing sanitized, the sanitizers, or libFuzzer too.
    CLANG = """#!/bin/sh
out=""; prev=""; fuzzer=0; sanitized=0
for a in "$@"; do
  case "$a" in
    --version) echo "clang version 0 (stand-in)"; exit 0 ;;
    -fsanitize=*fuzzer*) fuzzer=1 ;;
    -fsanitize=*) sanitized=1 ;;
  esac
  if [ "$prev" = "-o" ]; then out="$a"; fi
  prev="$a"
done
mode=$(cat "$STANDIN_MODE")
if [ "$fuzzer" = 1 ] && [ "$mode" != libfuzzer ]; then echo "no libclang_rt.fuzzer" >&2; exit 1; fi
if [ "$sanitized" = 1 ] && [ "$mode" = bare ]; then echo "no libclang_rt.asan" >&2; exit 1; fi
printf '#!/bin/sh\\necho "Done 16 runs"\\n' > "$out"
chmod +x "$out"
"""
    GCC = '#!/bin/sh\necho "g++ (stand-in) 0"\n'
    # Writes what the configure was asked for, and says what deep.cmake says.
    CMAKE = """#!/bin/sh
if [ "$1" = "--build" ]; then
  printf '#!/bin/sh\\n' > build/fuzz/fuzz_x
  chmod +x build/fuzz/fuzz_x
  exit 0
fi
engine=""
for a in "$@"; do
  case "$a" in
    -DCMAKE_CXX_COMPILER=*) echo "${a#*=}" > build/compiler ;;
    -DRMP_FUZZ_ENGINE=*) engine="${a#*=}" ;;
  esac
done
echo "=== SANITIZERS: address, undefined ==="
echo "=== FUZZ: $engine, fuzz_x ==="
"""
    TOOLS = ("bash", "sh", "dirname", "basename", "mkdir", "rm", "cat", "sed", "head", "grep",
             "tr", "wc", "chmod")

    def setUp(self):
        if shutil.which("bash") is None:
            self.skipTest("no bash")
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "tools").mkdir()
        shutil.copy(TOOLS / "fuzz.sh", self.root / "tools" / "fuzz.sh")
        (self.root / "tests" / "fuzz").mkdir(parents=True)
        (self.root / "tests" / "fuzz" / "x.cpp").write_text("// a harness\n")
        # Only the stand-ins and the tools the script runs: not this
        # machine's own clang or gcc.
        self.bin = self.root / "bin"
        self.bin.mkdir()
        for tool in self.TOOLS:
            found = shutil.which(tool)
            if found is None:
                self.skipTest(f"no {tool}")
            (self.bin / tool).symlink_to(found)
        self.stub("cmake", self.CMAKE)
        self.stub("g++", self.GCC)
        self.stub("gcc", self.GCC)
        self.mode = self.root / "mode"

    def tearDown(self):
        self._tmp.cleanup()

    def stub(self, name: str, text: str):
        (self.bin / name).write_text(text)
        (self.bin / name).chmod(0o755)

    def build(self, clang: str | None, **env):
        if clang is not None:
            self.mode.write_text(clang)
            self.stub("clang", self.CLANG)
            self.stub("clang++", self.CLANG)
        got = subprocess.run(["bash", "tools/fuzz.sh", "build"], cwd=self.root,
                             capture_output=True, text=True,
                             env={"PATH": str(self.bin), "STANDIN_MODE": str(self.mode),
                                  "HOME": str(self.root), **env})
        out = got.stdout + got.stderr
        compiler = self.root / "build" / "compiler"
        engine = self.root / "build" / "fuzz" / "engine"
        return (got.returncode, out, compiler.read_text().strip() if compiler.exists() else None,
                engine.read_text().strip() if engine.exists() else None)

    def test_libfuzzer_is_clangs(self):
        code, out, compiler, engine = self.build("libfuzzer")
        self.assertEqual(code, 0, out)
        self.assertEqual((compiler, engine), ("clang++", "libfuzzer"))

    def test_a_clang_that_sanitizes_replays(self):
        code, out, compiler, engine = self.build("sanitizers")
        self.assertEqual(code, 0, out)
        self.assertEqual((compiler, engine), ("clang++", "replay"))
        self.assertIn("no libFuzzer here", out)

    def test_a_clang_without_its_runtimes_leaves_the_build_to_gcc(self):
        code, out, compiler, engine = self.build("bare")
        self.assertEqual(code, 0, out)
        self.assertEqual((compiler, engine), ("g++", "replay"))
        self.assertIn("== fuzz: build (replay, g++ (stand-in) 0) ==", out)

    def test_no_clang_at_all_is_gcc(self):
        code, out, compiler, engine = self.build(None)
        self.assertEqual(code, 0, out)
        self.assertEqual((compiler, engine), ("g++", "replay"))

    def test_libfuzzer_required_and_missing_fails_before_configuring(self):
        code, out, compiler, engine = self.build("sanitizers", RMP_REQUIRE_LIBFUZZER="1")
        self.assertEqual(code, 1, out)
        self.assertIn("RMP_REQUIRE_LIBFUZZER=1", out)
        self.assertEqual((compiler, engine), (None, None))


# ---------------------------------------------------------------------------
# The mutation floors
# ---------------------------------------------------------------------------

# Files of src/rmp/ that only compile a vendored library: there is nothing of
# ours in them to mutate.
NOT_MUTATED = {
    "src/rmp/aseprite_impl.cpp": "compiles cute_aseprite, once",
    "src/rmp/tiled_impl.cpp": "compiles cute_tiled, once",
    "src/rmp/rres_impl.cpp": "compiles rres with its encryption, once",
    "src/rmp/cjson_impl.c": "compiles cJSON, with its nesting limit",
    "src/rmp/ui/clay_impl.cpp": "compiles Clay, once",
}
# What a C++ floor's `tests` line may run: the test programs tests/deep.cmake
# and CMakeLists.txt build.
TEST_PROGRAMS = {"unit_test", "ui_layout_test", "property_test"}


def framework_sources(root: Path) -> list[str]:
    return sorted(p.relative_to(root).as_posix() for p in (root / "src" / "rmp").rglob("*")
                  if p.suffix in (".cpp", ".c") and p.is_file())


# Where a checkout is, as doctest sees a test's file: the whole path the
# compiler was given (CMake hands it an absolute one), whose start a floor file
# cannot know -- so a -sf choice that holds in every checkout begins with `*`.
CHECKOUT = "/any/checkout/"
# A test case's name: TEST_CASE("...") or TEST_CASE_FIXTURE(Fixture, "..."),
# adjacent literals joined as the compiler joins them.
CASE = re.compile(r'\bTEST_CASE(?:_FIXTURE\s*\(\s*[\w:]+\s*,|\s*\()\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)')


@functools.cache
def doctest_names(root: Path) -> tuple[list[str], list[str], list[str]]:
    """What a doctest filter can select under root/tests/, lower case: the
    sources' whole paths, the suites and the cases."""
    sources = {path: path.read_text(encoding="utf-8", errors="replace")
               for path in (root / "tests").rglob("*.cpp")}
    files = sorted(CHECKOUT + path.relative_to(root).as_posix().lower() for path in sources)
    suites = {suite.lower() for text in sources.values()
              for suite in re.findall(r'TEST_SUITE(?:_BEGIN)?\(\s*"([^"]*)"', text)}
    cases = {"".join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', m.group(1))).lower()
             for text in sources.values() for m in CASE.finditer(text)}
    return files, sorted(suites), sorted(cases)


def doctest_filter_problems(name: str, program: list[str], root: Path) -> list[str]:
    """A doctest filter of a floor's test program that selects nothing: the
    program runs no test, passes, and every mutant survives -- read as a
    module nobody tested rather than as a filter that names a file renamed
    away. doctest matches -sf against the source's whole path, -ts against
    the suite names and -tc against the case names, with wildcards and
    without case, a comma between choices."""
    files, suites, cases = doctest_names(root)
    out = []
    for arg in program[1:]:
        flag, _, value = arg.partition("=")
        if flag in ("-sf", "--source-file"):
            what, names = "source file", files
        elif flag in ("-ts", "--test-suite"):
            what, names = "TEST_SUITE", suites
        elif flag in ("-tc", "--test-case"):
            what, names = "TEST_CASE", cases
        else:
            continue
        for choice in value.split(","):
            # doctest's wildcards are * and ? alone: a [ is itself.
            pattern = choice.lower().replace("[", "[[]")
            if not any(fnmatch.fnmatchcase(n, pattern) for n in names):
                hint = (" (doctest reads the whole path, which starts where the checkout is: "
                        "begin it with *)" if what == "source file" and not choice.startswith("*")
                        else "")
                out.append(f"{name}: tests {program[0]} {flag}={choice} matches no {what} under "
                           f"tests/{hint}: the program would run nothing and every mutant survive")
    return out


def floor_problems(floors: dict[str, mr.Floor], sources: list[str], root: Path) -> list[str]:
    out = []
    owner: dict[str, str] = {}
    for name, floor in floors.items():
        for source in floor.sources:
            if not (root / source).is_file():
                out.append(f"{name}: source {source} does not exist")
            if source in owner:
                out.append(f"{source} is in two floor files: {owner[source]} and {name}")
            owner[source] = name
            if source in NOT_MUTATED:
                out.append(f"{name}: {source} only compiles a vendored library")
        for program in floor.tests:
            if floor.kind == "cxx" and program[0] not in TEST_PROGRAMS:
                out.append(f"{name}: tests {program[0]} is not a test program "
                           f"({', '.join(sorted(TEST_PROGRAMS))})")
            if floor.kind == "cxx":
                out.extend(doctest_filter_problems(name, program, root))
            if floor.kind == "py":
                # A file, or a pytest node id in one: tests/x_test.py::SomeTest.
                for path in program:
                    if not (root / path.split("::")[0]).is_file():
                        out.append(f"{name}: tests {path} does not exist")
    for source in sources:
        if source not in owner and source not in NOT_MUTATED:
            out.append(f"{source} is in no floor file under tests/mutation/floors/: "
                       "add it to its module's sources, or write the module's file")
    for source in NOT_MUTATED:
        if source not in sources:
            out.append(f"NOT_MUTATED names {source}, which is gone")
    return out


class FloorsTest(unittest.TestCase):

    def test_every_framework_file_is_in_one_floor_file(self):
        floors = mr.load_floors()
        self.assertGreater(len(floors), 10)
        self.assertEqual(floor_problems(floors, framework_sources(REPO), REPO), [])

    def test_the_check_sees_each_mistake(self):
        floors = mr.load_floors()
        sources = framework_sources(REPO)
        some = next(n for n, f in floors.items() if f.kind == "cxx" and len(f.sources) == 1)
        cases = [
            ("a file nobody mutates", lambda f: f[some].sources.clear(),
             f"{floors[some].sources[0]} is in no floor file"),
            ("a file in two", lambda f: f["random"].sources.append(floors[some].sources[0]),
             "is in two floor files"),
            ("a source that is not there", lambda f: f[some].sources.append("src/rmp/gone.cpp"),
             "source src/rmp/gone.cpp does not exist"),
            ("a program nobody builds", lambda f: f[some].tests.append(["game_test"]),
             "tests game_test is not a test program"),
            ("a Python test that is not there",
             lambda f: f["configure"].tests.append(["tests/gone_test.py::SomeTest"]),
             "tests tests/gone_test.py::SomeTest does not exist"),
            ("a vendored unit", lambda f: f[some].sources.append("src/rmp/tiled_impl.cpp"),
             "only compiles a vendored library"),
            ("a source filter that matches no file",
             lambda f: f["random"].tests.append(["unit_test", "-sf=*randum_test.cpp"]),
             "random: tests unit_test -sf=*randum_test.cpp matches no source file"),
            ("one of two source filters that matches nothing",
             lambda f: f["random"].tests.append(["unit_test",
                                                 "-sf=*save_test.cpp,*gone_test.cpp"]),
             "-sf=*gone_test.cpp matches no source file"),
            ("a suite filter that matches no suite",
             lambda f: f["random"].tests.append(["unit_test", "-ts=randon"]),
             "random: tests unit_test -ts=randon matches no TEST_SUITE"),
            # doctest matches -sf against the whole path the compiler was
            # given, and that starts wherever the checkout is: this one ran
            # 0 of 571 cases, "Status: SUCCESS!".
            ("a source filter without the path's start",
             lambda f: f["random"].tests.append(["unit_test", "-sf=tests/save_test.cpp"]),
             "-sf=tests/save_test.cpp matches no source file"),
            ("a test-case filter that matches no case",
             lambda f: f["random"].tests.append(["unit_test", "--test-case=*no such case*"]),
             "--test-case=*no such case* matches no TEST_CASE"),
        ]
        for what, break_it, says in cases:
            with self.subTest(what):
                broken = mr.load_floors()
                break_it(broken)
                found = floor_problems(broken, sources, REPO)
                self.assertTrue(any(says in line for line in found), found)
        found = floor_problems(floors, [*sources, "src/rmp/new.cpp"], REPO)
        self.assertEqual(found, ["src/rmp/new.cpp is in no floor file under tests/mutation/floors/: "
                                 "add it to its module's sources, or write the module's file"])

    def test_filters_that_select_something_pass(self):
        """A test case is found by its whole name -- adjacent literals joined,
        a fixture's too -- and a source filter by its last components."""
        for program in (["unit_test", "-sf=*/tests/save_test.cpp"],
                        ["unit_test", "-sf=*SAVE_test.cpp,*audio_test.cpp"],
                        # Across the two literals the name is written in.
                        ["property_test", "-tc=*NaN and the infinities by name"],
                        ["unit_test", "-tc=the buses start at the values in the .toml"],
                        ["unit_test", "-tse=*nothing*", "-sce=*nothing*"]):
            with self.subTest(program=program):
                self.assertEqual(doctest_filter_problems("x", program, REPO), [])

    def test_the_measured_floor_is_the_pilots(self):
        """random was measured in the image on 2026-10-11: 51 of 62 (82.26%) in the pilot,
        81 of 81 after P5; a floor only rises."""
        self.assertGreaterEqual(mr.load_floors()["random"].floor, 80)


GOOD = """# a module
kind     cxx
sources  src/rmp/random.cpp
tests    unit_test -ts=random
floor    80
equivalent cxx_le_to_lt:src/rmp/random.cpp:9:9 max == min returns min either way
"""


def mull(*mutants):
    """An Elements report holding (mutator, line, column, status) mutants."""
    return {"files": {"/work/src/rmp/random.cpp": {"mutants": [
        {"mutatorName": m, "location": {"start": {"line": line, "column": col}}, "status": s}
        for m, line, col, s in mutants]}}}


class FloorCheckerTest(unittest.TestCase):

    def floor(self, text=GOOD):
        return mr.parse_floor(text, "random.txt")

    def verdict(self, killed, survived, floor=None, extra=()):
        mutants = [("cxx_add_to_sub", i, 1, "Killed") for i in range(killed)]
        mutants += [("cxx_sub_to_add", i, 1, "Survived") for i in range(survived)]
        mutants += [("cxx_le_to_lt", 9, 9, "Survived"), *extra]
        found = mr.mull_mutants(mull(*mutants), ["src/rmp/random.cpp"])
        return mr.check(floor or self.floor(), found)

    def test_at_and_above_the_floor(self):
        ok, fails, score = self.verdict(41, 9)   # 82%: the floor 80 is right
        self.assertEqual(fails, [])
        self.assertAlmostEqual(score, 82.0)
        self.assertIn("41/50 detected, 1 equivalent", ok[0])

    def test_below_the_floor(self):
        ok, fails, score = self.verdict(39, 11)  # 78%
        self.assertEqual(len(fails), 1, fails)
        self.assertIn("below its floor of 80", fails[0])

    def test_a_floor_left_behind_is_raised(self):
        ok, fails, score = self.verdict(43, 7)   # 86%: 85 is the floor now
        self.assertEqual(len(fails), 1, fails)
        self.assertIn("raise it to 85", fails[0])
        floor = self.floor()
        self.assertIn("floor    85", mr.raised(GOOD, floor, score))
        # Never lowered, and a `-` is a hand edit.
        self.assertEqual(mr.raised(GOOD, floor, 70.0), GOOD)
        unmeasured = GOOD.replace("floor    80", "floor    -")
        self.assertEqual(mr.raised(unmeasured, self.floor(unmeasured), 99.0), unmeasured)

    def test_no_floor_yet_reports_and_passes(self):
        text = GOOD.replace("floor    80", "floor    -")
        ok, fails, score = self.verdict(1, 49, floor=self.floor(text))
        self.assertEqual(fails, [])
        self.assertIn("no floor yet: 0 would be it", ok[0])

    def test_the_equivalent_list_only_shrinks(self):
        killed_now = ("cxx_le_to_lt", 9, 9, "Killed")
        found = mr.mull_mutants(mull(("cxx_add_to_sub", 1, 1, "Killed"), killed_now),
                                ["src/rmp/random.cpp"])
        ok, fails, score = mr.check(self.floor(), found)
        self.assertTrue(any("is killed now: it was not equivalent" in f for f in fails), fails)
        found = mr.mull_mutants(mull(("cxx_add_to_sub", 1, 1, "Killed")), ["src/rmp/random.cpp"])
        ok, fails, score = mr.check(self.floor(), found)
        self.assertTrue(any("is not in the report: the code moved" in f for f in fails), fails)

    def test_an_unfinished_or_empty_run_fails(self):
        ok, fails, score = self.verdict(45, 5, extra=[("cxx_x", 3, 3, "Pending")])
        self.assertTrue(any("never got a verdict" in f for f in fails), fails)
        ok, fails, score = mr.check(self.floor(), {})
        self.assertIsNone(score)
        self.assertIn("holds no mutant", fails[0])

    def test_no_coverage_counts_against_and_a_compile_error_not_at_all(self):
        found = mr.mull_mutants(mull(("a", 1, 1, "Killed"), ("b", 2, 2, "NoCoverage"),
                                     ("c", 3, 3, "CompileError"), ("d", 4, 4, "Timeout"),
                                     ("cxx_le_to_lt", 9, 9, "Survived")), ["src/rmp/random.cpp"])
        ok, fails, score = mr.check(self.floor(GOOD.replace("80", "-")), found)
        self.assertAlmostEqual(score, 100 * 2 / 3)

    def test_mull_ids_are_the_repositorys_and_other_files_are_ignored(self):
        report = mull(("cxx_add_to_sub", 74, 62, "Killed"))
        report["files"]["/elsewhere/src/rmp/value.cpp"] = {"mutants": [
            {"mutatorName": "x", "location": {"start": {"line": 1, "column": 1}},
             "status": "Survived"}]}
        self.assertEqual(mr.mull_mutants(report, ["src/rmp/random.cpp"]),
                         {"cxx_add_to_sub:src/rmp/random.cpp:74:62": mr.DETECTED})

    def test_killed_by_any_program_is_killed(self):
        with tempfile.TemporaryDirectory() as tmp:
            floors = Path(tmp) / "floors"
            floors.mkdir()
            (floors / "random.txt").write_text(GOOD.replace("80", "50"))
            one, two = Path(tmp) / "1.json", Path(tmp) / "2.json"
            one.write_text(json.dumps(mull(("a", 1, 1, "Killed"), ("b", 2, 2, "Survived"),
                                           ("cxx_le_to_lt", 9, 9, "Survived"))))
            two.write_text(json.dumps(mull(("a", 1, 1, "Survived"), ("b", 2, 2, "Killed"),
                                           ("cxx_le_to_lt", 9, 9, "Survived"))))
            out = subprocess.run([sys.executable, str(TOOLS / "mutation_report.py"), "random",
                                  "--floors", str(floors), "--mull", str(one)],
                                 capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout)
            self.assertIn("1/2 detected", out.stdout)
            out = subprocess.run([sys.executable, str(TOOLS / "mutation_report.py"), "random",
                                  "--floors", str(floors), "--mull", str(one), "--mull", str(two)],
                                 capture_output=True, text=True)
            self.assertEqual(out.returncode, 1, out.stdout)  # 100%: raise it to 100
            self.assertIn("2/2 detected", out.stdout)
            self.assertIn("raise it to 100", out.stdout)
            out = subprocess.run([sys.executable, str(TOOLS / "mutation_report.py"), "random",
                                  "--floors", str(floors), "--mull", str(one), "--mull", str(two),
                                  "--update"], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stdout)
            self.assertIn("floor    100", (floors / "random.txt").read_text())
            bad = Path(tmp) / "bad.json"
            bad.write_text("{not json")
            out = subprocess.run([sys.executable, str(TOOLS / "mutation_report.py"), "random",
                                  "--floors", str(floors), "--mull", str(bad)],
                                 capture_output=True, text=True)
            self.assertEqual(out.returncode, 1)
            self.assertIn("the report cannot be read", out.stdout)

    def test_mutmuts_exit_codes(self):
        meta = {"exit_code_by_key": {"m.x_f__mutmut_1": 1, "m.x_f__mutmut_2": 0,
                                     "m.x_f__mutmut_3": 33, "m.x_f__mutmut_4": 36,
                                     "m.x_f__mutmut_5": None, "m.x_f__mutmut_6": 34}}
        self.assertEqual(mr.mutmut_mutants(meta), {
            "m.x_f__mutmut_1": mr.DETECTED, "m.x_f__mutmut_2": mr.UNDETECTED,
            "m.x_f__mutmut_3": mr.UNDETECTED, "m.x_f__mutmut_4": mr.DETECTED,
            "m.x_f__mutmut_5": mr.UNFINISHED, "m.x_f__mutmut_6": mr.NOT_COUNTED})

    def test_a_select_line_counts_what_it_selected(self):
        """mutmut writes every mutant of the file into its .meta, and the ones
        `mutmut run <names>` was not asked to run stay null. This is the .meta
        mutmut 3.8 wrote in the image for a two-function file run with
        "tools.m.x_a*": read whole, x_b's two read as a run that did not
        finish, and a floor file with a select line could never pass."""
        meta = {"exit_code_by_key": {"tools.m.x_a__mutmut_1": 1, "tools.m.x_a__mutmut_2": 1,
                                     "tools.m.x_b__mutmut_1": None,
                                     "tools.m.x_b__mutmut_2": None}}
        self.assertEqual(mr.mutmut_mutants(meta, ["tools.m.x_a*"]),
                         {"tools.m.x_a__mutmut_1": mr.DETECTED,
                          "tools.m.x_a__mutmut_2": mr.DETECTED})
        self.assertEqual(len(mr.mutmut_mutants(meta)), 4)
        with tempfile.TemporaryDirectory() as tmp:
            floors = Path(tmp) / "floors"
            floors.mkdir()
            (floors / "m.txt").write_text("kind py\nsources tools/m.py\ntests tests/m_test.py\n"
                                          "select tools.m.x_a*\nfloor 100\n")
            (floors / "all.txt").write_text("kind py\nsources tools/m.py\ntests tests/m_test.py\n"
                                            "floor 100\n")
            report = Path(tmp) / "m.py.meta"
            report.write_text(json.dumps(meta))
            for module, code, says in (("m", 0, "2/2 detected"),
                                       ("all", 1, "2 mutant(s) never got a verdict")):
                with self.subTest(module=module):
                    out = subprocess.run([sys.executable, str(TOOLS / "mutation_report.py"),
                                          module, "--floors", str(floors), "--mutmut",
                                          str(report)], capture_output=True, text=True)
                    self.assertEqual(out.returncode, code, out.stdout)
                    self.assertIn(says, out.stdout)

    def test_a_floor_file_says_what_it_must(self):
        for text, says in [
            (GOOD.replace("kind     cxx", "kind     rust"), "kind is cxx or py"),
            (GOOD.replace("floor    80", "floor    101"), "floor is a whole number"),
            (GOOD.replace("floor    80", "floor    eighty"), "floor is a whole number"),
            (GOOD.replace("floor    80\n", ""), "no floor line"),
            (GOOD.replace("tests    unit_test -ts=random\n", ""), "no tests line"),
            (GOOD + "colour   blue\n", "unknown key 'colour'"),
            (GOOD + "floor    70\n", "a second floor line"),
            (GOOD + "select   x*\n", "select is mutmut's"),
            (GOOD.replace("max == min returns min either way", "x"), "says why"),
            (GOOD + GOOD.splitlines(keepends=True)[-1], "is listed twice"),
            (GOOD.replace("sources  src/rmp/random.cpp", "sources"), "sources says nothing"),
        ]:
            with self.subTest(says=says):
                with self.assertRaises(mr.FloorError) as caught:
                    self.floor(text)
                self.assertIn(says, str(caught.exception))
        self.assertEqual(self.floor().floor, 80)


class MutationScriptTest(unittest.TestCase):
    """tools/mutation.sh on a tree of its own: what it lists, what it refuses,
    and that `check` reads the report the last run left -- or fails without
    one, instead of passing on nothing."""

    def setUp(self):
        if shutil.which("bash") is None:
            self.skipTest("no bash")
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "tools").mkdir()
        for name in ("mutation.sh", "mutation_report.py"):
            shutil.copy(TOOLS / name, self.root / "tools" / name)
        floors = self.root / "tests" / "mutation" / "floors"
        floors.mkdir(parents=True)
        (floors / "random.txt").write_text(GOOD)
        (floors / "tool.txt").write_text("kind py\nsources tools/x.py\ntests tests/x_test.py\n"
                                         "floor -\n")

    def tearDown(self):
        self._tmp.cleanup()

    def mutation(self, *argv):
        got = subprocess.run(["bash", "tools/mutation.sh", *argv], cwd=self.root,
                             capture_output=True, text=True)
        return got.returncode, got.stdout + got.stderr

    def test_it_lists_the_modules_with_their_kind(self):
        self.assertEqual(self.mutation("modules"), (0, "cxx random\npy tool\n"))

    def test_an_unknown_module_and_a_missing_report_fail(self):
        code, out = self.mutation("run", "nope")
        self.assertEqual(code, 2, out)
        self.assertIn("no module called 'nope'", out)
        code, out = self.mutation("check", "random")
        self.assertEqual(code, 1, out)
        self.assertIn("random has no report: bash tools/mutation.sh run random", out)

    def test_check_reads_the_last_runs_report(self):
        report = self.root / "build" / "mutation" / "cxx-random" / "report"
        report.mkdir(parents=True)
        mutants = [("a", i, 1, "Killed") for i in range(41)]
        mutants += [("b", i, 1, "Survived") for i in range(9)]
        (report / "random-1.json").write_text(
            json.dumps(mull(*mutants, ("cxx_le_to_lt", 9, 9, "Survived"))))
        code, out = self.mutation("check", "random")
        self.assertEqual(code, 0, out)
        self.assertIn("ok    random                82.00%", out)
        (report / "random-1.json").write_text(json.dumps(mull(*mutants[:30], *mutants[41:],
                                                             ("cxx_le_to_lt", 9, 9, "Survived"))))
        code, out = self.mutation("check", "random")
        self.assertEqual(code, 1, out)
        self.assertIn("below its floor of 80", out)

    def test_without_the_tools_it_says_where_they_are(self):
        if shutil.which("mull-runner-18") or shutil.which("mull-runner"):
            self.skipTest("Mull is here: `run` would build")
        code, out = self.mutation("run", "random")
        self.assertEqual(code, 1, out)
        self.assertIn("no mull-runner here", out)

    def test_the_warm_up_run_has_time_for_property_test(self):
        """mull-runner's warm-up run -- the program unmutated -- times out at
        3 s unless told otherwise, and property_test takes 5 at Mull's -O0:
        the weekly value and save shards could not have measured anything."""
        text = (TOOLS / "mutation.sh").read_text()
        call = text[text.index('"$MULL_RUNNER"'):]
        call = call[:call.index("< /dev/null")]
        timeout = re.search(r"(?<![\w-])--timeout (\d+)", call)
        self.assertIsNotNone(timeout, "mull-runner is called without --timeout")
        self.assertGreaterEqual(int(timeout.group(1)), 60000)


# ---------------------------------------------------------------------------
# The resilience list: every public function held by a property, or waived
# ---------------------------------------------------------------------------

COVERS = re.compile(r"^\s*//\s*covers:\s*(\S.*?)\s*$", re.M)


def covered(folder: Path = PROPERTY) -> dict[str, str]:
    """`// covers: <function>` in a tests/property/*_prop.cpp: function -> file."""
    out = {}
    for path in sorted(folder.glob("*_prop.cpp")):
        for key in COVERS.findall(path.read_text(encoding="utf-8")):
            out.setdefault(key, path.name)
    return out


def read_waivers(path: Path = WAIVERS) -> list[str]:
    return [line.strip() for line in path.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.startswith("#")]


def resilience_problems(api: set[str], covers: dict[str, str], waived: list[str]) -> list[str]:
    out = []
    for key in sorted(set(waived)):
        if waived.count(key) > 1:
            out.append(f"waivers.txt lists {key} {waived.count(key)} times")
    for key in sorted(api):
        if key not in covers and key not in waived:
            out.append(f"{key} has no property and no waiver: write the property "
                       f"(`// covers: {key}` in a tests/property/*_prop.cpp)")
    for key in sorted(set(waived)):
        if key in covers:
            out.append(f"{key} is held by {covers[key]} now: take its line out of waivers.txt")
        elif key not in api:
            out.append(f"waivers.txt lists {key}, which is not a public function: "
                       "take its line out")
    for key, where in sorted(covers.items()):
        if key not in api:
            out.append(f"{where} covers {key}, which is not a public function")
    return out


def header_flags() -> list[str]:
    return ["-std=c++20", f"-I{REPO / 'include'}", "-isystem", str(REPO / "thirdparty/raylib/src"),
            "-isystem", str(REPO / "thirdparty")]


class ResilienceListTest(unittest.TestCase):

    def test_every_public_function_has_a_property_or_a_waiver(self):
        clang = os.environ.get("RMP_CLANGXX") or shutil.which("clang++")
        if clang is None:
            if os.environ.get("RMP_REQUIRE_CLANG") == "1":
                self.fail("no clang++ to read the headers with, and RMP_REQUIRE_CLANG=1")
            self.skipTest("no clang++ to read the public headers with")
        if not (REPO / "include" / "rmp" / "generated" / "config.h").is_file():
            self.fail("include/rmp/generated/config.h is not there: python3 tools/configure.py")
        import coverage_report
        api = set(coverage_report.public_api(clang, REPO / "include", header_flags()))
        self.assertGreater(len(api), 100, "the headers read as almost nothing")
        self.assertEqual(resilience_problems(api, covered(), read_waivers()), [])

    def test_the_check_sees_each_mistake(self):
        api = {"rmp::a()", "rmp::b(int)", "rmp::c()"}
        self.assertEqual(resilience_problems(api, {"rmp::a()": "a_prop.cpp"},
                                             ["rmp::b(int)", "rmp::c()"]), [])
        for covers, waived, says in [
            ({}, ["rmp::b(int)", "rmp::c()"], "rmp::a() has no property and no waiver"),
            ({"rmp::a()": "a_prop.cpp"}, ["rmp::a()", "rmp::b(int)", "rmp::c()"],
             "rmp::a() is held by a_prop.cpp now"),
            ({"rmp::a()": "a_prop.cpp"}, ["rmp::b(int)", "rmp::c()", "rmp::gone()"],
             "waivers.txt lists rmp::gone(), which is not a public function"),
            ({"rmp::a()": "a_prop.cpp", "rmp::z()": "z_prop.cpp"}, ["rmp::b(int)", "rmp::c()"],
             "z_prop.cpp covers rmp::z(), which is not a public function"),
            ({"rmp::a()": "a_prop.cpp"}, ["rmp::b(int)", "rmp::c()", "rmp::c()"],
             "lists rmp::c() 2 times"),
        ]:
            with self.subTest(says=says):
                found = resilience_problems(api, covers, waived)
                self.assertEqual(len(found), 1, found)
                self.assertIn(says, found[0])

    def test_covers_lines_are_read_from_the_property_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / "a_prop.cpp").write_text("// covers: rmp::a()\n  //covers:  rmp::b(int)  \n"
                                               "int x; // covers: not at the start\n")
            (folder / "b_test.cpp").write_text("// covers: rmp::c()\n")
            self.assertEqual(covered(folder), {"rmp::a()": "a_prop.cpp",
                                               "rmp::b(int)": "a_prop.cpp"})


def spelled_as(key: str) -> str | None:
    """The name a property file's code must say for `// covers: key` to be
    believed: the function's own, unqualified -- None for an operator or a
    destructor, which code uses without spelling their names."""
    head = key.split("(")[0].split(" [")[0]
    if "operator" in head:
        return None
    name = head.split("::")[-1]
    return None if name.startswith("~") else name


def unbacked_covers(folder: Path = PROPERTY) -> list[str]:
    """`// covers:` lines whose file never names the function in its code --
    a comment that would take a function off waivers.txt with no property
    behind it (configure_test's rejection gate was satisfied by a docstring
    once, the same way)."""
    out = []
    for path in sorted(folder.glob("*_prop.cpp")):
        text = path.read_text(encoding="utf-8")
        code = cpp_text.blank(text)
        for key in COVERS.findall(text):
            name = spelled_as(key)
            if name is not None and not re.search(rf"(?<![\w~]){re.escape(name)}\b", code):
                out.append(f"{path.name} covers {key}, and its code never calls {name}: "
                           "a `// covers:` line is a property's, not a promise")
    return out


# rmp::test::check(<description>, ...), and a plain check( under a using:
# never a member's x.check( or another namespace's y::check(.
CHECK_CALL = re.compile(r"(?<![\w.>:])(?:rmp::test::)?check\s*\(")
LITERALS = re.compile(r'\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)(?=,)')


def check_descriptions(folder: Path = PROPERTY) -> tuple[dict[str, list[str]], list[str]]:
    """Every rmp::test::check() description of the *_prop.cpp files, as
    {description: [file:line, ...]}, and the calls whose description is not
    a string literal."""
    found: dict[str, list[str]] = {}
    not_literal = []
    for path in sorted(folder.glob("*_prop.cpp")):
        code = cpp_text.blank(path.read_text(encoding="utf-8"), comments=True, strings=False)
        lines = cpp_text.Lines(code)
        for call in CHECK_CALL.finditer(code):
            where = f"{path.name}:{lines.of(call.start())}"
            literal = LITERALS.match(code, call.end())
            if literal is None:
                not_literal.append(where)
                continue
            text = "".join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', literal.group(1)))
            found.setdefault(text, []).append(where)
    return found, not_literal


def description_problems(folder: Path = PROPERTY) -> list[str]:
    found, not_literal = check_descriptions(folder)
    out = [f"{where}: a check() whose description is not a string literal -- it is the key "
           "RC_PARAMS=\"reproduce=...\" finds the property by, so it is written out"
           for where in not_literal]
    # RapidCheck's own rc::check() prints to stderr and returns a bool: a
    # property that fails through it is a line in the log and a green run.
    for path in sorted(folder.glob("*_prop.cpp")):
        code = cpp_text.blank(path.read_text(encoding="utf-8"))
        lines = cpp_text.Lines(code)
        for call in re.finditer(r"\brc::check\s*\(", code):
            out.append(f"{path.name}:{lines.of(call.start())}: rc::check() fails nothing -- it "
                       "prints and returns a bool; rmp::test::check() is a doctest check")
    for text, wheres in sorted(found.items()):
        if len(wheres) > 1:
            out.append(f"{' and '.join(wheres)} share the description {text!r}: a reproduce "
                       "string would replay its case against both")
    return out


class PropertyFilesTest(unittest.TestCase):
    """What tests/property/check.h and the resilience list take on trust,
    checked: a check()'s description is the key a reproduce string finds its
    property by -- written out, and never two alike -- and a `// covers:`
    line belongs to a file whose code calls the function."""

    def test_the_property_files_keep_both(self):
        found, not_literal = check_descriptions()
        self.assertTrue(found, "no check() found at all: the check would pass on nothing")
        self.assertEqual(not_literal, [])
        self.assertEqual(description_problems(), [])
        self.assertEqual(unbacked_covers(), [])

    def test_the_check_sees_each_mistake(self):
        good = ('// covers: rmp::random::range(int, int)\n'
                '// covers: rmp::Value::operator==(const Value &) const\n'
                '// covers: rmp::Value::~Value()\n'
                'TEST_CASE("x") {\n'
                '    // rmp::test::check("in a comment", ...) is not a call\n'
                '    rmp::test::check("a range holds"\n'
                '                     " its ends",\n'
                '                     [](int a) { RC_ASSERT(rmp::random::range(a, a) == a); });\n'
                '    other.check("not ours", 1);\n'
                '}\n')
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            (folder / "a_prop.cpp").write_text(good)
            self.assertEqual(description_problems(folder), [])
            self.assertEqual(unbacked_covers(folder), [])
            self.assertEqual(check_descriptions(folder)[0],
                             {"a range holds its ends": ["a_prop.cpp:6"]})
            cases = [
                ("b_prop.cpp", 'void f() { check("a range holds" " its ends", g); }\n',
                 "a_prop.cpp:6 and b_prop.cpp:1 share the description 'a range holds its ends'"),
                ("b_prop.cpp", 'void f() { rmp::test::check(name, g); }\n',
                 "b_prop.cpp:1: a check() whose description is not a string literal"),
                ("b_prop.cpp", 'void f() {\n  // rc::check("said", g) in a comment\n'
                               '  rc::check("a range holds its ends", g);\n}\n',
                 "b_prop.cpp:3: rc::check() fails nothing"),
                ("b_prop.cpp", '// covers: rmp::random::chance(float)\n'
                               'void f() { /* chance(0.5F) */ const char *s = "chance(1)"; }\n',
                 "b_prop.cpp covers rmp::random::chance(float), and its code never calls chance"),
                ("b_prop.cpp", '// covers: rmp::random::range(int, int)\n'
                               'void f() { int ranged = 0; }\n',
                 "b_prop.cpp covers rmp::random::range(int, int), and its code never calls range"),
            ]
            for name, text, says in cases:
                with self.subTest(says=says):
                    (folder / name).write_text(text)
                    found = description_problems(folder) + unbacked_covers(folder)
                    (folder / name).unlink()
                    self.assertEqual(len(found), 1, found)
                    self.assertIn(says, found[0])

    def test_the_name_a_covers_line_asks_for(self):
        for key, name in [("rmp::random::range(int, int)", "range"),
                          ("rmp::global [template]", "global"),
                          ("rmp::Value::Value(const S &) [template]", "Value"),
                          ("rmp::Value::Ref::operator const rmp::Value &() const", None),
                          ("rmp::Callback::operator() [template]", None),
                          ("rmp::Object::~Object()", None)]:
            with self.subTest(key=key):
                self.assertEqual(spelled_as(key), name)


# ---------------------------------------------------------------------------
# What the nightly runs found: one issue per signature
# ---------------------------------------------------------------------------

ASAN = """INFO: Seed: 1234
==4242==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x602000000011 at pc 0x55d1
READ of size 1 at 0x602000000011 thread T0
    #0 0x55d1aa in rmp::save::detail::decode(std::vector<unsigned char> const&, rmp::Value*, bool) /work/src/rmp/save.cpp:351:12
    #1 0x55d1bb in (anonymous namespace)::walk(rmp::Value const&) /work/src/rmp/save.cpp:90:3
    #2 0x55d1cc in LLVMFuzzerTestOneInput /work/tests/fuzz/save_decode.cpp:20:5
    #3 0x55d1dd in fuzzer::Fuzzer::ExecuteCallback(unsigned char const*, unsigned long) (/work/build/fuzz/fuzz_save_decode+0x123)

SUMMARY: AddressSanitizer: heap-buffer-overflow /work/src/rmp/save.cpp:351:12 in decode
"""
UBSAN = """/w/tests/e2e/script.cpp:196:24: runtime error: signed integer overflow: 899999999 * 10 cannot be represented in type 'int'
    #0 0x558dc07f9cae in e2e::(anonymous namespace)::number(std::basic_string_view<char, std::char_traits<char>>) /w/tests/e2e/script.cpp:196:24
    #1 0x558dc07f79d9 in e2e::readable_type[abi:cxx11](std::basic_string_view<char, std::char_traits<char>>) /w/tests/e2e/script.cpp:535:43
    #2 0x558dc07d02b4 in LLVMFuzzerTestOneInput /w/tests/fuzz/e2e_script.cpp:118:34

SUMMARY: UndefinedBehaviorSanitizer: signed-integer-overflow /w/tests/e2e/script.cpp:196:24
"""


class FindingTest(unittest.TestCase):
    """tools/finding.py: what makes two failures one issue."""

    finding = load_tool("finding")

    def test_a_sanitizer_report_is_its_kind_and_our_top_frames(self):
        sig, what = self.finding.signature("fuzz", "save_decode", ASAN)
        self.assertEqual(what, "heap-buffer-overflow in rmp::save::detail::decode < "
                               "(anonymous namespace)::walk")
        # Another run: other addresses, the lines moved by an edit -- the same failure.
        moved = re.sub(r"0x[0-9a-f]+", "0xdead", ASAN).replace(":351:12", ":360:4")
        self.assertEqual(self.finding.signature("fuzz", "save_decode", moved)[0], sig)
        # Another function on top is another failure, and so is another target.
        other = ASAN.replace("(anonymous namespace)::walk", "rmp::save::detail::read_header")
        self.assertNotEqual(self.finding.signature("fuzz", "save_decode", other)[0], sig)
        self.assertNotEqual(self.finding.signature("fuzz", "value_json", ASAN)[0], sig)
        # A name that starts like a tool's frame is still ours; main itself is not.
        menu = ASAN.replace("rmp::save::detail::decode", "main_menu::draw")
        self.assertTrue(self.finding.signature("fuzz", "x", menu)[1].startswith(
            "heap-buffer-overflow in main_menu::draw < "))
        self.assertEqual(self.finding.our_frames("    #0 0x1 in main /w/a.cpp:1\n"
                                                 "    #1 0x2 in ours() /w/b.cpp:2\n"), ["ours"])
        sig, what = self.finding.signature("fuzz", "e2e_script", UBSAN)
        self.assertEqual(what, "signed-integer-overflow in e2e::(anonymous namespace)::number < "
                               "e2e::readable_type")

    def test_an_oracle_a_property_a_test_and_a_floor(self):
        cases = [
            ("fuzz", "x", "Executed a\nFUZZ ORACLE FAILED: a refusal says why\n==1== deadly signal",
             "oracle: a refusal says why"),
            ("property", "rapidcheck",
             "TEST SUITE: property: gen\nTEST CASE:  a drawn Value is the tree\n"
             "Falsifiable after 23 tests\nRC_ASSERT(rmp::test::gen::depth(v) <= 66)\n",
             "property: a drawn Value is the tree / RC_ASSERT(rmp::test::gen::depth(v) <= 66)"),
            ("hypothesis", "hypothesis",
             "FAIL: test_x (cpp_text_property_test.BlankTest.test_x)\nAssertionError\n",
             "hypothesis: test_x (cpp_text_property_test.BlankTest.test_x)"),
            ("mutation", "random",
             "  FAIL  random                78.00%  39/50 detected, 0 equivalent -- below its "
             "floor of 80\n", "mutation: random below its floor"),
            ("mutation", "random", "FAIL: no mull-runner here\n", "FAIL: no mull-runner here"),
        ]
        for kind, target, log, says in cases:
            with self.subTest(says=says):
                self.assertEqual(self.finding.signature(kind, target, log)[1], says)

    def test_a_property_is_its_own_case_and_its_own_assertion(self):
        """property_test's log as doctest prints it, one block a failing case:
        the case and what failed in it come from the same block. Read as one
        text, the first case went with the second case's RC_ASSERT -- a
        failure that never happened, filed under a signature nobody can
        find again (seen with the property of gen_prop.cpp made to fail)."""
        rule = "=" * 79
        log = (f"[doctest] doctest version is \"2.5.0\"\n{rule}\n"
               "/w/tests/property/gen_prop.cpp:113:\nTEST SUITE: property: gen\n"
               "TEST CASE:  value() reaches past what a save reads back, and no further\n\n"
               "/w/tests/property/gen_prop.cpp:124: ERROR: CHECK( std::ranges::all_of(drawn, f) )"
               " is NOT correct!\n  values: CHECK( false )\n\n"
               f"Using configuration: max_success=200 seed=20261010\n{rule}\n"
               "/w/tests/property/gen_prop.cpp:140:\nTEST SUITE: property: gen\n"
               "TEST CASE:  a drawn Value is the tree depth() measures, through a copy\n\n"
               "/w/tests/property/check.h:43: ERROR: CHECK( held ) is NOT correct!\n"
               "  values: CHECK( false )\n  logged: a copy has the same type, size and depth\n"
               "Falsifiable after 4 tests and 35 shrinks\n\n/w/tests/property/gen_prop.cpp:149:\n"
               "RC_ASSERT(rmp::test::gen::depth(v) <= rmp::test::gen::DEEPEST)\n\n"
               f"Expands to:\n66 <= 65\n\n{rule}\n"
               "[doctest] test cases:  6 |  4 passed | 2 failed | 0 skipped\n")
        self.assertEqual(self.finding.signature("property", "rapidcheck", log)[1],
                         "property: value() reaches past what a save reads back, and no further"
                         " / CHECK( std::ranges::all_of(drawn, f) )")
        second = log[log.index("Using configuration"):]
        self.assertEqual(self.finding.signature("property", "rapidcheck", second)[1],
                         "property: a drawn Value is the tree depth() measures, through a copy"
                         " / RC_ASSERT(rmp::test::gen::depth(v) <= rmp::test::gen::DEEPEST)")

    def test_one_issue_per_signature(self):
        """`report` against a stand-in gh: a new signature is an issue, one
        that is open already is a comment on it, and one that was closed is a
        new issue that names the closed one -- the docstring said so from the
        start, and the first version asked gh for open issues only."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            found = root / "findings"
            found.mkdir()
            record = self.finding.record("fuzz", "save_decode", ASAN, "bash tools/fuzz.sh replay",
                                         "https://run")
            (found / "fuzz-save_decode.json").write_text(json.dumps(record))
            calls = root / "calls"
            gh = root / "gh"
            listed = root / "listed.json"
            created = root / "created.md"

            def report(issues):
                listed.write_text(json.dumps(issues))
                gh.write_text("#!/bin/sh\n"
                              f'echo "$*" >> "{calls}"\n'
                              'case "$1 $2" in\n'
                              f'  "issue list") cat "{listed}" ;;\n'
                              f'  "issue create") cat > "{created}"; echo https://issue/7 ;;\n'
                              '  "issue comment") cat > /dev/null ;;\n'
                              'esac\n')
                gh.chmod(0o755)
                calls.write_text("")
                created.write_text("")
                got = subprocess.run([sys.executable, str(TOOLS / "finding.py"), "report",
                                      str(found)], capture_output=True, text=True,
                                     env={**os.environ, "PATH": f"{root}:{os.environ['PATH']}"})
                self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
                return [line.split()[:2] for line in calls.read_text().splitlines()]

            self.assertEqual(report([]), [["label", "create"], ["issue", "list"],
                                          ["issue", "create"]])
            self.assertNotIn("came back", created.read_text())
            self.assertIn("--state all", calls.read_text())
            self.assertEqual(report([{"number": 7, "title": record["title"], "state": "OPEN"}]),
                             [["label", "create"], ["issue", "list"], ["issue", "comment"]])
            # A title that only mentions the signature elsewhere is not its issue.
            self.assertEqual(report([{"number": 8, "title": "about " + record["signature"],
                                      "state": "OPEN"}]),
                             [["label", "create"], ["issue", "list"], ["issue", "create"]])
            self.assertNotIn("came back", created.read_text())
            # Closed: not reopened, and not forgotten either.
            self.assertEqual(report([{"number": 5, "title": record["title"], "state": "CLOSED"}]),
                             [["label", "create"], ["issue", "list"], ["issue", "create"]])
            self.assertIn("It came back.** The same signature was closed before: #5.",
                          created.read_text())
            self.assertIn("bash tools/fuzz.sh replay", created.read_text())
            # Closed once and open again: the open one is the issue.
            self.assertEqual(report([{"number": 5, "title": record["title"], "state": "CLOSED"},
                                     {"number": 9, "title": record["title"], "state": "OPEN"}]),
                             [["label", "create"], ["issue", "list"], ["issue", "comment"]])
            self.assertIn("issue comment 9 ", calls.read_text())
        self.assertIn(f"[{record['signature']}]", record["title"])
        self.assertTrue(record["title"].startswith("nightly-finding: fuzz save_decode: "))
        self.assertIn("bash tools/fuzz.sh replay", record["body"])
        self.assertIn("ERROR: AddressSanitizer", record["body"])


# ---------------------------------------------------------------------------
# Hypothesis, RapidCheck and where exceptions may be
# ---------------------------------------------------------------------------

class HypothesisProfilesTest(unittest.TestCase):

    PROBE = ("import sys, unittest\n"
             "sys.modules['hypothesis'] = None\n"  # importing it raises ImportError
             "sys.path.insert(0, 'tests')\n"
             "try:\n"
             "    import hypothesis_profiles\n"
             "except unittest.SkipTest as e:\n"
             "    print('skipped:', e)\n")

    def probe(self, **env):
        return subprocess.run([sys.executable, "-c", self.PROBE], cwd=REPO, capture_output=True,
                              text=True, env={**os.environ, "RMP_HYPOTHESIS_SKIP_SAID": "",
                                              "RMP_REQUIRE_HYPOTHESIS": "", **env})

    def test_without_hypothesis_a_skip_that_says_so(self):
        got = self.probe()
        self.assertEqual(got.returncode, 0, got.stderr)
        self.assertEqual(got.stdout, "skipped: hypothesis is not installed\n")
        self.assertIn("  skip  the Hypothesis property tests: hypothesis is not installed",
                      got.stderr)

    def test_without_hypothesis_a_failure_where_it_is_required(self):
        got = self.probe(RMP_REQUIRE_HYPOTHESIS="1")
        self.assertNotEqual(got.returncode, 0)
        self.assertRegex(got.stderr, r"(ModuleNotFound|Import)Error: import of hypothesis")
        self.assertEqual(got.stdout, "")

    def test_the_profiles_are_what_they_say(self):
        try:
            import hypothesis
        except ImportError:
            if os.environ.get("RMP_REQUIRE_HYPOTHESIS") == "1":
                raise
            self.skipTest("hypothesis is not installed")
        sys.path.insert(0, str(REPO / "tests"))
        import hypothesis_profiles  # noqa: F401
        ci = hypothesis.settings.get_profile("ci")
        self.assertEqual((ci.derandomize, ci.max_examples, ci.database, ci.deadline),
                         (True, 200, None, None))
        nightly = hypothesis.settings.get_profile("nightly")
        self.assertEqual((nightly.derandomize, nightly.max_examples), (False, 5000))
        got = subprocess.run([sys.executable, "-c", "import sys; sys.path.insert(0, 'tests'); "
                              "import hypothesis_profiles"], cwd=REPO, capture_output=True,
                             text=True, env={**os.environ, "RMP_HYPOTHESIS_PROFILE": "weekly"})
        self.assertNotEqual(got.returncode, 0)
        self.assertIn("RMP_HYPOTHESIS_PROFILE='weekly': one of ci, nightly", got.stderr)


def throws(root: Path) -> list[str]:
    """Every `throw` and every RapidCheck include under src/rmp/ and
    include/rmp/, comments and strings aside."""
    out = []
    for folder in ("src/rmp", "include/rmp"):
        for path in sorted((root / folder).rglob("*")):
            if path.suffix not in (".h", ".hpp", ".c", ".cpp") or not path.is_file():
                continue
            raw = path.read_text(encoding="utf-8", errors="replace")
            code = cpp_text.blank(raw)
            lines = cpp_text.Lines(code)
            rel = path.relative_to(root).as_posix()
            for m in re.finditer(r"\bthrow\b", code):
                out.append(f"{rel}:{lines.of(m.start())}: throws")
            for m in re.finditer(r"#\s*include\s*[<\"]rapidcheck", raw):
                out.append(f"{rel}:{lines.of(m.start())}: includes RapidCheck")
    return out


class ExceptionsStayInTheTestsTest(unittest.TestCase):
    """RapidCheck's RC_ASSERT throws, and Omar allowed that in the test
    binaries and nowhere else (2026-10-10): the framework never throws."""

    def test_the_framework_never_throws(self):
        self.assertEqual(throws(REPO), [])

    def test_the_check_sees_a_throw_and_an_include(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "src" / "rmp").mkdir(parents=True)
            (root / "include" / "rmp").mkdir(parents=True)
            (root / "src" / "rmp" / "a.cpp").write_text(
                "// we never throw\nconst char *s = \"throw\";\nvoid f() {\n  throw 1;\n}\n")
            (root / "include" / "rmp" / "b.h").write_text("#include <rapidcheck.h>\n")
            self.assertEqual(throws(root), ["src/rmp/a.cpp:4: throws",
                                            "include/rmp/b.h:1: includes RapidCheck"])


def deep_workflow_problems(workflow: dict, script_text: str) -> list[str]:
    """What would let .github/workflows/deep.yml run where it must not, or not
    run what it says."""
    out = []
    jobs = workflow.get("jobs") or {}
    on = workflow.get("on", workflow.get(True)) or {}  # YAML 1.1 reads `on` as True
    guard = "github.repository == 'omardev29/raylib_multiplatform'"
    if " ".join(str(jobs.get("plan", {}).get("if", "")).split()) != guard:
        out.append(f"the plan job is not guarded by {guard}")
    for name, job in jobs.items():
        if name == "plan":
            continue
        needs = job.get("needs") or []
        needs = [needs] if isinstance(needs, str) else needs
        if "plan" not in needs:
            out.append(f"job {name} does not need plan, so the guard does not stop it")
        condition = " ".join(str(job.get("if", "")).split())
        if "always()" in condition and "needs.plan.result == 'success'" not in condition:
            out.append(f"job {name} runs always(), past a skipped plan")
    concurrency = workflow.get("concurrency") or {}
    if concurrency.get("group") != "deep" or concurrency.get("cancel-in-progress") is not False:
        out.append("the runs do not share one concurrency group that cancels nothing: two "
                   "at once open two issues for one signature, and a cancelled one loses "
                   "what it found")
    crons = [entry.get("cron") for entry in on.get("schedule") or []]
    weekly = re.findall(r"\$SCHEDULE\" = '([^']+)' \]; then MODE=weekly", script_text)
    if len(weekly) != 1 or weekly[0] not in crons:
        out.append(f"the plan job's weekly cron {weekly} is not one of the schedules {crons}")
    return out


class DeepWorkflowTest(unittest.TestCase):
    """deep.yml runs on this repository and nowhere else -- a repository made
    from this one is a copy, not a fork, and would open issues on itself --
    and its Sunday schedule is the one the plan reads as weekly; and one run
    at a time, none cancelled."""

    def load(self):
        try:
            import yaml
        except ImportError:
            if Path("/etc/raylib-build-image.json").is_file():
                self.fail("PyYAML is missing inside the build image")
            self.skipTest("PyYAML not installed (it ships in the build image)")
        text = (REPO / ".github" / "workflows" / "deep.yml").read_text()
        return yaml, text

    def test_guarded_and_scheduled(self):
        yaml, text = self.load()
        self.assertEqual(deep_workflow_problems(yaml.safe_load(text), text), [])

    def test_the_check_sees_each_mistake(self):
        yaml, text = self.load()
        cases = [
            ("    if: github.repository == 'omardev29/raylib_multiplatform'\n", "",
             "the plan job is not guarded"),
            ("    if: always() && needs.plan.result == 'success'\n", "    if: always()\n",
             "job report runs always(), past a skipped plan"),
            ("    - cron: '30 4 * * 0'", "    - cron: '30 5 * * 0'",
             "weekly cron ['30 4 * * 0'] is not one of the schedules"),
            ("  group: deep\n", "  group: deep-${{ github.event.schedule || inputs.mode }}\n",
             "do not share one concurrency group"),
            ("  cancel-in-progress: false\n", "  cancel-in-progress: true\n",
             "do not share one concurrency group"),
        ]
        for before, after, says in cases:
            with self.subTest(says=says):
                self.assertEqual(text.count(before), 1, before)
                broken = text.replace(before, after)
                found = deep_workflow_problems(yaml.safe_load(broken), broken)
                self.assertEqual(len(found), 1, found)
                self.assertIn(says, found[0])
        workflow = yaml.safe_load(text)
        workflow["jobs"]["fuzz"]["needs"] = "other"
        self.assertEqual(deep_workflow_problems(workflow, text),
                         ["job fuzz does not need plan, so the guard does not stop it"])


class DeepCmakeTest(unittest.TestCase):
    """tests/deep.cmake comes in through one OPTIONAL line, after everything
    it calls, and builds nothing a plain build asks for."""

    def test_one_optional_line_after_what_it_uses(self):
        text = (REPO / "CMakeLists.txt").read_text()
        lines = [i for i, line in enumerate(text.splitlines())
                 if "tests/deep.cmake" in line and not line.lstrip().startswith("#")]
        self.assertEqual(len(lines), 1)
        line = text.splitlines()[lines[0]].strip()
        self.assertEqual(line, 'include("${CMAKE_CURRENT_SOURCE_DIR}/tests/deep.cmake" OPTIONAL)')
        for needed in ("function(rmp_link_framework", "function(rmp_apply_warnings",
                       "function(rmp_apply_compile_flags", "add_library(rmp STATIC"):
            with self.subTest(needed=needed):
                self.assertLess(text.index(needed), text.index(line))
        self.assertNotIn("rapidcheck", text.replace(line, ""))

    def test_every_target_is_built_only_by_name(self):
        deep = (REPO / "tests" / "deep.cmake").read_text()
        made = re.findall(r"^\s*add_(?:executable|library)\((\S+)([^\n]*)", deep, re.M)
        self.assertEqual(sorted(name for name, _ in made),
                         ["${_target}", "property_test", "rapidcheck"])
        for name, rest in made:
            with self.subTest(target=name):
                self.assertIn(" EXCLUDE_FROM_ALL", rest)
        self.assertEqual(re.findall(r"target_link_libraries\((\w+) PRIVATE rapidcheck\)", deep),
                         ["property_test"])


if __name__ == "__main__":
    unittest.main()
