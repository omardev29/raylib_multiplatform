"""Tests for tools/coverage_report.py and the probe of tools/coverage_check.sh.

Run with:  python3 -m unittest discover -s tests -p coverage_report_test.py

Every FAIL the report can say is made to happen here, on a tree built in a
temporary folder: a src/rmp file, an LCOV export written by hand, the floor
file, the list of untested functions and a public header that clang reads.
Each test asks for the exact sentence and the exit status, and the passing
fixture is the same tree with nothing wrong in it -- so a gate that also
failed the good tree would be caught as surely as one that passed the bad.

The functions half needs clang++ and llvm-cxxfilt. Without them those tests
skip, unless RMP_REQUIRE_CLANG=1 or this is the build image, which has both:
there a skip would be a gate's tests that proved nothing.
"""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import coverage_report as report  # noqa: E402

IN_BUILD_IMAGE = Path("/etc/raylib-build-image.json").is_file()
NOWHERE = Path("/nonexistent/raylib-build-image.json")


def tools_or_skip(case: unittest.TestCase):
    missing = [t for t in ("clang++", "llvm-cxxfilt") if shutil.which(t) is None]
    if missing:
        if IN_BUILD_IMAGE or os.environ.get("RMP_REQUIRE_CLANG") == "1":
            case.fail(f"{' and '.join(missing)} missing, and the image has them")
        case.skipTest(f"no {' or '.join(missing)}: the report cannot read the headers")


# One public header: what is public, and each thing that is not.
HEADER = """\
#pragma once
namespace rmp {
int called(int x);
int never(int x);
struct Box {
    Box();
    int get() const;
    void set(int v);
    Box(const Box &) = default;
    void gone() = delete;
  private:
    int hidden();
    int _v = 0;
};
template <class T> T twice(T v) { return v + v; }
namespace detail {
int internal();
}
} // namespace rmp
"""

# Called (count > 0) unless the test says otherwise. The constructor is the
# C2 symbol -- the one clang emits a body for -- and the header's AST names C1.
CALLS = {
    "_ZN3rmp6calledEi": 3,
    "_ZN3rmp5neverEi": 0,
    "_ZN3rmp3BoxC2Ev": 1,
    "_ZNK3rmp3Box3getEv": 2,
    "_ZN3rmp3Box3setEi": 1,
    "_ZN3rmp5twiceIiEET_S1_": 4,
    "_ZN3rmp6detail8internalEv": 0,
}
API = {"rmp::called(int)", "rmp::never(int)", "rmp::Box::Box()", "rmp::Box::get() const",
       "rmp::Box::set(int)", "rmp::twice [template]"}

SOURCE = "int f(int x) {\n    return x;\n}\n" * 10  # 30 lines, all "code"


class Tree:
    """A tree for the report: src/rmp/, include/rmp/api.h, the two lists."""

    def __init__(self, root: Path):
        self.root = root
        (root / "include" / "rmp").mkdir(parents=True)
        (root / "include" / "rmp" / "api.h").write_text(HEADER)
        (root / "src" / "rmp").mkdir(parents=True)
        self.lines: dict[str, dict[int, int]] = {}
        self.calls = dict(CALLS)
        self.floor = root / "floor.txt"
        self.untested = root / "untested.txt"
        # The good tree: one file, 20 of 30 lines run, at its floor of 66.
        self.source("src/rmp/a.cpp", SOURCE, run=20, of=30)
        self.floor.write_text("# a comment\nsrc/rmp/a.cpp 66\n")
        self.untested.write_text("# a comment\nrmp::never(int)\n")

    def source(self, rel: str, text: str, run: int = 0, of: int = 0):
        """A file with `of` mapped lines, the first `run` of them run."""
        (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
        (self.root / rel).write_text(text)
        if of:
            self.lines[rel] = {n: (1 if n <= run else 0) for n in range(1, of + 1)}

    def lcov(self) -> Path:
        out = []
        for rel, lines in self.lines.items():
            out.append(f"SF:{self.root / rel}")
            out += [f"DA:{n},{c}" for n, c in sorted(lines.items())]
            out.append("end_of_record")
        out.append(f"SF:{self.root / 'include/rmp/api.h'}")
        for name, count in self.calls.items():
            out += [f"FN:1,{name}", f"FNDA:{count},{name}"]
        out.append("end_of_record")
        path = self.root / "coverage.lcov"
        path.write_text("\n".join(out) + "\n")
        return path

    def run(self, *extra: str, image: bool = True):
        """(exit status, what it printed). `image`: as inside the build image."""
        argv = ["--lcov", str(self.lcov()), "--root", str(self.root),
                "--floor", str(self.floor), "--untested", str(self.untested),
                "--out", str(self.root / "out"), *extra]
        manifest = Path(__file__) if image else NOWHERE  # a file that exists, or not
        out = io.StringIO()
        with mock.patch.object(report, "IMAGE_MANIFEST", manifest), \
                contextlib.redirect_stdout(out):
            code = report.main(argv)
        return code, out.getvalue()


class ReportTestCase(unittest.TestCase):
    def setUp(self):
        tools_or_skip(self)
        self.tmp = tempfile.TemporaryDirectory()
        self.tree = Tree(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def fails(self, said: str, *extra: str, image: bool = True):
        code, out = self.tree.run(*extra, image=image)
        self.assertEqual(code, 1, out)
        self.assertIn(said, out)
        return out


class GoodTreeTest(ReportTestCase):
    def test_the_good_tree_passes_in_the_image_and_out_of_it(self):
        for image in (True, False):
            with self.subTest(image=image):
                code, out = self.tree.run(image=image)
                self.assertEqual(code, 0, out)
                self.assertIn("PASS: 66.67% of src/rmp's lines run", out)
                self.assertNotIn("FAIL", out)

    def test_it_reads_the_api_it_was_given(self):
        """Public, outside detail, not private, implicit, defaulted or deleted;
        a template once, by name."""
        api = report.public_api("clang++", self.tree.root / "include",
                                ["-std=c++20", f"-I{self.tree.root / 'include'}"])
        self.assertEqual(set(api), API)

    def test_it_writes_what_it_measured(self):
        code, out = self.tree.run()
        self.assertEqual(code, 0, out)
        got = (self.tree.root / "out" / "untested_api.txt").read_text()
        self.assertEqual([l for l in got.splitlines() if not l.startswith("#")],
                         ["rmp::never(int)"])
        self.assertIn("| `src/rmp/a.cpp` | 30 | 20 | 66.67 | 66 |",
                      (self.tree.root / "out" / "summary.md").read_text())
        self.assertTrue((self.tree.root / "out" / "coverage.json").is_file())


class FloorTest(ReportTestCase):
    def test_below_the_floor_fails_in_the_image(self):
        self.tree.floor.write_text("src/rmp/a.cpp 67\n")
        self.fails("src/rmp/a.cpp: 66.67% of its lines run, below its floor of 67%")

    def test_below_the_floor_is_a_note_outside_it_and_strict_fails_it(self):
        self.tree.floor.write_text("src/rmp/a.cpp 67\n")
        code, out = self.tree.run(image=False)
        self.assertEqual(code, 0, out)
        self.assertIn("note: src/rmp/a.cpp: 66.67% of its lines run, below its floor", out)
        self.fails("below its floor of 67%", "--strict", image=False)

    def test_two_points_over_the_floor_is_stale(self):
        self.tree.floor.write_text("src/rmp/a.cpp 64\n")  # 66.67 >= 64 + 2
        self.fails("src/rmp/a.cpp: 66.67% is 2 points or more over its floor of 64%: "
                   "raise the line to `src/rmp/a.cpp 66`")

    def test_just_under_two_points_over_is_not(self):
        self.tree.source("src/rmp/a.cpp", SOURCE, run=21, of=30)  # 70.00
        self.tree.floor.write_text("src/rmp/a.cpp 69\n")  # 70 < 69 + 2
        code, out = self.tree.run()
        self.assertEqual(code, 0, out)
        self.tree.floor.write_text("src/rmp/a.cpp 68\n")  # 70 >= 68 + 2: the edge is stale
        self.fails("is 2 points or more over its floor of 68%")

    def test_exactly_at_the_floor_passes(self):
        self.tree.source("src/rmp/a.cpp", SOURCE, run=21, of=30)  # 70.00 exactly
        self.tree.floor.write_text("src/rmp/a.cpp 70\n")
        code, out = self.tree.run()
        self.assertEqual(code, 0, out)

    def test_a_file_with_lines_and_no_floor_fails_everywhere(self):
        self.tree.source("src/rmp/b.cpp", SOURCE, run=3, of=4)
        for image in (True, False):
            with self.subTest(image=image):
                self.fails("src/rmp/b.cpp has 4 lines and no floor: add `src/rmp/b.cpp 75` "
                           "to tools/coverage_floor.txt", image=image)

    def test_a_floor_for_a_file_that_is_not_there_fails(self):
        self.tree.floor.write_text("src/rmp/a.cpp 66\nsrc/rmp/old.cpp 50\n")
        self.fails("src/rmp/old.cpp is not in the coverage report, and "
                   "tools/coverage_floor.txt has a line for it: remove the line", image=False)

    def test_a_malformed_floor_file_fails(self):
        for text, said in (("src/rmp/a.cpp 66\nsrc/rmp/a.cpp 66\n", "is listed twice"),
                           ("src/rmp/a.cpp sixty\n", "not understood: `src/rmp/a.cpp sixty`"),
                           ("src/rmp/a.cpp 101\n", "not understood"),
                           ("tests/a.cpp 66\n", "not understood"),
                           ("src/rmp/a.cpp\n", "not understood")):
            with self.subTest(text=text):
                self.tree.floor.write_text(text)
                self.fails(said, image=False)

    def test_no_floor_file_fails(self):
        self.tree.floor.unlink()
        self.fails("does not exist: --update in the image writes it", image=False)


class MissingFileTest(ReportTestCase):
    def test_a_source_the_report_does_not_hold_fails(self):
        self.tree.source("src/rmp/ui/lost.cpp", "int lost() { return 1; }\n")
        for image in (True, False):
            with self.subTest(image=image):
                self.fails("src/rmp/ui/lost.cpp is not in the coverage report: no test binary "
                           "holds its code", image=image)

    def test_a_c_file_too(self):
        self.tree.source("src/rmp/lost.c", "/* c */\nint lost(void) { return 1; }\n")
        self.fails("src/rmp/lost.c is not in the coverage report")

    def test_a_vendored_implementation_has_no_code_of_its_own(self):
        """cjson_impl.c: comments, a #define and an #include. Its library's
        lines are in a system header, which clang does not map."""
        self.tree.source("src/rmp/vendor_impl.c",
                         "/* a block\n * comment */\n// a line\n#define LIMIT 64\n"
                         "#include <stdio.h>\n\n")
        code, out = self.tree.run()
        self.assertEqual(code, 0, out)

    def test_a_header_with_no_mapped_line_needs_nothing(self):
        self.tree.source("src/rmp/thing_internal.h", "#pragma once\nint thing();\n")
        code, out = self.tree.run()
        self.assertEqual(code, 0, out)


class MarkerTest(ReportTestCase):
    def marked(self, *middle: str) -> str:
        return "".join(f"int f{i}() {{ return {i}; }}\n" for i in range(10)) + \
            "".join(m + "\n" for m in middle)

    def test_marked_lines_are_counted_apart(self):
        # Line 11 OFF, 12-16 out, 17 ON: the 23 other mapped lines run, the 5 out do not.
        text = "\n" * 10 + "// RMP_COVERAGE_OFF(needs a GPU)\n" + "\n" * 5 + \
            "// RMP_COVERAGE_ON\n"
        self.tree.source("src/rmp/a.cpp", text, run=0, of=0)
        self.tree.lines["src/rmp/a.cpp"] = {n: (0 if 12 <= n <= 16 else 1) for n in range(1, 31)
                                            if n not in (11, 17)}
        self.tree.floor.write_text("src/rmp/a.cpp 100\n")
        code, out = self.tree.run()
        self.assertEqual(code, 0, out)
        self.assertRegex(out, r"src/rmp/a\.cpp +23/23 +100\.00%  floor 100 \(5 marked out\)")
        self.assertIn("(5 lines marked out)", out)

    def test_a_marker_without_a_reason_fails(self):
        for spelt in ("// RMP_COVERAGE_OFF()", "// RMP_COVERAGE_OFF(   )"):
            with self.subTest(spelt=spelt):
                self.tree.source("src/rmp/a.cpp", self.marked(spelt, "// RMP_COVERAGE_ON"))
                self.fails("src/rmp/a.cpp:11: RMP_COVERAGE_OFF() without a reason", image=False)

    def test_an_off_never_closed_fails(self):
        self.tree.source("src/rmp/a.cpp", self.marked("// RMP_COVERAGE_OFF(a store)"))
        self.fails("src/rmp/a.cpp:11: RMP_COVERAGE_OFF is never closed", image=False)

    def test_an_on_with_no_off_fails(self):
        self.tree.source("src/rmp/a.cpp", self.marked("// RMP_COVERAGE_ON"))
        self.fails("src/rmp/a.cpp:11: RMP_COVERAGE_ON with no RMP_COVERAGE_OFF before it",
                   image=False)

    def test_an_off_inside_an_off_fails(self):
        self.tree.source("src/rmp/a.cpp", self.marked("// RMP_COVERAGE_OFF(one)",
                                                      "// RMP_COVERAGE_OFF(two)",
                                                      "// RMP_COVERAGE_ON"))
        self.fails("src/rmp/a.cpp:12: RMP_COVERAGE_OFF inside the one opened at line 11",
                   image=False)

    def test_a_marker_is_a_line_of_its_own(self):
        for spelt in ("int x; // RMP_COVERAGE_OFF(why)", "/* RMP_COVERAGE_OFF(why) */"):
            with self.subTest(spelt=spelt):
                self.tree.source("src/rmp/a.cpp", self.marked(spelt, "// RMP_COVERAGE_ON"))
                self.fails("src/rmp/a.cpp:11: a marker is a comment line of its own",
                           image=False)

    def test_the_markers_alone(self):
        """The parser without clang: where the lines are, and what it says."""
        text = "a\n// RMP_COVERAGE_OFF(the store)\nb\nc\n// RMP_COVERAGE_ON\nd\n"
        self.assertEqual(report.markers(text, "x.cpp"), ({3, 4}, []))
        self.assertEqual(report.markers("a\n", "x.cpp"), (set(), []))


class UntestedTest(ReportTestCase):
    def test_a_new_uncalled_function_fails(self):
        self.tree.calls["_ZN3rmp3Box3setEi"] = 0
        self.fails("rmp::Box::set(int) (")
        out = self.fails("is public and no test calls it: test it")
        self.assertNotIn("rmp::never(int) (", out)  # listed: not a failure

    def test_it_is_a_note_outside_the_image(self):
        self.tree.calls["_ZN3rmp3Box3setEi"] = 0
        code, out = self.tree.run(image=False)
        self.assertEqual(code, 0, out)
        self.assertIn("note: rmp::Box::set(int)", out)

    def test_a_listed_function_that_is_called_now_fails(self):
        self.tree.calls["_ZN3rmp5neverEi"] = 1
        self.fails("tools/untested_api.txt lists `rmp::never(int)`, and a test calls it now: "
                   "remove the line")

    def test_a_listed_function_that_is_gone_fails_everywhere(self):
        self.tree.untested.write_text("rmp::never(int)\nrmp::removed(float)\n")
        self.fails("tools/untested_api.txt lists `rmp::removed(float)`, and it is no public "
                   "function any more: remove the line", image=False)

    def test_a_function_never_emitted_is_uncalled(self):
        """An inline function no test referenced has no symbol at all."""
        del self.tree.calls["_ZN3rmp6calledEi"]
        self.fails("rmp::called(int) (")

    def test_a_template_is_called_by_any_instantiation(self):
        self.tree.calls["_ZN3rmp5twiceIiEET_S1_"] = 0
        self.tree.calls["_ZN3rmp5twiceIfEET_S1_"] = 1
        code, out = self.tree.run()
        self.assertEqual(code, 0, out)
        self.tree.calls["_ZN3rmp5twiceIfEET_S1_"] = 0
        self.fails("rmp::twice [template] (")

    def test_either_constructor_symbol_calls_the_constructor(self):
        del self.tree.calls["_ZN3rmp3BoxC2Ev"]
        self.tree.calls["_ZN3rmp3BoxC1Ev"] = 1
        code, out = self.tree.run()
        self.assertEqual(code, 0, out)
        self.tree.calls["_ZN3rmp3BoxC1Ev"] = 0
        self.fails("rmp::Box::Box() (")

    def test_a_local_symbol_with_its_file_counts(self):
        """llvm names a function with internal linkage `file.cpp:_Z...`."""
        del self.tree.calls["_ZN3rmp6calledEi"]
        self.tree.calls["a_test.cpp:_ZN3rmp6calledEi"] = 1
        code, out = self.tree.run()
        self.assertEqual(code, 0, out)

    def test_a_list_line_twice_fails(self):
        self.tree.untested.write_text("rmp::never(int)\nrmp::never(int)\n")
        self.fails("`rmp::never(int)` is listed twice", image=False)


class UpdateTest(ReportTestCase):
    def test_it_raises_the_floor_and_shrinks_the_list(self):
        self.tree.floor.write_text("src/rmp/a.cpp 60\n")
        self.tree.untested.write_text("rmp::never(int)\nrmp::Box::set(int)\n")
        self.tree.calls["_ZN3rmp6calledEi"] = 0  # newly uncalled: never added
        code, out = self.tree.run("--update")
        self.assertEqual(code, 0, out)
        floors = [l for l in self.tree.floor.read_text().splitlines() if not l.startswith("#")]
        self.assertEqual(floors, ["src/rmp/a.cpp                    66"])
        listed = [l for l in self.tree.untested.read_text().splitlines()
                  if not l.startswith("#")]
        self.assertEqual(listed, ["rmp::never(int)"])

    def test_it_never_lowers_a_floor(self):
        self.tree.floor.write_text("src/rmp/a.cpp 80\n")
        out = self.fails("nothing was written: --update raises floors and never lowers one",
                         "--update")
        self.assertIn("FAIL: src/rmp/a.cpp: 66.67% of its lines run, below its floor", out)
        self.assertEqual(self.tree.floor.read_text(), "src/rmp/a.cpp 80\n")

    def test_it_writes_nothing_out_of_a_wrong_run(self):
        self.tree.source("src/rmp/lost.cpp", "int lost() { return 1; }\n")
        before = self.tree.floor.read_text()
        self.fails("nothing was written: the problems above make the numbers wrong", "--update")
        self.assertEqual(self.tree.floor.read_text(), before)

    def test_it_refuses_outside_the_image(self):
        before = self.tree.floor.read_text()
        self.fails("--update writes numbers measured in the pinned build image", "--update",
                   image=False)
        self.assertEqual(self.tree.floor.read_text(), before)

    def test_it_starts_the_files_that_do_not_exist(self):
        self.tree.floor.unlink()
        self.tree.untested.unlink()
        code, out = self.tree.run("--update")
        self.assertEqual(code, 0, out)
        self.assertIn("src/rmp/a.cpp                    66\n", self.tree.floor.read_text())
        self.assertIn("rmp::never(int)\n", self.tree.untested.read_text())
        code, out = self.tree.run()
        self.assertEqual(code, 0, out)


class NamesTest(unittest.TestCase):
    """What a demangled instantiation is called, without its template
    arguments and its return type: what a template's name is matched on."""

    def test_plain_names(self):
        for demangled, name in (
                ("Player& rmp::Scene::spawn<Player>(int) const", "rmp::Scene::spawn"),
                ("(anonymous namespace)::Marker<1>& rmp::global<(anonymous namespace)::"
                 "Marker<1>>()", "rmp::global"),
                ("rmp::Handle<Player>::operator->() const", "rmp::Handle::operator->"),
                ("rmp::Callback<void (int)>::operator()(int)", "rmp::Callback::operator()"),
                ("bool rmp::operator==<int>(rmp::Handle<int> const&, rmp::Handle<int> const&)",
                 "rmp::operator=="),
                ("std::ostream& rmp::operator<<(std::ostream&, rmp::Value const&)",
                 "rmp::operator<<"),
                ("rmp::Handle<std::vector<std::vector<int>>>::~Handle()", "rmp::Handle::~Handle"),
                ("rmp::Ref<int>::operator bool() const", "rmp::Ref::operator bool"),
                ("rmp::cooperate()", "rmp::cooperate"),
                ("std::__cxx11::basic_string<char> rmp::name[abi:cxx11]<int>()", "rmp::name")):
            with self.subTest(demangled=demangled):
                self.assertEqual(report.plain_name(demangled), name)

    def test_signatures(self):
        for spelt, sig in (("bool (std::string_view, const Options &) const noexcept",
                            "(std::string_view, const Options &) const"),
                           ("auto (int) -> int", "(int)"),
                           ("std::function<void (int)> (int &&) &&", "(int &&) &&"),
                           ("void ()", "()")):
            with self.subTest(spelt=spelt):
                self.assertEqual(report.signature(spelt), sig)


class ProbeTest(unittest.TestCase):
    """tools/coverage_check.sh FAILS when it cannot measure, and never skips:
    a clang without the profile runtime (the image's before 2026-10 was one),
    and llvm tools of another release than clang's. Stand-ins first on PATH,
    so the real tools are never reached."""

    def probe(self, clang: str):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("clang", "clang++"):
                stub = Path(tmp) / name
                stub.write_text(clang)
                stub.chmod(0o755)
            got = subprocess.run(["bash", str(REPO / "tools" / "coverage_check.sh")],
                                 capture_output=True, text=True, stdin=subprocess.DEVNULL,
                                 env=dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}"))
        return got.returncode, got.stdout + got.stderr

    def test_a_clang_that_cannot_link_the_runtime_fails(self):
        major = "18"
        for tool in ("llvm-profdata", "llvm-cov", "llvm-cxxfilt"):
            if shutil.which(f"{tool}-{major}") is None:
                found = shutil.which(tool)
                if found is None:
                    self.skipTest(f"no {tool}: the probe stops before the link")
                ver = subprocess.run([found, "--version"], capture_output=True, text=True).stdout
                major = next((w.split(".")[0] for w in ver.split() if w[:1].isdigit()), major)
        code, out = self.probe(textwrap.dedent(f"""\
            #!/bin/sh
            case "$1" in
              -dumpversion) echo {major}.1.3 ;;
              --version) echo "clang version {major}.1.3" ;;
              *) echo "ld: cannot find libclang_rt.profile-x86_64.a" >&2; exit 1 ;;
            esac
            """))
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: clang++ cannot link a program with -fprofile-instr-generate", out)
        self.assertIn("ld: cannot find libclang_rt.profile-x86_64.a", out)
        self.assertIn("tools/dev_shell.sh bash tools/coverage_check.sh", out)
        self.assertNotIn("== coverage: build ==", out)

    def test_llvm_tools_of_another_release_fail(self):
        code, out = self.probe("#!/bin/sh\necho 99.0.0\n")
        self.assertEqual(code, 1, out)
        self.assertRegex(out, r"FAIL: (no llvm-profdata \(nor llvm-profdata-99\) for clang 99"
                              r"|llvm-profdata is LLVM \d+ and clang\+\+ is 99)")
        self.assertIn("tools/dev_shell.sh bash tools/coverage_check.sh", out)
        self.assertNotIn("== coverage: build ==", out)


if __name__ == "__main__":
    unittest.main()
