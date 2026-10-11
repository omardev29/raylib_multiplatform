"""Every tools/*_check.sh, seen red on the mistake it exists for and green on
the tree as it is.

"A test you have not seen go red is not a test, it is a hope with a name" --
and a gate is a test. Each check here runs the way `rmp test` and CI run it,
on a SCRATCH TREE: the script and what it reads, copied from the repository's
tracked files into a temporary folder (each script cds to its own
dirname/.., so the scratch is the tree it judges). Then a mistake is planted
in the scratch, and the check has to say FAIL and name it. Not a probe file
passed by name -- several of the checks have a FILE... mode, and the tests of
that live beside them -- but the tree mode CI runs, with the scoping and the
exceptions only the tree mode has.

GATES names every tools/*_check.sh, and EveryGateIsSeenRedTest holds that list
to the folder: a class here, or the class elsewhere that plants it red, and
why there. A new check script fails here until it is one or the other.

Planting found holes, each fixed with its case here (the commit says how):
repo_check.sh passed a tracked object file with a name git quotes; seam_check.sh
read a `//` inside a string as a comment and missed the call after it;
glibc_check.sh said "ok, static?" of a file that is no ELF at all;
workflow_check.sh passed a callee that asks to write what its caller only
lets it read, and was a traceback on `permissions: write-all` and on a
`workflow_call:` with nothing under it; examples_check.sh read neither a
second plain C example nor a header. And at review: glibc_check.sh took
objdump's translated words for a file it could not read (a static program
was a FAIL on a Spanish desktop), and header_cost.py was a traceback,
without the compiler's error, for a header that does not compile.

And two Python gates' counterexamples from their property tests, named:
header_cost.py's budget file with a note after a number, and
android_release_check.py's compile database cut short.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / "tools"
IN_BUILD_IMAGE = Path("/etc/raylib-build-image.json").is_file()

# Every tools/*_check.sh, and where it is seen red: "here" (a class of this
# file whose SCRIPT is it), or "<test file>::<class>: why there".
GATES = {
    "coverage_check.sh": "coverage_report_test.py::ProbeTest: it FAILS when it cannot "
                         "measure (a clang without the profile runtime, llvm tools of "
                         "another release); its floors are coverage_report.py's, seen red "
                         "in FloorTest. A clean run is a full instrumented build: the "
                         "coverage job runs it on every push",
    "cppcheck_check.sh": "configure_test.py::CppcheckCheckTest: a finding, a suppression "
                         "without its reason and one that matches nothing, each on a probe; "
                         "the tree needs cppcheck and the lint preset's compile database",
    "examples_check.sh": "here",
    "glibc_check.sh": "here",
    "header_check.sh": "here",
    "header_cost_check.sh": "here",
    "naming_check.sh": "here",
    "ownership_check.sh": "here",
    "portable_check.sh": "here",
    "render_check.sh": "configure_test.py::RenderCheckReadsTheExitTest: a stand-in cmake "
                       "and game, an exit status and each sanitizer's report after every "
                       "marker",
    "repo_check.sh": "here",
    "sanitize_check.sh": "here",
    "seam_check.sh": "here",
    "shell_pattern_check.sh": "configure_test.py::ShellPatternCheckTest: the pattern, across "
                              "a continued line, in a workflow's run block; and the tree",
    "shipped_check.sh": "here",
    "style_check.sh": "here",
    "versions_check.sh": "configure_test.py::VersionsCheckRunImageTest: a digest planted in "
                         "a copy of the tree, each way the two places can disagree",
    "workflow_check.sh": "here",
}


def tracked(*paths: str) -> list[str]:
    """The repository's tracked files under `paths`, or every file there when
    this is not a git checkout (mutmut's copy of the tree is not one)."""
    got = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--", *paths],
                         capture_output=True, text=True)
    if got.returncode == 0 and got.stdout:
        return [p for p in got.stdout.split("\0") if p and (REPO / p).is_file()]
    out = []
    for path in paths:
        here = REPO / path
        if here.is_file():
            out.append(path)
        else:
            out += [p.relative_to(REPO).as_posix() for p in here.rglob("*")
                    if p.is_file() and "build" not in p.relative_to(REPO).parts]
    return out


class Scratch:
    """A tree for one gate: the repository's tracked files under `paths`,
    copied, and `links` symlinked whole (thirdparty/, which only gets read).
    `plant` writes a file, `edit` changes one, `run` starts a script of
    tools/ the way rmp does: from the root, reading nothing on stdin."""

    def __init__(self, *paths: str, links=(), git=False):
        self._tmp = tempfile.TemporaryDirectory(prefix="rmp-gate-")
        self.root = Path(self._tmp.name)
        for rel in tracked(*paths):
            dest = self.root / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(REPO / rel, dest)
        for rel in links:
            (self.root / rel).parent.mkdir(parents=True, exist_ok=True)
            (self.root / rel).symlink_to(REPO / rel)
        # git of its own: no global config, and no personal ignore file (git
        # reads $XDG_CONFIG_HOME/git/ignore with no config at all).
        self.env = dict(os.environ, GIT_CONFIG_GLOBAL=str(self.root / ".gitconfig-none"),
                        XDG_CONFIG_HOME=str(self.root / ".xdg"),
                        GIT_CONFIG_NOSYSTEM="1", GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                        GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        if git:
            self.git("init", "-q", "-b", "main")
            self.commit()

    def close(self):
        self._tmp.cleanup()

    def git(self, *argv) -> str:
        return subprocess.run(["git", *argv], cwd=self.root, env=self.env, check=True,
                              capture_output=True, text=True).stdout

    def commit(self):
        self.git("add", "-A", "-f")
        self.git("commit", "-q", "--allow-empty", "-m", "scratch")

    def plant(self, rel: str, text: str) -> Path:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def edit(self, rel: str, old: str, new: str, count: int = 1):
        """`old` replaced in `rel`: the first `count` times, or every time with -1."""
        path = self.root / rel
        text = path.read_text(encoding="utf-8")
        if old not in text:
            raise AssertionError(f"{rel} no longer holds {old!r}: the plant needs its new form")
        path.write_text(text.replace(old, new, count), encoding="utf-8")

    def run(self, script: str, *args, env=None, shell="bash"):
        got = subprocess.run([shell, f"tools/{script}", *args], cwd=self.root,
                             env=dict(self.env, **(env or {})), stdin=subprocess.DEVNULL,
                             capture_output=True, text=True)
        return got.returncode, got.stdout + got.stderr


class GateTest(unittest.TestCase):
    """A class per gate: SCRIPT, the scratch it reads (PATHS, LINKS, GIT), and
    `check()` to run it there. Every one has test_the_tree_as_it_is_passes
    and at least one test_planted_*: EveryGateIsSeenRedTest says so."""

    SCRIPT = ""
    PATHS: tuple = ()
    LINKS: tuple = ()
    GIT = False

    def setUp(self):
        if shutil.which("bash") is None:
            self.skipTest("the gates are bash")
        self.tree = Scratch(f"tools/{self.SCRIPT}", "tools/cpp_text.py", *self.PATHS,
                            links=self.LINKS, git=self.GIT)
        self.addCleanup(self.tree.close)

    def check(self, *args, env=None):
        return self.tree.run(self.SCRIPT, *args, env=env)

    def assert_green(self, *args, env=None):
        code, out = self.check(*args, env=env)
        self.assertEqual(code, 0, out)
        self.assertNotIn("Traceback", out)
        return out

    def assert_red(self, *says, args=(), env=None):
        code, out = self.check(*args, env=env)
        self.assertEqual(code, 1, out)
        self.assertNotIn("Traceback", out)
        for said in says:
            self.assertIn(said, out)
        return out


# ---------------------------------------------------------------------------
# The index
# ---------------------------------------------------------------------------

class RepoCheckTest(GateTest):
    SCRIPT = "repo_check.sh"
    PATHS = (".gitignore", "README.md")
    GIT = True

    def setUp(self):
        if shutil.which("git") is None:
            self.skipTest("repo_check.sh reads git's index")
        super().setUp()

    def test_the_tree_as_it_is_passes(self):
        self.assert_green()

    def test_planted_a_cmake_cache(self):
        self.tree.plant("build-memory/CMakeCache.txt", "CMAKE_HOME_DIRECTORY:INTERNAL=/x\n")
        self.tree.commit()
        self.assert_red("pattern (^|/)CMakeCache\\.txt$", "build-memory/CMakeCache.txt")

    def test_planted_an_object_file_and_a_log(self):
        self.tree.plant("out/main.o", "x")
        self.tree.plant("tools/run.log", "x")
        self.tree.commit()
        self.assert_red("out/main.o", "tools/run.log")

    def test_planted_an_object_file_whose_name_git_quotes(self):
        """A hole, found planting: `git ls-files` quotes a path with a byte
        that is not ASCII -- "out/m\\303\\263dulo.o", closing quote and all --
        and no pattern ending in `$` matched it. Read with -z, unquoted. With
        no .gitignore to say it, the patterns are all there is."""
        self.tree.plant(".gitignore", "")
        self.tree.plant("out/m\u00f3dulo.o", "x")
        self.tree.plant("out/tab\there.so", "x")
        self.tree.commit()
        self.assert_red("out/m\u00f3dulo.o", "out/tab\there.so")

    def test_a_personal_ignore_file_is_not_the_repositorys(self):
        """A hole the other way, found planting: the second half asked git
        with --exclude-standard, which reads the person's own ignore file
        ($XDG_CONFIG_HOME/git/ignore, core.excludesFile) and .git/info/exclude
        too -- so a tracked README.md was red on the machine of somebody who
        ignores README.md everywhere, and green in CI. It is the repository's
        .gitignore the rule is about."""
        self.tree.plant(".xdg/git/ignore", "README.md\n")
        self.tree.plant(".git/info/exclude", "README.md\n")
        self.assert_green()

    def test_planted_a_file_the_gitignore_ignores(self):
        self.tree.plant("notes.tmp", "x")
        self.tree.commit()
        self.tree.plant(".gitignore", (self.tree.root / ".gitignore").read_text() + "*.tmp\n")
        self.tree.commit()
        self.assert_red("tracked AND matched by .gitignore", "notes.tmp")


# ---------------------------------------------------------------------------
# The text gates: what clang-tidy cannot see, over every #if branch
# ---------------------------------------------------------------------------

CODE = ("include", "src", "tests", "examples")
THIRDPARTY = ("thirdparty",)


class SeamCheckTest(GateTest):
    SCRIPT = "seam_check.sh"
    PATHS = ("src/rmp", "include/rmp")

    def test_the_tree_as_it_is_passes(self):
        self.assert_green()

    def test_planted_a_clock_read_in_a_new_file(self):
        self.tree.plant("src/rmp/later.cpp",
                        "#include <raylib.h>\nnamespace rmp {\n"
                        "float later() { return GetFrameTime(); }\n}\n")
        self.assert_red("src/rmp/later.cpp reads time, input, randomness or the screen directly",
                        "3:float later() { return GetFrameTime(); }")

    def test_planted_a_call_after_a_string_with_two_slashes(self):
        """A hole, found planting: comments were stripped with sed's `//.*`, so
        a URL in a string took the rest of its line -- and the call after it
        -- with it. The scanner every other text gate reads with
        (tools/cpp_text.py) knows a string from a comment."""
        self.tree.plant("src/rmp/later.cpp",
                        '#include <raylib.h>\nnamespace rmp {\n'
                        'float later() { const char *where = "https://x"; return GetTime(); }\n'
                        '}\n')
        self.assert_red("src/rmp/later.cpp reads time, input, randomness or the screen directly")

    def test_planted_a_call_in_a_public_header(self):
        self.tree.plant("include/rmp/later.h",
                        "#pragma once\n#include <raylib.h>\n"
                        "inline bool held() { return IsKeyDown(KEY_A); }\n")
        self.assert_red("include/rmp/later.h reads time")

    def test_a_call_in_a_comment_or_a_string_is_not_one(self):
        self.tree.plant("src/rmp/later.cpp",
                        "// GetFrameTime() is clock.cpp's alone\n"
                        "/* and GetTime()\n * is too */\n"
                        'const char *said = "IsKeyDown(KEY_A)";\n')
        self.assert_green()

    def test_planted_a_shadowed_raylib_type(self):
        self.tree.plant("src/rmp/later.cpp", "namespace rmp {\nImage pack_read_image();\n}\n")
        self.assert_red("a shadowed raylib type is written without ::")

    def test_planted_a_tree_that_moved_away(self):
        shutil.rmtree(self.tree.root / "src" / "rmp")
        shutil.rmtree(self.tree.root / "include" / "rmp")
        self.assert_red("fewer than 10 sources found")


class OwnershipCheckTreeTest(GateTest):
    SCRIPT = "ownership_check.sh"
    PATHS = CODE
    LINKS = THIRDPARTY

    def test_the_tree_as_it_is_passes(self):
        self.assert_green()

    def test_planted_new_and_free_in_the_framework(self):
        self.tree.plant("src/rmp/later.cpp", "void f() { int *p = new int(3); delete p; }\n")
        self.tree.plant("include/rmp/later.h", "inline void g(void *p) { free(p); }\n")
        self.assert_red("src/rmp/later.cpp:1: O1 new", "src/rmp/later.cpp:1: O1 delete",
                        "include/rmp/later.h:1: O1 free")

    def test_planted_a_raylib_load_in_an_example(self):
        self.tree.plant("examples/games/01_pong/src/later.cpp",
                        'void f() { Texture2D t = LoadTexture("x.png"); UnloadTexture(t); }\n')
        self.assert_red("examples/games/01_pong/src/later.cpp:1: O2 LoadTexture")

    def test_what_the_tree_mode_leaves_alone(self):
        """An example owns its memory as it likes (O1 is the framework's), C
        has no destructor to put an Unload in, a vendored library compiled in
        an _impl file is its own business, and the owners keep their calls."""
        self.tree.plant("examples/games/01_pong/src/later.cpp", "int *p = new int(3);\n")
        self.tree.plant("examples/plain_c/src/later.c",
                        'void f(void) { Texture2D t = LoadTexture("x"); UnloadTexture(t); }\n')
        self.tree.plant("src/rmp/later_impl.cpp", "void f() { free(malloc(4)); }\n")
        self.tree.edit("src/rmp/resource.cpp", "namespace rmp", "void f() { LoadImage(\"x\"); }\n"
                       "namespace rmp")
        self.assert_green()


class NamingCheckTreeTest(GateTest):
    SCRIPT = "naming_check.sh"
    PATHS = CODE
    LINKS = THIRDPARTY

    def test_the_tree_as_it_is_passes(self):
        self.assert_green()

    def test_planted_a_name_of_each_kind_in_each_root(self):
        self.tree.plant("tests/later_test.cpp", "static int g_count = 0;\n")
        self.tree.plant("examples/games/01_pong/src/later.cpp", "constexpr int kSpeed = 3;\n")
        self.tree.plant("src/rmp/later.cpp", "#define LATER 1\nconstexpr float PI = 3.f;\n")
        self.tree.plant("include/rmp/later.h", "#if APP_SAVE_PORTABLE\n#endif\n")
        self.assert_red("tests/later_test.cpp:1: R1 g_count",
                        "examples/games/01_pong/src/later.cpp:1: R2 kSpeed",
                        "src/rmp/later.cpp:1: R5 #define LATER",
                        "src/rmp/later.cpp:2: R6 PI is a macro elsewhere",
                        "include/rmp/later.h:1: R7 APP_SAVE_PORTABLE")

    def test_planted_a_vendored_knob_outside_the_file_that_configures_it(self):
        """src/rmp/tiled_impl.cpp defines it, and the tree passes; the same
        line anywhere else is a macro without RMP_."""
        self.assertIn("#define CUTE_TILED_IMPLEMENTATION",
                      (self.tree.root / "src" / "rmp" / "tiled_impl.cpp").read_text())
        self.tree.plant("src/rmp/later.cpp", "#define CUTE_TILED_IMPLEMENTATION\n")
        self.assert_red("src/rmp/later.cpp:1: R5 #define CUTE_TILED_IMPLEMENTATION")

    def test_what_the_tree_leaves_out(self):
        """tests/fixtures/ holds third-party-shaped files on purpose."""
        self.tree.plant("tests/fixtures/later/later.cpp", "static int g_count = 0;\n")
        self.assert_green()


class StyleCheckTreeTest(GateTest):
    SCRIPT = "style_check.sh"
    PATHS = CODE

    def test_the_tree_as_it_is_passes(self):
        self.assert_green()

    def test_planted_a_rule_of_each_kind(self):
        self.tree.plant("src/rmp/later.cpp",
                        "#include <format>\n"
                        "int f(double d) { return (int)d; }\n"
                        "void *g() { return NULL; }\n"
                        "void h(int i) { i++; }\n")
        self.tree.plant("examples/games/01_pong/src/later.cpp",
                        "// TODO: later\nusing namespace std::literals;\n")
        self.assert_red("src/rmp/later.cpp:1: S4 #include <format>",
                        "src/rmp/later.cpp:2: S1 (int)", "src/rmp/later.cpp:3: S2 NULL",
                        "src/rmp/later.cpp:4: S6 i++",
                        "examples/games/01_pong/src/later.cpp:1: S3 TODO",
                        "examples/games/01_pong/src/later.cpp:2: S5")

    def test_what_the_tree_mode_exempts(self):
        """C is exempt from the cast, NULL and postfix rules; tests/ from the
        floor compiler's, built on the image's compilers only."""
        self.tree.plant("examples/plain_c/src/later.c",
                        "int f(double d) { int i = (int)d; i++; return NULL == 0; }\n")
        self.tree.plant("tests/later_test.cpp", "#include <format>\n")
        self.assert_green()


class PortableCheckTest(GateTest):
    SCRIPT = "portable_check.sh"
    PATHS = ("tools", "rmp", ".github/workflows/_bsd.yml", ".github/workflows/_apple.yml")

    def test_the_tree_as_it_is_passes(self):
        self.assert_green()

    def test_planted_a_bash_4_builtin_and_gnu_flags(self):
        self.tree.plant("tools/later.sh", "#!/usr/bin/env bash\nmapfile -t lines < x\n"
                                          "n=$(nproc)\nsed -i 's/a/b/' f\n")
        self.assert_red("FAIL  mapfile/readarray is bash 4", "FAIL  nproc is GNU coreutils",
                        "FAIL  sed -i needs an empty argument", "tools/later.sh:2:")

    def test_planted_in_the_bsd_workflow(self):
        """Its cpa.sh block runs on a BSD, and a byte that is not ASCII does
        not survive the trip into the VM."""
        self.tree.edit(".github/workflows/_bsd.yml", "shell: cpa.sh",
                       "shell: cpa.sh\n          # two hyphens, not \u2014 this")
        self.assert_red("a non-ASCII character in the cpa.sh block")

    def test_planted_a_script_that_is_not_there(self):
        self.tree.plant("tools/later.sh", "#!/bin/sh\nsh tools/never_written.sh\n")
        self.assert_red("a script that does not exist is named", "tools/never_written.sh")

    def test_a_comment_that_names_a_construct_is_not_one(self):
        self.tree.plant("tools/later.sh", "#!/bin/sh\n# mapfile is bash 4, so not here\n")
        self.assert_green()


class ExamplesCheckTest(GateTest):
    SCRIPT = "examples_check.sh"
    PATHS = ("examples",)

    def test_the_tree_as_it_is_passes(self):
        self.assert_green()

    def test_planted_a_main_and_a_loop_in_an_example(self):
        self.tree.plant("examples/games/01_pong/src/later.cpp",
                        "int main() { while (!WindowShouldClose()) {} }\n"
                        "void on_frame() {}\n")
        self.assert_red("writes its own int main()", "drives its own frame loop",
                        "declares on_frame() with no delta")

    def test_planted_a_second_plain_c_example(self):
        """A hole, found planting: only *.cpp was read, so a second C file --
        the language whose example is the one opt-out -- taught the loop that
        does not work on the web or iOS, unread."""
        self.tree.plant("examples/plain_c/src/later.c",
                        "int main(void) { while (!WindowShouldClose()) {} return 0; }\n")
        self.assert_red("examples/plain_c/src/later.c writes its own int main()",
                        "examples/plain_c/src/later.c drives its own frame loop")

    def test_planted_a_loop_in_an_examples_header(self):
        """The same hole: a header an example includes teaches what it holds."""
        self.tree.plant("examples/games/01_pong/include/later.h",
                        "#pragma once\ninline void loop() { while (!WindowShouldClose()) {} }\n")
        self.assert_red("examples/games/01_pong/include/later.h drives its own frame loop")

    def test_planted_an_example_nobody_links_and_a_link_to_nothing(self):
        self.tree.plant("examples/games/99_later/src/main.cpp", "RMP_GAME(Later);\n")
        readme = self.tree.root / "examples" / "README.md"
        readme.write_text(readme.read_text() + "\n| [gone](games/98_gone/) | gone |\n")
        self.assert_red("examples/games/99_later has no link in examples/README.md",
                        "points at games/98_gone/, which does not exist")

    def test_planted_an_opt_out_that_stopped_saying_its_cost(self):
        self.tree.edit("examples/plain_c/src/main.c", "ASYNCIFY", "the browser", count=-1)
        self.assert_red("no longer mentions ASYNCIFY")


# ---------------------------------------------------------------------------
# Headers: on their own, and what they cost
# ---------------------------------------------------------------------------

# Everything configure.py reads to write include/rmp/generated/config.h,
# which every public header includes.
CONFIGURABLE = ("raylib_multiplatform.toml", "LICENSE", "tools/configure.py",
                "tools/license_db.py",
                "CMakePresets.json", "raymob", "ios", "branding", ".github", "cmake",
                "THIRD_PARTY_LICENSES.md", "CMakeLists.txt", "resources", "src", "include")


class HeaderCheckTest(GateTest):
    SCRIPT = "header_check.sh"
    PATHS = CONFIGURABLE
    LINKS = THIRDPARTY

    def setUp(self):
        if shutil.which(os.environ.get("CXX", "g++")) is None:
            self.skipTest("header_check.sh compiles with $CXX, g++ by default")
        super().setUp()

    def test_the_tree_as_it_is_passes(self):
        out = self.assert_green()
        self.assertRegex(out, r"ok    \d+ public headers, each self-contained")

    def test_planted_a_header_that_is_not_self_contained_and_one_without_the_config(self):
        self.tree.plant("include/rmp/later.h",
                        "#pragma once\n#include <rmp/config.h>\n"
                        "inline std::vector<int> later() { return {}; }\n")
        self.tree.plant("include/rmp/sooner.h", "#pragma once\ninline int sooner() { return 1; }\n")
        self.assert_red("FAIL  rmp/later.h", "FAIL  rmp/sooner.h", "FAIL: 2 header(s)")


class HeaderCostCheckTest(GateTest):
    """header_cost_check.sh on a tree of two public headers, so that a clean
    run measures two and not every one (five compiles a header)."""

    SCRIPT = "header_cost_check.sh"
    PATHS = ("tools/header_cost.py", "tools/header_budget.txt") + CONFIGURABLE
    LINKS = THIRDPARTY
    KEEP = ("random.h", "math.h")

    def setUp(self):
        if shutil.which("g++") is None and shutil.which("clang++") is None:
            self.skipTest("header_cost.py compiles each header")
        super().setUp()
        got = subprocess.run([sys.executable, "tools/configure.py"], cwd=self.tree.root,
                             capture_output=True, text=True, stdin=subprocess.DEVNULL)
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        for header in (self.tree.root / "include" / "rmp").glob("*.h"):
            if header.name not in self.KEEP + ("config.h",):
                header.unlink()
        budget = self.tree.root / "tools" / "header_budget.txt"
        lines = budget.read_text().splitlines()
        budget.write_text("\n".join(line for line in lines if line.startswith("#")
                                    or line.split()[:1] and line.split()[0] in self.KEEP)
                          + "\n")

    def budget(self) -> Path:
        return self.tree.root / "tools" / "header_budget.txt"

    def test_the_tree_as_it_is_passes(self):
        out = self.assert_green()
        self.assertIn("PASS: 2 headers measured", out)

    def test_planted_a_header_with_no_budget(self):
        shutil.copy(REPO / "include" / "rmp" / "save.h", self.tree.root / "include" / "rmp")
        self.assert_red("FAIL  rmp/save.h", "NO BUDGET: add it to tools/header_budget.txt")

    def test_planted_a_budget_for_a_header_that_is_gone(self):
        self.budget().write_text(self.budget().read_text() + "gone.h           10\n")
        out = self.assert_red("is in tools/header_budget.txt but not in include/rmp/")
        self.assertRegex(out, r"FAIL  rmp/gone\.h +is in")

    def test_planted_a_header_that_does_not_compile(self):
        """A header that does not compile with this script's include list --
        a vendored folder added to CMake's and not to INCLUDES -- was a
        CalledProcessError traceback, and what the compiler said was not in
        it. It is a FAIL naming the header, with the compiler's words."""
        self.tree.plant("include/rmp/random.h",
                        "#pragma once\n#include <rmp/config.h>\n#include <never_vendored.h>\n")
        out = self.assert_red("FAIL  rmp/random.h       does not compile", "never_vendored.h")
        self.assertIn("ok    rmp/math.h", out)

    def test_planted_a_note_after_a_number(self):
        """Found by tests/header_cost_property_test.py: a budget line with a
        note after its number -- how a hand edit says why -- was a ValueError
        out of read_budget(), and `rmp test cost` a traceback. It names the
        line, before a single header is compiled."""
        lines = self.budget().read_text().splitlines()
        self.budget().write_text("".join(
            line + ("  # raised for xoshiro" if line.startswith("random.h") else "") + "\n"
            for line in lines))
        out = self.assert_red("FAIL: tools/header_budget.txt:")
        self.assertIn("is not `<header> <lines>`", out)
        self.assertNotIn("rmp/random.h", out.split("FAIL: tools/header_budget.txt:")[0])


# ---------------------------------------------------------------------------
# The workflows
# ---------------------------------------------------------------------------

CALLER = """\
on: push
{workflow_permissions}jobs:
  build:
{permissions}    uses: ./.github/workflows/_x.yml
"""
CALLEE = """\
on:
  workflow_call:{inputs}
{permissions}jobs:
  x:
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
"""


class WorkflowCheckTest(GateTest):
    """workflow_check.sh [root] reads the workflows of `root`: a caller and a
    reusable workflow it calls, written here."""

    SCRIPT = "workflow_check.sh"
    PATHS = (".github/workflows",)

    def setUp(self):
        try:
            import yaml  # noqa: F401
        except ImportError:
            if IN_BUILD_IMAGE:
                self.fail("PyYAML is missing inside the build image")
            self.skipTest("workflow_check.sh reads YAML, and PyYAML is not installed")
        super().setUp()

    def pair(self, caller_permissions="", callee_permissions="", inputs="",
             workflow_permissions=""):
        fixture = self.tree.root / "fixture"
        flows = fixture / ".github" / "workflows"
        flows.mkdir(parents=True, exist_ok=True)
        (flows / "ci.yml").write_text(CALLER.format(permissions=caller_permissions,
                                                    workflow_permissions=workflow_permissions))
        (flows / "_x.yml").write_text(CALLEE.format(permissions=callee_permissions,
                                                    inputs=inputs))
        return str(fixture)

    def test_the_tree_as_it_is_passes(self):
        self.assert_green()

    def test_planted_a_permission_the_caller_does_not_grant(self):
        self.assert_red("asks for ['packages: read'] and is not granted it", args=[self.pair(
            "    permissions:\n      contents: read\n",
            "permissions:\n  contents: read\n  packages: read\n")])

    def test_planted_write_where_the_caller_grants_read(self):
        """A hole, found planting: the check compared the names of the scopes
        and not their levels. GitHub refuses a called workflow that asks for
        more than its caller allows -- `packages: write` under `packages:
        read` -- with a run of zero jobs and no log, which is what this
        check exists to catch first."""
        self.assert_red("asks for ['packages: write'] and is not granted it", args=[self.pair(
            "    permissions:\n      contents: read\n      packages: read\n",
            "permissions:\n  contents: read\n  packages: write\n")])
        self.assert_green(self.pair(
            "    permissions:\n      contents: read\n      packages: write\n",
            "permissions:\n  contents: read\n  packages: read\n"))

    def test_planted_write_all_and_read_all(self):
        """The two strings `permissions:` can be instead of a map: a
        traceback (`str` has no keys) before. write-all asks for everything,
        read-all grants reading everything."""
        self.assert_red("asks for ['write-all'] and is not granted it", args=[self.pair(
            "    permissions: read-all\n", "permissions: write-all\n")])
        self.assert_green(self.pair("    permissions: write-all\n",
                                    "permissions:\n  packages: write\n  contents: read\n"))
        self.assert_green(self.pair("    permissions: read-all\n",
                                    "permissions:\n  packages: read\n"))

    def test_planted_a_scope_a_job_s_map_leaves_out(self):
        """A job that says `permissions:` grants nothing it does not name --
        only metadata is read whatever the map says. contents was taken as
        granted everywhere, as it is when nothing is declared at all, so a
        callee that reads the repository under a caller that names only
        packages passed; GitHub starts no job of that run."""
        self.assert_red("asks for ['contents: read'] and is not granted it", args=[self.pair(
            "    permissions:\n      packages: read\n",
            "permissions:\n  contents: read\n  packages: read\n")])
        self.assert_green(self.pair(
            "", "permissions:\n  contents: read\n  metadata: read\n"))

    def test_planted_a_workflow_that_grants_less_than_its_job_is_asked(self):
        """A job that says nothing runs with its workflow's permissions, not
        the repository's default: `packages: write` asked under a workflow
        that grants `packages: read` is refused, and granted under one that
        grants write."""
        self.assert_red("asks for ['packages: write'] and is not granted it", args=[self.pair(
            "", "permissions:\n  packages: write\n",
            workflow_permissions="permissions:\n  contents: read\n  packages: read\n")])
        self.assert_green(self.pair(
            "", "permissions:\n  contents: read\n  packages: write\n",
            workflow_permissions="permissions:\n  contents: read\n  packages: write\n"))
        # The job's own map, when it has one, and not the workflow's.
        self.assert_red("asks for ['packages: write'] and is not granted it", args=[self.pair(
            "    permissions:\n      contents: read\n      packages: read\n",
            "permissions:\n  contents: read\n  packages: write\n",
            workflow_permissions="permissions: write-all\n")])

    def test_a_reusable_workflow_with_nothing_under_workflow_call(self):
        """`on: workflow_call:` and no inputs is a reusable workflow GitHub
        runs; the check was a traceback on it (None has no .get)."""
        self.assert_green(self.pair())


# ---------------------------------------------------------------------------
# What a binary needs
# ---------------------------------------------------------------------------

class GlibcCheckTest(GateTest):
    """glibc_check.sh BINARY FLOOR on programs built here with the system's
    compiler: a dynamic one needs the glibc it was linked against."""

    SCRIPT = "glibc_check.sh"
    PATHS = ("tools/configure.py", "raylib_multiplatform.toml")

    def setUp(self):
        if sys.platform != "linux" or shutil.which("gcc") is None \
                or shutil.which("objdump") is None:
            self.skipTest("a glibc program, built with gcc and read with objdump")
        super().setUp()
        source = self.tree.plant("s.c", "int main(void) { return 0; }\n")
        self.dynamic = self.tree.root / "dynamic"
        subprocess.run(["gcc", "-o", str(self.dynamic), str(source)], check=True)

    def test_the_tree_as_it_is_passes(self):
        out = self.assert_green(str(self.dynamic), "99.0")
        self.assertRegex(out, r"ok    needs glibc 2\.\d+, floor is 99\.0")

    def test_planted_a_floor_older_than_the_build(self):
        self.assert_red("needs glibc 2.", "and the floor this target is held to is 2.2",
                        "__libc_start_main", args=[str(self.dynamic), "2.2"])

    def test_report_never_fails(self):
        out = self.assert_green(str(self.dynamic), "report")
        self.assertIn("info  this binary needs glibc 2.", out)

    def test_planted_a_file_that_is_no_program(self):
        """A hole, found planting: objdump failing on a file it cannot read
        left no versions, and that was "ok, static?" -- of a zip, a packed
        binary, a download cut short. A static program says it is not a
        dynamic object; anything else is a FAIL that says what objdump said."""
        notelf = self.tree.plant("notelf", "this is no program at all\n")
        cut = self.tree.root / "cut"
        cut.write_bytes(self.dynamic.read_bytes()[:200])
        for what in (notelf, cut):
            with self.subTest(what=what.name):
                self.assert_red(f"objdump cannot read {what} as a program",
                                args=[str(what), "2.28"])

    def test_a_static_program_needs_nothing(self):
        source = self.tree.root / "s.c"
        static = self.tree.root / "static"
        got = subprocess.run(["gcc", "-static", "-o", str(static), str(source)],
                             capture_output=True)
        if got.returncode != 0:
            self.skipTest("no static libc here")
        out = self.assert_green(str(static), "2.28")
        self.assertIn("is statically linked", out)
        # Whatever language the machine speaks: objdump's "not a dynamic
        # object" is translated (binutils ships es, de, fr...), and on a
        # Spanish desktop -- "no es un objeto dinamico" -- a static program
        # was a FAIL. Proved where the translation is installed; elsewhere
        # objdump answers in English whatever it is asked.
        for language in ("es_ES.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8"):
            with self.subTest(language=language):
                out = self.assert_green(str(static), "2.28", env={
                    "LC_ALL": language, "LANG": language, "LANGUAGE": language[:2]})
                self.assertIn("is statically linked", out)

    def test_no_objdump_is_a_failure_in_the_image(self):
        """Without objdump the check cannot read anything: on a laptop that
        is said and skipped, in the image -- where CI runs it -- a FAIL. A
        skip there would be a release job that checked nothing."""
        bin_dir = self.tree.root / "bin"
        bin_dir.mkdir()
        for tool in ("bash", "dirname", "python3", "cat"):
            if shutil.which(tool):
                (bin_dir / tool).symlink_to(shutil.which(tool))
        code, out = self.check(str(self.dynamic), "2.28", env={"PATH": str(bin_dir)})
        if IN_BUILD_IMAGE:
            self.assertEqual(code, 1, out)
            self.assertIn("FAIL: objdump is not installed", out)
        else:
            self.assertEqual(code, 0, out)
            self.assertIn("skip  objdump not installed", out)


# ---------------------------------------------------------------------------
# The two gates that build: their verdict, with a cmake that only pretends
# ---------------------------------------------------------------------------

# A cmake that configures nothing and "builds" a game that does what
# STUB_GAME says: from the project folder (`here`) or from another
# (`elsewhere`), print the markers or not, and exit with STUB_EXIT.
STUB_CMAKE = textwrap.dedent("""\
    #!/bin/sh
    case "$1" in
      --build)
        dir="$2"; shift 2
        while [ $# -gt 0 ]; do
          if [ "$1" = "--target" ]; then shift; for t in "$@"; do
            mkdir -p "$dir"
            cat > "$dir/$t" <<'GAME'
    #!/bin/sh
    case "$(pwd)" in */elsewhere) where=elsewhere ;; *) where=here ;; esac
    case " $STUB_GAME " in *" boot-$where "*)
        echo "RAY_TEST_BOOT_OK assets_failed=0 frames=5" ;; esac
    case " $STUB_GAME " in *" done "*) echo "RAY_TEST_DONE_FRAMES 5" ;; esac
    case " $STUB_GAME " in *" ui-pass "*) echo "PASS" ;; esac
    case " $STUB_GAME " in *" doctest-pass "*)
        echo "[doctest] test cases: 1 | 1 passed"; echo "[doctest] Status: SUCCESS!" ;; esac
    exit "${STUB_EXIT:-0}"
    GAME
            chmod +x "$dir/$t"
          done; break; fi
          shift
        done ;;
      -S | -B)
        echo "-- configured"
        case " $STUB_GAME " in *" sanitized "*) echo "=== SANITIZERS: address, undefined ===" ;; esac ;;
    esac
    exit 0
    """)


class StubbedBuildTest(GateTest):
    def setUp(self):
        if shutil.which("sh") is None:
            self.skipTest("POSIX sh")
        super().setUp()
        self.bin = self.tree.root / "stubbin"
        self.bin.mkdir()
        for name in ("cmake",):
            (self.bin / name).write_text(STUB_CMAKE)
            (self.bin / name).chmod(0o755)
        for name in ("gcc", "g++"):
            (self.bin / name).write_text("#!/bin/sh\necho 'g++ (stub) 13.3.0'\n")
            (self.bin / name).chmod(0o755)

    def stubbed(self, game, status=0):
        return {"PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}", "STUB_GAME": game,
                "STUB_EXIT": str(status)}


class ShippedCheckTest(StubbedBuildTest):
    """shipped_check.sh: a release started where it was built and from
    another folder. A game in plain C here (no src/rmp/), so it ships loose
    resources/ and no pack has to be made."""

    SCRIPT = "shipped_check.sh"
    PATHS = ("tools/ship_resources.sh",)
    GOOD = "boot-here boot-elsewhere done"

    def setUp(self):
        super().setUp()
        self.tree.plant("resources/later.png", "x")

    def shipped(self, game, status=0):
        return self.tree.run(self.SCRIPT, "Ninja", "", "game", shell="sh",
                             env=self.stubbed(game, status))

    def test_the_tree_as_it_is_passes(self):
        code, out = self.shipped(self.GOOD)
        self.assertEqual(code, 0, out)
        self.assertIn("PASS: the release found its resources/ from another folder", out)

    def test_planted_a_release_that_finds_nothing_from_elsewhere(self):
        code, out = self.shipped("boot-here done")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: started from another folder, the release did not boot", out)

    def test_planted_a_release_that_moves_away_from_the_project_folder(self):
        code, out = self.shipped("boot-elsewhere done")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: started from the project folder", out)

    def test_planted_a_release_that_dies_before_its_last_frame(self):
        code, out = self.shipped("boot-here boot-elsewhere")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: it died before the end of its frame budget", out)

    def test_planted_an_exit_status_after_every_marker(self):
        code, out = self.shipped(self.GOOD, status=3)
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: the release exited with status 3", out)


class SanitizeCheckTest(StubbedBuildTest):
    """sanitize_check.sh's verdict on what the build and the two test
    programs say."""

    SCRIPT = "sanitize_check.sh"
    GOOD = "sanitized doctest-pass ui-pass"

    def sanitized(self, game, status=0):
        return self.check(env=self.stubbed(game, status))

    def test_the_tree_as_it_is_passes(self):
        code, out = self.sanitized(self.GOOD)
        self.assertEqual(code, 0, out)
        self.assertIn("PASS: no AddressSanitizer or UndefinedBehaviorSanitizer report", out)

    def test_planted_a_build_that_is_not_instrumented(self):
        code, out = self.sanitized("doctest-pass ui-pass")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: the build is not instrumented with address and undefined", out)

    def test_planted_tests_that_exit_0_and_never_said_success(self):
        code, out = self.sanitized("sanitized ui-pass")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: the unit tests are not clean under ASan and UBSan (exit 0)", out)

    def test_planted_a_layout_test_that_says_nothing(self):
        code, out = self.sanitized("sanitized doctest-pass")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL: the UI layout test is not clean", out)

    def test_planted_a_report_and_its_exit_status(self):
        code, out = self.sanitized(self.GOOD, status=1)
        self.assertEqual(code, 1, out)
        self.assertIn("(exit 1)", out)


# ---------------------------------------------------------------------------
# The Python gates' counterexamples
# ---------------------------------------------------------------------------

def load(name: str):
    spec = importlib.util.spec_from_file_location(f"tools.{name}", TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class AndroidReleaseCheckCounterexamplesTest(unittest.TestCase):
    """Found by tests/android_release_check_property_test.py: a compile
    database cut short (a build stopped halfway), of another shape, or with an
    entry whose fields are not what the format says, was a traceback in the
    Android job. Each is a FAIL naming the database, exit 1."""

    def check(self, content):
        arc = load("android_release_check")
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "RelWithDebInfo" / "h" / "arm64-v8a" / "compile_commands.json"
            db.parent.mkdir(parents=True)
            if isinstance(content, bytes):
                db.write_bytes(content)
            else:
                db.write_text(content)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = arc.main(["android_release_check.py", tmp])
        return code, out.getvalue()

    def test_each_database_it_cannot_read(self):
        whole = json.dumps([{"directory": "/b", "file": "/w/src/main.cpp",
                             "command": "clang++ -DRMP_PRODUCTION_BUILD=1 -O2 -c x.cpp"}])
        cases = {
            "cut short": (whole[:40], "not a compile database this reads"),
            "empty": ("", "not a compile database this reads"),
            "not UTF-8": (b"\x80[]", "not a compile database this reads"),
            "an object": ('{"file": "/w/src/main.cpp"}', "not a list of compile commands"),
            "a list of numbers": ("[0]", "not a list of compile commands"),
            "a file that is no path": ('[{"file": 3, "command": "cc"}]', "an entry whose file is 3, not a path"),
            "arguments that are no list": ('[{"file": "/w/src/a.cpp", "arguments": "-O2"}]',
                                           "an entry whose arguments are '-O2'"),
            "a command that does not split": ('[{"file": "/w/src/a.cpp", "command": "cc \\"x"}]',
                                              "an entry whose command does not split into words"),
        }
        for what, (content, says) in cases.items():
            with self.subTest(what=what):
                code, out = self.check(content)
                self.assertEqual(code, 1, out)
                self.assertIn("compile_commands.json: " + says, out)
                self.assertIn("FAIL: 1 release compile database(s) could not be read", out)


class HeaderCostCounterexamplesTest(unittest.TestCase):
    """Found by tests/header_cost_property_test.py, on the budget file: each
    line a person can type that is not `<header> <lines>` names its line."""

    def test_each_line_that_is_not_a_budget(self):
        hc = load("header_cost")
        for text, line in (("app.h 12 # raised", 1), ("# note\napp.h twelve", 2),
                           ("app.h 1_000", 1), ("app.h -5", 1), ("app.h \u0663", 1),
                           ("app.h", 1), ("app.h 1\n\napp.h 2", 3)):
            with self.subTest(text=text):
                with self.assertRaisesRegex(hc.BudgetError,
                                            rf"^tools/header_budget\.txt:{line}: "):
                    hc.parse_budget(text)
        self.assertEqual(hc.parse_budget("# note\n\n  app.h   12  \nui.h 0\n"),
                         {"app.h": 12, "ui.h": 0})


# ---------------------------------------------------------------------------
# The list itself
# ---------------------------------------------------------------------------

class EveryGateIsSeenRedTest(unittest.TestCase):
    """GATES is every tools/*_check.sh, each seen red: here, by a class with
    a clean run and a planted one, or by the class elsewhere it names."""

    def here(self) -> dict[str, type]:
        found = {}
        for value in globals().values():
            if isinstance(value, type) and issubclass(value, GateTest) and value.SCRIPT:
                found[value.SCRIPT] = value
        return found

    def test_the_list_is_the_folder(self):
        scripts = sorted(p.name for p in TOOLS.glob("*_check.sh"))
        self.assertEqual(sorted(GATES), scripts,
                         "a tools/*_check.sh with no line in GATES, or a line for one gone")

    def test_here_means_a_class_with_both_runs(self):
        classes = self.here()
        for script, where in GATES.items():
            with self.subTest(script=script):
                if where != "here":
                    self.assertNotIn(script, classes, f"{script} is planted here: say `here`")
                    continue
                self.assertIn(script, classes, f"GATES says {script} is planted here")
                tests = [n for n in dir(classes[script]) if n.startswith("test_")]
                self.assertIn("test_the_tree_as_it_is_passes", tests)
                self.assertTrue(any(n.startswith("test_planted_") for n in tests),
                                f"{classes[script].__name__} plants nothing")

    def test_elsewhere_names_a_class_that_exists(self):
        for script, where in GATES.items():
            if where == "here":
                continue
            with self.subTest(script=script):
                m = re.match(r"([\w.]+_test\.py)::(\w+): \S", where)
                self.assertIsNotNone(m, f"{script}: `<file>::<Class>: why there`, not {where!r}")
                text = (REPO / "tests" / m.group(1)).read_text(encoding="utf-8")
                self.assertRegex(text, rf"\nclass {m.group(2)}\(")
                self.assertIn(script.split(".")[0], text)


if __name__ == "__main__":
    unittest.main()
