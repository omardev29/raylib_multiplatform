"""Tests for the `rmp` command: tools/rmp.py and its three launchers.

Run with:  python3 -m unittest discover -s tests -p rmp_test.py

Most of these run tools/rmp.py's own functions with the one door every command
goes through -- Ctx.run -- replaced by a recorder, so what a command WOULD run is
asserted without a compiler. `push` and `deploy` run against a real git, in a
temporary repository with a bare one as its origin, and a fake `gh` on PATH.
"""

from __future__ import annotations

import ast
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
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RMP_PY = REPO / "tools" / "rmp.py"

spec = importlib.util.spec_from_file_location("rmp_cli", RMP_PY)
rmp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rmp)

IN_BUILD_IMAGE = Path("/etc/raylib-build-image.json").is_file()


def quiet():
    return contextlib.redirect_stdout(io.StringIO())


def call(argv, cwd=REPO):
    """main(), with what it printed."""
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = rmp.main(argv, cwd=Path(cwd))
    return code, out.getvalue(), err.getvalue()


class Recorder:
    """Stands in for Ctx.run: records the argv, answers with what it is told."""

    def __init__(self, answers=None):
        self.calls = []
        self.answers = answers or {}

    def __call__(self, ctx, argv, cwd=None, env=None, check=True, capture=False):
        argv = [str(a) for a in argv]
        self.calls.append((argv, env))
        for needle, (code, out) in self.answers.items():
            if needle in " ".join(argv):
                return subprocess.CompletedProcess(argv, code, out, "")
        return subprocess.CompletedProcess(argv, 0, "", "")


@contextlib.contextmanager
def recording(answers=None):
    rec = Recorder(answers)
    original = rmp.Ctx.run
    rmp.Ctx.run = lambda self, *a, **k: rec(self, *a, **k)
    try:
        yield rec
    finally:
        rmp.Ctx.run = original


@contextlib.contextmanager
def windows(yes=True):
    original = rmp.on_windows
    rmp.on_windows = lambda: yes
    try:
        yield
    finally:
        rmp.on_windows = original


def fake_project(root: Path, framework=False, name="demo"):
    root.mkdir(parents=True, exist_ok=True)
    (root / rmp.TOML).write_text(f'[project]\nname = "{name}"\n')
    (root / "tools").mkdir(exist_ok=True)
    if framework:
        (root / "tests").mkdir(exist_ok=True)
        (root / rmp.FRAMEWORK_MARKER).write_text("")
    return root


class HelpTest(unittest.TestCase):
    """Every command has a page, and every page is true."""

    def test_every_command_has_a_page_and_a_handler(self):
        for name, c in rmp.COMMANDS.items():
            with self.subTest(command=name):
                self.assertTrue(c.summary and c.sentence)
                self.assertTrue(2 <= len(c.examples) <= 4, c.examples)
                self.assertTrue(callable(c.handler))
                self.assertTrue(c.usage.split()[0] == name)

    def test_every_example_is_a_command_line_rmp_accepts(self):
        """A page that shows `rmp tset` would be a page that lies."""
        for name, c in rmp.COMMANDS.items():
            for cmdline, _ in c.examples:
                if not cmdline.startswith("rmp "):
                    continue
                words = cmdline.split()[1:]
                with self.subTest(example=cmdline):
                    self.assertIn(words[0], rmp.COMMANDS)
                    if words[0] == "test" and len(words) > 1:
                        self.assertIn(words[1], [s.name for s in rmp.STAGES])

    def test_help_is_ascii_and_fits_78_columns(self):
        for mode in ("framework", "game"):
            texts = [rmp.usage_text(mode)] + [rmp.page(n, mode) for n in rmp.COMMANDS]
            for text in texts:
                for line in text.splitlines():
                    with self.subTest(mode=mode, line=line):
                        self.assertTrue(line.isascii())
                        self.assertLessEqual(len(line), 78)

    def test_the_four_spellings_of_help_are_one(self):
        outs = {call(argv)[1] for argv in ([], ["-h"], ["--help"], ["help"])}
        self.assertEqual(len(outs), 1)
        self.assertTrue(next(iter(outs)).startswith("usage: rmp"))
        self.assertEqual(call(["-h"])[0], 0)

    def test_a_page_three_ways(self):
        pages = {call(argv)[1] for argv in (["help", "deploy"], ["deploy", "-h"],
                                            ["deploy", "--help"])}
        self.assertEqual(len(pages), 1)
        self.assertIn("rmp deploy 1.2.0", next(iter(pages)))

    def test_maintainer_commands_are_listed_in_the_framework_only(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            _, out, _ = call(["help"], cwd=game)
            self.assertNotIn("lint", out)
            self.assertNotIn("framework only", out)
        _, out, _ = call(["help"])
        self.assertIn("framework only:", out)
        self.assertIn("lint [fix]", out)

    def test_help_test_names_the_stages_of_the_project(self):
        _, out, _ = call(["help", "test"])
        for stage in rmp.stages_for("framework"):
            self.assertIn(stage.name, out)
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            _, out, _ = call(["help", "test"], cwd=game)
            self.assertIn("smoke", out)
            self.assertNotIn("naming", out)

    def test_help_for_nothing_is_a_usage_error(self):
        self.assertEqual(call(["help", "nosuch"])[0], 2)

    def test_every_word_a_command_accepts_is_in_its_usage(self):
        """`rmp test all`, `rmp build debug`, `rmp fmt write` and `rmp lint
        check` were accepted and listed nowhere -- and `rmp help --json` is
        what the docs site checks command lines against. They are refused now,
        one spelling per thing; and every word a handler's one_of() takes is
        in the usage line it is listed under."""
        tree = ast.parse(RMP_PY.read_text())
        handlers = {fn.name: fn for fn in tree.body if isinstance(fn, ast.FunctionDef)}
        checked = 0
        for name, command in rmp.COMMANDS.items():
            fn = handlers[command.handler.__name__]
            for node in ast.walk(fn):
                if isinstance(node, ast.Call) and getattr(node.func, "id", "") == "one_of":
                    for word in ast.literal_eval(node.args[1]):
                        checked += 1
                        with self.subTest(command=name, word=word):
                            self.assertRegex(command.usage, rf"\b{re.escape(word)}\b")
        self.assertGreater(checked, 3)
        for argv in (["test", "all"], ["build", "debug"], ["fmt", "write"], ["lint", "check"]):
            with self.subTest(refused=" ".join(argv)):
                self.assertEqual(call(argv)[0], 2)

    def test_no_page_says_ci_runs_what_a_games_ci_does_not(self):
        """`rmp help fmt` called `rmp fmt check` "what CI runs"; a game's CI
        does not check formatting -- the fmt stage is the framework's."""
        stages = {s.name: s for s in rmp.STAGES}
        self.assertEqual(stages["fmt"].scope, "framework")
        self.assertNotIn("CI", rmp.page("fmt", "game"))


class DispatchTest(unittest.TestCase):
    def test_an_unknown_command_suggests_the_near_one(self):
        code, _, err = call(["tset"])
        self.assertEqual(code, 2)
        self.assertIn("did you mean test", err)

    def test_a_framework_command_in_a_game_says_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            code, _, err = call(["lint"], cwd=game)
            self.assertEqual(code, 2)
            self.assertIn("lint is a framework command; this is a game", err)

    def test_bad_arguments_run_nothing(self):
        for argv in (["build", "debug2"], ["push", "now"], ["deploy"], ["test", "nosuch"],
                     ["clean", "everything"], ["fmt", "all"], ["test", "unit", "smoke"]):
            with self.subTest(argv=argv), recording() as rec:
                code, _, err = call(argv)
                self.assertEqual(code, 2, err)
                self.assertEqual(rec.calls, [])

    def test_outside_a_project_only_help_works(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(call(["help"], cwd=tmp)[0], 0)
            code, _, err = call(["build"], cwd=tmp)
            self.assertEqual(code, 2)
            self.assertIn("not inside a project", err)


class RootTest(unittest.TestCase):
    def test_found_from_a_folder_deep_inside(self):
        self.assertEqual(rmp.find_root(REPO / "examples" / "games"), REPO)

    def test_the_nearest_project_wins(self):
        with tempfile.TemporaryDirectory() as tmp:
            outer = fake_project(Path(tmp) / "outer")
            inner = fake_project(outer / "sub" / "inner")
            self.assertEqual(rmp.find_root(inner / "deeper" if (inner / "deeper").mkdir() is None
                                           else inner), inner.resolve())
            self.assertEqual(rmp.find_root(outer / "sub"), outer.resolve())

    def test_nothing_above_is_none(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIsNone(rmp.find_root(Path(tmp)))


class DelegationTest(unittest.TestCase):
    """Inside a game, a framework rmp on PATH runs the game's own copy."""

    def game_with_recording_rmp(self, tmp):
        game = fake_project(Path(tmp) / "g")
        (game / "tools" / "rmp.py").write_text(
            "import json, os, sys\n"
            "print(json.dumps({'argv': sys.argv[1:], 'delegated': os.environ.get('RMP_DELEGATED')}))\n"
            "sys.exit(7)\n")
        return game

    def run_in(self, cwd, *argv, env=None):
        return subprocess.run([sys.executable, str(RMP_PY), *argv], cwd=cwd,
                              capture_output=True, text=True,
                              env=dict(os.environ, **(env or {})))

    def test_it_runs_the_projects_own_copy_with_every_argument(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = self.game_with_recording_rmp(tmp)
            got = self.run_in(game, "build", "release", "a b", "*")
            self.assertEqual(got.returncode, 7)
            said = json.loads(got.stdout)
            self.assertEqual(said["argv"], ["build", "release", "a b", "*"])
            self.assertEqual(said["delegated"], "1")

    def test_new_is_never_delegated(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = self.game_with_recording_rmp(tmp)
            got = self.run_in(game, "new", "--help")
            self.assertNotIn('"argv"', got.stdout)

    def test_the_guard_and_the_escape(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = self.game_with_recording_rmp(tmp)
            for env in ({"RMP_DELEGATED": "1"}, {"RMP_NO_DELEGATE": "1"}):
                with self.subTest(env=env):
                    got = self.run_in(game, "help", env=env)
                    self.assertTrue(got.stdout.startswith("usage: rmp"), got.stdout)

    def test_the_same_file_is_not_delegated_to(self):
        self.assertIsNone(rmp.delegate(REPO, ["help"]))


class CommandSequenceTest(unittest.TestCase):
    """What each command runs, exactly, on POSIX and on Windows."""

    NAME = {"tools/configure.py --print-name": (0, "demo\n")}

    def run_cmd(self, argv, win=False, answers=None, cwd=REPO):
        with windows(win), recording(dict(self.NAME, **(answers or {}))) as rec, quiet():
            code = rmp.main(argv, cwd=cwd)
        return code, [c for c, _ in rec.calls if "--print-name" not in " ".join(c)]

    def test_build_and_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            code, calls = self.run_cmd(["build"], cwd=game)
            self.assertEqual(code, 0)
            self.assertEqual(calls, [["cmake", "--preset", "debug"],
                                     ["cmake", "--build", "build", "--target", "demo"]])
            _, calls = self.run_cmd(["build", "release"], cwd=game)
            self.assertEqual(calls[0], ["cmake", "--preset", "release"])

    def test_run_builds_first_and_runs_the_exe_on_windows(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            _, calls = self.run_cmd(["run"], cwd=game)
            self.assertEqual(calls[-1], [str(game.resolve() / "build" / "demo")])
            _, calls = self.run_cmd(["run"], win=True, cwd=game)
            self.assertEqual(calls[-1], [str(game.resolve() / "build" / "demo.exe")])

    def test_pack_and_the_rest(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            _, calls = self.run_cmd(["pack"], cwd=game)
            self.assertEqual(calls[-1], ["cmake", "--build", "build", "--target", "pack_resources"])

    def test_web_refuses_without_the_emsdk_before_running_anything(self):
        env = dict(os.environ)
        env.pop("EMSDK", None)
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            saved = os.environ.pop("EMSDK", None)
            try:
                code, calls = self.run_cmd(["web"], cwd=game)
            finally:
                if saved is not None:
                    os.environ["EMSDK"] = saved
            self.assertEqual(code, 1)
            self.assertEqual(calls, [])

    def test_android_on_both(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            apk = game / "raymob" / "app" / "build" / "x.apk"
            apk.parent.mkdir(parents=True)
            apk.write_text("")
            _, calls = self.run_cmd(["android"], cwd=game)
            self.assertEqual(calls[-1], ["sh", "./gradlew", "assembleDebug"])

    def test_unpack_with_and_without_a_pack(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            pack = game / "resources" / "resources.rres"
            pack.parent.mkdir()
            pack.write_text("x")
            self.assertEqual(self.run_cmd(["unpack"], cwd=game)[0], 0)
            self.assertFalse(pack.exists())
            self.assertEqual(self.run_cmd(["unpack"], cwd=game)[0], 0)


class ReconfigureTest(unittest.TestCase):
    def cache(self, root, line):
        (root / "build").mkdir(parents=True, exist_ok=True)
        (root / "build" / "CMakeCache.txt").write_text(line + "\n")

    def test_another_build_type_clears_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = fake_project(Path(tmp) / "g")
            self.cache(root, "CMAKE_BUILD_TYPE:STRING=Release")
            (root / "build" / "readonly").write_text("x")
            os.chmod(root / "build" / "readonly", 0o400)
            with quiet():
                rmp.reconfigure(rmp.Ctx(root), "Debug")
            self.assertFalse((root / "build").exists())

    def test_the_same_type_keeps_it_and_no_cache_does_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = fake_project(Path(tmp) / "g")
            rmp.reconfigure(rmp.Ctx(root), "Debug")
            self.cache(root, "CMAKE_BUILD_TYPE:STRING=Debug")
            rmp.reconfigure(rmp.Ctx(root), "Debug")
            self.assertTrue((root / "build").exists())

    def test_a_cache_with_no_build_type_is_cleared(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = fake_project(Path(tmp) / "g")
            self.cache(root, "OTHER:STRING=1")
            with quiet():
                rmp.reconfigure(rmp.Ctx(root), "Debug")
            self.assertFalse((root / "build").exists())


class CleanTest(unittest.TestCase):
    def test_it_removes_exactly_what_is_generated(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = fake_project(Path(tmp) / "g")
            for rel in rmp.CLEAN_PATHS:
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text("x")
            (root / "src").mkdir()
            (root / "src" / "main.cpp").write_text("x")
            with quiet():
                self.assertEqual(rmp.main(["clean"], cwd=root), 0)
            for rel in rmp.CLEAN_PATHS:
                self.assertFalse((root / rel).exists(), rel)
            self.assertTrue((root / "src" / "main.cpp").exists())

    def test_it_is_the_generated_block_of_gitignore(self):
        text = (REPO / ".gitignore").read_text()
        for rel in rmp.CLEAN_PATHS:
            with self.subTest(path=rel):
                self.assertRegex(text, rf"(?m)^/?{re.escape(rel)}/?$")


class GitRepo:
    """A git repository in a temp folder, with a bare one as its origin.
    `pushed=False` is a game straight out of `rmp new` with `git remote add
    origin` run on it: a branch with no upstream, and an empty origin."""

    def __init__(self, tmp: Path, pushed: bool = True):
        self.env = dict(os.environ, GIT_CONFIG_GLOBAL=str(tmp / "gitconfig"),
                        GIT_CONFIG_NOSYSTEM="1", GIT_TERMINAL_PROMPT="0",
                        GIT_ALLOW_PROTOCOL="file", RMP_NO_DELEGATE="1",
                        GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                        GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
        (tmp / "gitconfig").write_text("[init]\n\tdefaultBranch = main\n")
        self.origin = tmp / "origin.git"
        self.root = fake_project(tmp / "work", name="demo")
        self.git("init", "-q", "--bare", str(self.origin), cwd=tmp)
        self.git("init", "-q")
        self.git("remote", "add", "origin", str(self.origin))
        # A stand-in configure.py: records what deploy asked of it.
        (self.root / "tools" / "configure.py").write_text(
            "import os, sys, json\n"
            "log = os.path.join(os.path.dirname(__file__), 'calls.json')\n"
            "open(log, 'a').write(json.dumps({'argv': sys.argv[1:], "
            "'ref': os.environ.get('GITHUB_REF_NAME'), "
            "'type': os.environ.get('GITHUB_REF_TYPE')}) + '\\n')\n"
            "if '--print-name' in sys.argv: print('demo')\n"
            "if '--print-ci' in sys.argv: "
            "print('on_push=' + os.environ.get('FAKE_ON_PUSH', 'true'))\n"
            "sys.exit(int(os.environ.get('FAKE_CONFIGURE_EXIT', '0')))\n")
        (self.root / ".gitignore").write_text("tools/calls.json\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "first")
        if pushed:
            self.git("push", "-q", "-u", "origin", "main")

    def git(self, *argv, cwd=None):
        return subprocess.run(["git", *argv], cwd=cwd or self.root, env=self.env,
                              capture_output=True, text=True, check=True)

    def rmp(self, *argv, env=None):
        return subprocess.run([sys.executable, str(RMP_PY), *argv], cwd=self.root,
                              env=dict(self.env, **(env or {})), capture_output=True, text=True)


class DeployTest(unittest.TestCase):
    def setUp(self):
        if shutil.which("git") is None:
            self.skipTest("git not installed")
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = GitRepo(Path(self._tmp.name))

    def tearDown(self):
        self._tmp.cleanup()

    def tags(self):
        return self.repo.git("tag").stdout.split()

    def test_the_happy_path_tags_pushes_and_asks_configure_first(self):
        got = self.repo.rmp("deploy", "1.2.0")
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertEqual(self.tags(), ["v1.2.0"])
        self.assertEqual(self.repo.git("cat-file", "-t", "v1.2.0").stdout.strip(), "tag")
        self.assertIn("Release v1.2.0", self.repo.git("tag", "-n1").stdout)
        remote = subprocess.run(["git", "ls-remote", "--tags", str(self.repo.origin)],
                                capture_output=True, text=True).stdout
        self.assertIn("refs/tags/v1.2.0", remote)
        calls = [json.loads(l) for l in (self.repo.root / "tools" / "calls.json")
                 .read_text().splitlines()]
        gate = [c for c in calls if "--strict-release" in c["argv"]]
        self.assertEqual(len(gate), 1)
        self.assertEqual((gate[0]["type"], gate[0]["ref"]), ("tag", "v1.2.0"))

    def test_the_framework_tags_its_demo_without_the_placeholder_check(self):
        (self.repo.root / rmp.FRAMEWORK_MARKER).parent.mkdir(exist_ok=True)
        (self.repo.root / rmp.FRAMEWORK_MARKER).write_text("")
        self.repo.git("add", "-A")
        self.repo.git("commit", "-q", "-m", "the framework")
        self.repo.git("push", "-q")
        got = self.repo.rmp("deploy", "1.3.0")
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        calls = [json.loads(l) for l in (self.repo.root / "tools" / "calls.json")
                 .read_text().splitlines()]
        gate = [c for c in calls if "--print-config" in c["argv"]]
        self.assertEqual(len(gate), 1)
        self.assertNotIn("--strict-release", gate[0]["argv"])
        self.assertEqual((gate[0]["type"], gate[0]["ref"]), ("tag", "v1.3.0"))

    def test_a_leading_v_is_the_same_version(self):
        self.assertEqual(self.repo.rmp("deploy", "v1.2.0").returncode, 0)
        self.assertEqual(self.tags(), ["v1.2.0"])

    def test_uncommitted_changes_are_refused(self):
        (self.repo.root / rmp.TOML).write_text("[project]\nname = \"changed\"\n")
        got = self.repo.rmp("deploy", "1.0.0")
        self.assertEqual(got.returncode, 1)
        self.assertIn("uncommitted", got.stdout)
        self.assertEqual(self.tags(), [])
        self.repo.git("add", "-A")
        got = self.repo.rmp("deploy", "1.0.0")   # staged counts too
        self.assertEqual(got.returncode, 1)
        self.assertEqual(self.tags(), [])

    def test_an_untracked_file_is_allowed(self):
        (self.repo.root / "notes.txt").write_text("x")
        self.assertEqual(self.repo.rmp("deploy", "1.0.0").returncode, 0)

    def test_a_tag_that_exists_here_or_there_is_refused(self):
        self.repo.git("tag", "v2.0.0")
        got = self.repo.rmp("deploy", "2.0.0")
        self.assertEqual(got.returncode, 1)
        self.assertIn("already exists here", got.stdout)
        self.repo.git("push", "-q", "origin", "v2.0.0")
        self.repo.git("tag", "-d", "v2.0.0")
        got = self.repo.rmp("deploy", "2.0.0")
        self.assertEqual(got.returncode, 1)
        self.assertIn("already on the remote", got.stdout)

    def test_an_unreachable_origin_is_refused_not_ignored(self):
        self.repo.git("remote", "set-url", "origin", str(self.repo.root / "nowhere.git"))
        got = self.repo.rmp("deploy", "1.0.0")
        self.assertEqual(got.returncode, 1)
        self.assertIn("could not ask origin", got.stdout)
        self.assertEqual(self.tags(), [])

    def test_head_ahead_of_origin_is_refused(self):
        (self.repo.root / "x.txt").write_text("x")
        self.repo.git("add", "-A")
        self.repo.git("commit", "-q", "-m", "local only")
        got = self.repo.rmp("deploy", "1.0.0")
        self.assertEqual(got.returncode, 1)
        self.assertIn("HEAD is not what origin/main points at", got.stdout)

    def test_a_branch_never_pushed_is_refused(self):
        self.repo.git("checkout", "-q", "-b", "feature")
        got = self.repo.rmp("deploy", "1.0.0")
        self.assertEqual(got.returncode, 1)
        self.assertIn("origin/feature does not exist", got.stdout)

    def test_a_failing_gate_makes_no_tag(self):
        got = self.repo.rmp("deploy", "1.0.0", env={"FAKE_CONFIGURE_EXIT": "1"})
        self.assertEqual(got.returncode, 1)
        self.assertEqual(self.tags(), [])


class PushTest(unittest.TestCase):
    """Refused while any CI run is live, because pushing cancels it."""

    def setUp(self):
        if shutil.which("git") is None:
            self.skipTest("git not installed")
        self._tmp = tempfile.TemporaryDirectory()
        tmp = Path(self._tmp.name)
        self.repo = GitRepo(tmp)
        self.bin = tmp / "bin"
        self.bin.mkdir()
        (self.repo.root / "x.txt").write_text("x")
        self.repo.git("add", "-A")
        self.repo.git("commit", "-q", "-m", "second")

    def tearDown(self):
        self._tmp.cleanup()

    def fake_gh(self, runs=None, exit_code=0):
        log = self.bin / "gh.log"
        script = self.bin / "gh"
        script.write_text("#!/bin/sh\n"
                          f"echo \"$@\" >> '{log}'\n"
                          f"cat <<'JSON'\n{json.dumps(runs or [])}\nJSON\n"
                          f"exit {exit_code}\n")
        script.chmod(0o755)
        return log

    def push(self, *args, gh=True):
        if gh:
            # The fake first, so it shadows a real gh.
            path = str(self.bin) + os.pathsep + os.environ["PATH"]
        else:
            # Only git, so there is no gh to find at all.
            only_git = self.bin.parent / "only_git"
            only_git.mkdir(exist_ok=True)
            if not (only_git / "git").exists():
                (only_git / "git").symlink_to(shutil.which("git"))
            path = str(only_git)
        return self.repo.rmp("push", *args, env={"PATH": path})

    def pushed(self):
        local = self.repo.git("rev-parse", "HEAD").stdout.strip()
        remote = subprocess.run(["git", "--git-dir", str(self.repo.origin), "rev-parse", "main"],
                                capture_output=True, text=True).stdout.strip()
        return local == remote

    def test_every_live_status_refuses(self):
        for status in ("queued", "in_progress", "waiting", "pending", "requested"):
            with self.subTest(status=status):
                self.fake_gh([{"databaseId": 42, "status": status, "event": "push"}])
                got = self.push()
                self.assertEqual(got.returncode, 1)
                self.assertIn("42", got.stdout)
                self.assertIn("rmp push force", got.stdout)
                self.assertFalse(self.pushed())

    def test_completed_runs_push(self):
        log = self.fake_gh([{"databaseId": 1, "status": "completed", "event": "push"}])
        got = self.push()
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertTrue(self.pushed())
        self.assertIn("--workflow ci.yml", log.read_text())

    def test_force_never_asks(self):
        log = self.fake_gh([{"databaseId": 1, "status": "in_progress", "event": "push"}])
        self.assertEqual(self.push("force").returncode, 0)
        self.assertTrue(self.pushed())
        self.assertFalse(log.exists())

    def test_without_gh_it_pushes_and_says_so(self):
        got = self.push(gh=False)
        self.assertEqual(got.returncode, 0)
        self.assertIn("gh is not installed", got.stdout)
        self.assertTrue(self.pushed())

    def test_a_failing_gh_pushes_with_a_warning(self):
        self.fake_gh(exit_code=1)
        got = self.push()
        self.assertEqual(got.returncode, 0)
        self.assertIn("warning", got.stdout)

    def test_on_push_false_starts_no_run_so_it_asks_no_one(self):
        """[ci] on_push = false: a push starts no run, and a run is only ever
        cancelled by another one starting -- a run in flight is no reason to
        refuse, and is not even looked for."""
        log = self.fake_gh([{"databaseId": 42, "status": "in_progress", "event": "push"}])
        got = self.repo.rmp("push", env={"PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
                                         "FAKE_ON_PUSH": "false"})
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertTrue(self.pushed())
        self.assertFalse(log.exists(), "gh was asked about runs a push cannot cancel")
        self.assertIn("gh workflow run ci.yml", got.stdout)
        self.assertIn("on_push = false", got.stdout)

    def test_a_ci_yml_not_committed_yet_is_checked_anyway(self):
        """The run follows the ci.yml that is committed: one rewritten for
        on_push = false and not committed still runs on this push."""
        ci = self.repo.root / ".github" / "workflows" / "ci.yml"
        ci.parent.mkdir(parents=True)
        ci.write_text("on: push\n")
        self.repo.git("add", "-A")
        self.repo.git("commit", "-q", "-m", "ci")
        ci.write_text("on: workflow_dispatch\n")
        log = self.fake_gh([{"databaseId": 42, "status": "in_progress", "event": "push"}])
        got = self.repo.rmp("push", env={"PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
                                         "FAKE_ON_PUSH": "false"})
        self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
        self.assertIn("not committed", got.stdout)
        self.assertIn("--workflow ci.yml", log.read_text())
        self.assertFalse(self.pushed())

    def test_a_project_configure_refuses_is_not_pushed(self):
        """CI's first step would stop the run: an invalid .toml, or a ci.yml
        whose triggers are not the ones [ci] on_push writes."""
        log = self.fake_gh([])
        got = self.repo.rmp("push", env={"PATH": str(self.bin) + os.pathsep + os.environ["PATH"],
                                         "FAKE_CONFIGURE_EXIT": "1"})
        self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
        self.assertIn("configure.py refused", got.stdout)
        self.assertFalse(self.pushed())
        self.assertFalse(log.exists())


class FirstPushTest(unittest.TestCase):
    """The first push of a branch sets its upstream. A game made with `rmp
    new` has `main` and, after `git remote add origin URL`, a remote -- and a
    bare `git push` there fails with "has no upstream branch"."""

    def setUp(self):
        if shutil.which("git") is None:
            self.skipTest("git not installed")
        self._tmp = tempfile.TemporaryDirectory()
        tmp = Path(self._tmp.name)
        self.repo = GitRepo(tmp, pushed=False)
        self.bin = tmp / "bin"
        self.bin.mkdir()

    def tearDown(self):
        self._tmp.cleanup()

    def push(self, *args, runs=()):
        script = self.bin / "gh"
        script.write_text(f"#!/bin/sh\ncat <<'JSON'\n{json.dumps(list(runs))}\nJSON\n")
        script.chmod(0o755)
        return self.repo.rmp("push", *args,
                             env={"PATH": str(self.bin) + os.pathsep + os.environ["PATH"]})

    def upstream(self, branch):
        got = subprocess.run(["git", "rev-parse", "--abbrev-ref", f"{branch}@{{upstream}}"],
                             cwd=self.repo.root, env=self.repo.env, capture_output=True,
                             text=True)
        return got.stdout.strip() if got.returncode == 0 else None

    def on_origin(self, branch):
        return subprocess.run(["git", "--git-dir", str(self.repo.origin), "rev-parse", "-q",
                               "--verify", f"refs/heads/{branch}"],
                              capture_output=True, text=True).stdout.strip()

    def head(self):
        return self.repo.git("rev-parse", "HEAD").stdout.strip()

    def test_a_game_from_rmp_new_pushes_the_first_time(self):
        self.assertIsNone(self.upstream("main"))
        got = self.push()
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertEqual(self.on_origin("main"), self.head())
        self.assertEqual(self.upstream("main"), "origin/main")
        # And from then on it is a plain push to that upstream.
        (self.repo.root / "x.txt").write_text("x")
        self.repo.git("add", "-A")
        self.repo.git("commit", "-q", "-m", "second")
        got = self.push()
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertEqual(self.on_origin("main"), self.head())

    def test_a_new_branch_is_pushed_under_its_own_name(self):
        self.repo.git("checkout", "-q", "-b", "feature")
        self.assertEqual(self.push().returncode, 0)
        self.assertEqual(self.on_origin("feature"), self.head())
        self.assertEqual(self.upstream("feature"), "origin/feature")
        self.assertEqual(self.on_origin("main"), "")

    def test_force_sets_the_upstream_too(self):
        got = self.push("force", runs=[{"databaseId": 7, "status": "queued", "event": "push"}])
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertEqual(self.upstream("main"), "origin/main")

    def test_a_live_run_still_refuses_the_first_push(self):
        got = self.push(runs=[{"databaseId": 7, "status": "queued", "event": "push"}])
        self.assertEqual(got.returncode, 1)
        self.assertIn("rmp push force", got.stdout)
        self.assertEqual(self.on_origin("main"), "")
        self.assertIsNone(self.upstream("main"))

    def test_without_a_remote_it_says_how_to_add_one(self):
        self.repo.git("remote", "remove", "origin")
        got = self.push()
        self.assertEqual(got.returncode, 1)
        self.assertIn("git remote add origin", got.stdout + got.stderr)


WORKFLOWS = REPO / ".github" / "workflows"


def require_yaml(case):
    """PyYAML: a skip on a laptop, a failure inside the build image -- where
    the lint job runs this file, and a skip would read as a pass."""
    try:
        import yaml
    except ImportError:
        if IN_BUILD_IMAGE:
            case.fail("PyYAML is missing inside the build image")
        case.skipTest("PyYAML not installed (it ships in the build image)")
    return yaml


def load_workflow(case, name: str) -> dict:
    return require_yaml(case).safe_load((WORKFLOWS / name).read_text())


# The two spellings of "only in the framework's own repository". Exact, so
# that a condition which merely mentions the mode is not taken for the gate.
FRAMEWORK_GATES = ("needs.config.outputs.framework == 'true'", "inputs.framework")


def gated(condition) -> bool:
    return " ".join(str(condition or "").split()) in FRAMEWORK_GATES


def code_lines(script: str) -> str:
    """A run: block without its comment lines (sh and pwsh both use #)."""
    return "\n".join(line for line in script.splitlines()
                     if not line.lstrip().startswith("#"))


def lint_steps(case):
    """(gated, code) for each step of ci.yml's lint job."""
    job = load_workflow(case, "ci.yml")["jobs"]["lint"]
    case.assertFalse(gated(job.get("if")), "the lint job itself is framework-only")
    return [(gated(step.get("if")), code_lines(str(step.get("run", "")) + " " +
                                                str(step.get("uses", ""))))
            for step in job["steps"]]


class StagesAgreeWithLintTest(unittest.TestCase):
    """Every stage of `rmp test` runs in the CI lint job too: a stage that
    exists in one place and not the other is how the UI layout test ran on
    laptops only, for four phases. And a stage a game runs has to run in a
    game's CI: its step may not be gated to the framework."""

    COVERED_BY = {"fmt-check": "clang-format", "smoke": "render_check.sh"}

    def tokens(self, stage):
        out = []
        for step in stage.steps:
            if step[0] == "run":
                for a in step[1]:
                    if a.startswith(("tools/", "build/")) or a.endswith("_test.py"):
                        out.append(a.removeprefix("build/"))
            elif step[0] == "build" and step[1]:
                out.append(step[1])
            elif step[0] in self.COVERED_BY:
                out.append(self.COVERED_BY[step[0]])
            elif step[0] == "render":
                out.append("render_check.sh")
        return out

    def test_every_stage_has_its_step_in_the_lint_job(self):
        steps = lint_steps(self)
        self.assertTrue(any("actionlint" in code for _, code in steps),
                        "the lint job did not parse")
        for stage in rmp.STAGES:
            if stage.name in ("examples", "render-update"):
                continue  # the examples job, and a recording -- not a check
            # A stage the game runs too needs a step the game's CI runs.
            usable = [code for is_gated, code in steps
                      if not is_gated or stage.scope == "framework"]
            for token in self.tokens(stage):
                with self.subTest(stage=stage.name, token=token):
                    self.assertTrue(any(token in code for code in usable),
                                    f"`rmp test {stage.name}` runs {token} and the CI lint "
                                    "job does not" + ("" if stage.scope == "framework" else
                                                      " in a game"))


class CiModeTest(unittest.TestCase):
    """One ci.yml for the framework and for every game: what a game does not
    have is skipped by the mode, and nothing a game's CI runs is missing from
    the game."""

    # Named by an ungated step and absent from a game, on purpose: each is
    # only ever probed for, and the reason says what happens without it.
    OPTIONAL = {
        "tests/fixtures/render_hash.txt":
            "the framework's golden frame: compared only where it exists",
    }

    PATH_RE = re.compile(r"(?<![\w.$/-])((?:tools|tests|cmake|examples|\.github/scripts)"
                         r"[/\\][\w./\\-]*\w)")

    def mode_block(self):
        config = load_workflow(self, "ci.yml")["jobs"]["config"]
        script = next(s["run"] for s in config["steps"] if s.get("id") == "o")
        self.assertIn("set -euo pipefail", script)
        m = re.search(r"(?ms)^\s*MODE=\$\(python3 tools/rmp\.py --mode\)\n.*?^\s*esac\n",
                      script)
        self.assertIsNotNone(m, "the config job does not read tools/rmp.py --mode")
        self.assertEqual(config["outputs"]["framework"], "${{ steps.o.outputs.framework }}")
        return m.group(0)

    def run_mode(self, said: str, code: int = 0):
        with tempfile.TemporaryDirectory() as tmp:
            stub = Path(tmp) / "python3"
            stub.write_text(f"#!/bin/sh\nprintf '%s\\n' '{said}'\nexit {code}\n")
            stub.chmod(0o755)
            out = Path(tmp) / "out"
            out.write_text("")
            got = subprocess.run(["bash", "-c", "set -euo pipefail\n" + self.mode_block()],
                                 capture_output=True, text=True,
                                 env=dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}",
                                          GITHUB_OUTPUT=str(out)))
            return got.returncode, out.read_text()

    def test_the_mode_is_one_of_two_words_or_the_run_stops(self):
        self.assertEqual(self.run_mode("framework"), (0, "framework=true\n"))
        self.assertEqual(self.run_mode("game"), (0, "framework=false\n"))
        for said, code in (("", 0), ("Framework", 0), ("game framework", 0),
                           ("framework", 1), ("game", 2)):
            with self.subTest(said=said, code=code):
                status, out = self.run_mode(said, code)
                self.assertNotEqual(status, 0)
                self.assertEqual(out, "")

    def run_tag_gate(self, ref_type, mode):
        """The config job's tag check, run with a python3 that refuses
        --strict-release the way configure.py does on a placeholder id."""
        config = load_workflow(self, "ci.yml")["jobs"]["config"]
        script = next(s["run"] for s in config["steps"] if s.get("id") == "o")
        lines = script.splitlines()
        at = [i for i, line in enumerate(lines) if "--strict-release" in line]
        self.assertEqual(len(at), 1, "the config job does not check a tag's ids")
        self.assertEqual(lines[at[0]].strip(), "python3 tools/configure.py --check --strict-release")
        start = max(i for i in range(at[0]) if lines[i].lstrip().startswith("if "))
        end = next(i for i in range(at[0], len(lines)) if lines[i].strip() == "fi")
        block = "\n".join(lines[start:end + 1]).replace("${{ github.ref_type }}", ref_type)
        with tempfile.TemporaryDirectory() as tmp:
            stub = Path(tmp) / "python3"
            stub.write_text('#!/bin/sh\necho "$*" >> "$(dirname "$0")/calls"\n'
                            'case "$*" in *--strict-release*) exit 1 ;; esac\n')
            stub.chmod(0o755)
            got = subprocess.run(["bash", "-c", "set -euo pipefail\n" + block],
                                 env=dict(os.environ, PATH=f"{tmp}:{os.environ['PATH']}",
                                          MODE=mode), capture_output=True, text=True)
            calls = (Path(tmp) / "calls").read_text() if (Path(tmp) / "calls").exists() else ""
        return got.returncode, calls

    def test_a_games_tag_refuses_placeholder_ids(self):
        """The README says a tag build refuses com.example.* ids. Only
        `rmp deploy` did; a tag pushed by hand went through."""
        self.assertEqual(self.run_tag_gate("tag", "game"),
                         (1, "tools/configure.py --check --strict-release\n"))
        self.assertEqual(self.run_tag_gate("tag", "framework"), (0, ""))
        self.assertEqual(self.run_tag_gate("branch", "game"), (0, ""))
        readme = " ".join((REPO / "README.md").read_text().split())
        self.assertIn("the build is refused while your application id is still `com.example.*`",
                      readme)

    def test_the_framework_jobs_are_gated_and_release_tells_skipped_apart(self):
        jobs = load_workflow(self, "ci.yml")["jobs"]
        release = " ".join(jobs["release"]["if"].split())
        for name in ("examples", "rmp_new", "docs"):
            with self.subTest(job=name):
                self.assertTrue(gated(jobs[name].get("if")))
                self.assertIn(name, jobs["release"]["needs"])
                self.assertIn(f"(needs.{name}.result == 'success' || "
                              f"(needs.config.outputs.framework == 'false' && "
                              f"needs.{name}.result == 'skipped'))", release)
        self.assertIn("needs.lint.result == 'success'", release)

    def test_windows_is_told_which_it_is(self):
        windows = load_workflow(self, "_windows.yml")
        on = windows.get("on", windows.get(True))   # YAML 1.1 reads `on` as True
        spec = on["workflow_call"]["inputs"]["framework"]
        self.assertEqual((spec["type"], spec["required"], "default" in spec),
                         ("boolean", True, False))
        ci = load_workflow(self, "ci.yml")["jobs"]["windows"]["with"]["framework"]
        self.assertEqual(ci, "${{ needs.config.outputs.framework == 'true' }}")
        canary = load_workflow(self, "canary.yml")["jobs"]["windows"]["with"]["framework"]
        self.assertIs(canary, True)

    def offences(self, tracked, replaced=None):
        """What a game's CI would reach for and not find. `replaced` stands a
        text in for a workflow file, for the test that sees this go red."""
        yaml = require_yaml(self)
        found = []
        for wf in sorted(WORKFLOWS.glob("*.yml")):
            rel = wf.relative_to(REPO).as_posix()
            if rmp.classify(rel)[0] != "include":
                continue
            data = yaml.safe_load((replaced or {}).get(wf.name) or wf.read_text())
            for job_name, job in (data.get("jobs") or {}).items():
                if gated(job.get("if")):
                    continue
                uses = str(job.get("uses", ""))
                if uses.startswith("./") and rmp.classify(uses[2:])[0] != "include":
                    found.append(f"{rel}: job {job_name} calls {uses}")
                for step in job.get("steps") or []:
                    if gated(step.get("if")):
                        continue
                    code = code_lines(str(step.get("run", "")))
                    code += " " + " ".join(str(v) for v in (step.get("with") or {}).values())
                    where = f"{rel}: {job_name} / {step.get('name', step.get('uses'))}"
                    if re.search(r"(?<![\w/.-])examples(?![\w/-])", code):
                        found.append(f"{where} uses examples/")
                    for path in self.PATH_RE.findall(code):
                        path = path.replace("\\", "/").rstrip("/")
                        if path in self.OPTIONAL:
                            if f"-f {path}" not in code:
                                found.append(f"{where} reads {path} without asking if it is there")
                            continue
                        under = [t for t in tracked if t == path or t.startswith(path + "/")]
                        if not under:
                            continue    # generated, or build output
                        if not any(rmp.classify(t)[0] in ("include", "rename") for t in under):
                            found.append(f"{where} uses {path}, which a game does not get")
        return found

    def test_a_game_gets_every_file_its_ci_uses(self):
        tracked = [p for _, _, p in rmp.tracked(REPO)]
        self.assertEqual(self.offences(tracked), [])

    def test_the_check_sees_each_kind_of_mistake(self):
        """Seen red: the same check with one gate or guard taken away."""
        tracked = [p for _, _, p in rmp.tracked(REPO)]
        cases = [
            ("ci.yml", "      - name: The seam holds\n"
                       "        if: needs.config.outputs.framework == 'true'\n",
             "      - name: The seam holds\n",
             "ci.yml: lint / The seam holds uses tools/seam_check.sh, "
             "which a game does not get"),
            ("_windows.yml", "      - name: Examples still compile (MSVC)\n"
                             "        if: inputs.framework\n",
             "      - name: Examples still compile (MSVC)\n",
             "_windows.yml: x64 / Examples still compile (MSVC) uses examples/"),
            ("_linux.yml", "if [ -f tests/fixtures/render_hash.txt ]; then",
             "if true; then",
             "reads tests/fixtures/render_hash.txt without asking if it is there"),
        ]
        for name, before, after, says in cases:
            with self.subTest(workflow=name, says=says):
                original = (WORKFLOWS / name).read_text()
                self.assertEqual(original.count(before), 1)
                found = self.offences(tracked, {name: original.replace(before, after)})
                self.assertEqual(len(found), 1, found)
                self.assertIn(says, found[0])


class MachineReadableTest(unittest.TestCase):
    """What the docs site reads from rmp: every command and stage as JSON, and
    the list of what a new game gets."""

    def test_help_json_is_every_command_and_stage(self):
        code, out, _ = call(["help", "--json"])
        self.assertEqual(code, 0)
        data = json.loads(out)
        self.assertEqual([c["name"] for c in data["commands"]], list(rmp.COMMANDS))
        self.assertEqual([s["name"] for s in data["stages"]], [s.name for s in rmp.STAGES])
        for c in data["commands"]:
            with self.subTest(command=c["name"]):
                self.assertTrue(c["examples"])
                self.assertEqual(c["framework_only"], rmp.COMMANDS[c["name"]].framework_only)

    def test_new_list_is_the_manifest(self):
        code, out, _ = call(["new", "--list"])
        self.assertEqual(code, 0)
        listed = out.split()
        self.assertEqual(listed, rmp.new_game_paths(rmp.tracked(REPO)))
        self.assertIn("README.md", listed)
        self.assertIn("thirdparty/raylib_multiplatform/LICENSE", listed)
        self.assertNotIn("LICENSE", listed)
        self.assertNotIn("tests/configure_test.py", listed)

    def test_a_closed_pipe_is_not_a_traceback(self):
        got = subprocess.run(f"{sys.executable} {RMP_PY} new --list | head -1", shell=True,
                             cwd=REPO, capture_output=True, text=True)
        self.assertEqual(got.stderr, "")
        self.assertEqual(len(got.stdout.splitlines()), 1)


class DocsTest(unittest.TestCase):
    """What the files say about the command is true. `just` is gone, and
    every `rmp ...` a document, a comment or a workflow shows is a command
    that exists, with an argument it takes: a stage for `rmp test`, an example
    for `rmp example`. A help page that names a command it does not have is
    the kind of lie nobody finds until they type it."""

    # The two words that may still be written down: this file names them to
    # forbid them.
    JUST = re.compile(r"\bjust (run|test|dev|rel|fmt|lint|example|push|deploy|clean|pack|"
                      r"unpack|web|android)\b|\bJustfile\b")

    def texts(self):
        for path in rmp_tracked_files():
            # thirdparty/ is other people's text -- except the notes there that
            # are ours: FROZEN_VERSIONS.md and the PATCHES.md files. Skipping
            # the whole folder is how `just fmt check` survived in one of them.
            ours = path == "thirdparty/FROZEN_VERSIONS.md" or path.endswith("/PATCHES.md")
            if (path.startswith("thirdparty/") and not ours) or path == "tests/rmp_test.py":
                continue
            try:
                yield path, (REPO / path).read_text(encoding="utf-8")
            except (UnicodeDecodeError, IsADirectoryError, FileNotFoundError):
                continue

    def test_no_just_is_left(self):
        found = [f"{path}:{text.count(chr(10), 0, m.start()) + 1}: {m.group(0)}"
                 for path, text in self.texts() for m in self.JUST.finditer(text)]
        self.assertEqual(found, [])

    def shown(self, path, text):
        """(line, words) for every rmp command line the text shows."""
        out = []
        lines = text.splitlines()
        fenced = False
        for n, line in enumerate(lines, 1):
            if line.lstrip().startswith("```"):
                fenced = not fenced
                continue
            for m in re.finditer(r"`\.?/?rmp ([^`]+)`", line):
                out.append((n, m.group(1).split()))
            if path.endswith(".md") and fenced:
                m = re.match(r"\s*\.?/?rmp ((?:[^\s#]+ ?)+)", line)
                if m:
                    out.append((n, m.group(1).split()))
            # A command listing in a comment: `#   rmp fmt check    what CI runs`.
            m = re.match(r"\s*(?:#|//)\s+rmp ([a-z-]+(?: [^\s]+)?)\s{2,}\S", line)
            if m:
                out.append((n, m.group(1).split()))
            # And in a workflow's run: block, a line that runs it.
            m = re.match(r"\s*\.?/?rmp ([a-z-]+(?: [^\s;|&>]+)*)", line)
            if m and path.startswith(".github/workflows/"):
                out.append((n, m.group(1).split()))
        return out

    def problem(self, words, examples):
        # A bare `rmp deploy` in prose names the command; only what is given
        # to it can be wrong.
        words = [w.rstrip("\\") for w in words]
        command, rest = words[0], words[1:]
        if command not in rmp.COMMANDS:
            return f"there is no `rmp {command}`"
        usage = rmp.COMMANDS[command].usage.split()[1:]
        if not rest:
            return None
        arg = rest[0]
        if arg.startswith(("<", "[")) or arg.isupper() or not usage:
            return None if usage else f"`rmp {command}` takes no argument"
        slot = usage[0].strip("[]")
        if slot == "stage":
            ok = arg in {s.name for s in rmp.STAGES}
        elif slot == "name":
            ok = arg in examples or arg.removeprefix("examples/") in examples \
                or any(e.endswith("/" + arg) for e in examples)
        elif slot == "command":
            ok = arg in rmp.COMMANDS
        elif slot.isupper():
            ok = True
        else:
            ok = arg == slot
        return None if ok else f"`rmp {command}` does not take {arg!r}"

    def test_every_command_shown_exists(self):
        examples = set(rmp.examples(rmp.Ctx(REPO)))
        self.assertIn("games/01_pong", examples)
        checked, wrong = 0, []
        for path, text in self.texts():
            for n, words in self.shown(path, text):
                checked += 1
                why = self.problem(words, examples)
                if why:
                    wrong.append(f"{path}:{n}: rmp {' '.join(words)} -- {why}")
        self.assertEqual(wrong, [])
        self.assertGreater(checked, 60, "the scan found almost nothing; did it stop matching?")

    def test_the_check_sees_a_wrong_command(self):
        examples = {"games/01_pong"}
        for words in (["tset"], ["test", "unit-tests"], ["build", "relase"],
                      ["example", "02_pong"], ["help", "nope"], ["clean", "all"]):
            with self.subTest(words=words):
                self.assertIsNotNone(self.problem(words, examples))
        for words in (["test"], ["test", "smoke"], ["build", "release"], ["example", "01_pong"],
                      ["deploy", "1.2.0"], ["new", "my_game"], ["help", "deploy"]):
            with self.subTest(words=words):
                self.assertIsNone(self.problem(words, examples))
        text = "Run it: `rmp exampel 01_pong`.\n```bash\nrmp tset\n```\n#   rmp fmt chek    x\n"
        self.assertEqual([w for _, w in self.shown("x.md", text)],
                         [["exampel", "01_pong"], ["tset"], ["fmt", "chek"]])


def rmp_tracked_files():
    got = subprocess.run(["git", "-c", "safe.directory=*", "ls-files", "-z"], cwd=REPO,
                         capture_output=True, text=True, check=True)
    return [p for p in got.stdout.split("\0") if p]


class LauncherTest(unittest.TestCase):
    """The three launchers find a Python 3.11+ and hand it every argument."""

    SH = REPO / "rmp"

    def test_they_are_ascii_and_the_sh_one_is_lf_and_executable(self):
        for name in ("rmp", "rmp.ps1", "rmp.cmd"):
            data = (REPO / name).read_bytes()
            with self.subTest(launcher=name):
                self.assertTrue(data.isascii())
                self.assertFalse(data.startswith(b"\xef\xbb\xbf"))
        self.assertTrue(self.SH.read_text().startswith("#!/bin/sh\n"))
        self.assertNotIn(b"\r", self.SH.read_bytes())
        # safe.directory: in a CI container the checkout belongs to another
        # user, and git then answers nothing at all -- which read as a launcher
        # with no eol rule.
        git = ["git", "-c", "safe.directory=*"]
        mode = subprocess.run([*git, "ls-files", "-s", "rmp"], cwd=REPO,
                              capture_output=True, text=True).stdout.split()[:1]
        if mode:   # tracked
            self.assertEqual(mode[0], "100755")
        eol = subprocess.run([*git, "check-attr", "eol", "--", "rmp"], cwd=REPO,
                             capture_output=True, text=True)
        self.assertEqual(eol.returncode, 0, eol.stderr)
        self.assertIn("eol: lf", eol.stdout)

    def shells(self):
        found = [s for s in ("sh", "bash", "dash", "zsh", "ksh", "mksh") if shutil.which(s)]
        if IN_BUILD_IMAGE:
            self.assertIn("dash", found, "the build image has no dash")
        return found

    def test_every_shell_parses_it(self):
        for shell in self.shells():
            with self.subTest(shell=shell):
                got = subprocess.run([shell, "-n", str(self.SH)], capture_output=True, text=True)
                self.assertEqual(got.returncode, 0, got.stderr)

    def test_shellcheck_reads_it_as_posix_sh(self):
        if shutil.which("shellcheck") is None:
            if IN_BUILD_IMAGE:
                self.fail("shellcheck is missing inside the build image")
            self.skipTest("shellcheck not installed")
        got = subprocess.run(["shellcheck", "-s", "sh", str(self.SH)], capture_output=True, text=True)
        self.assertEqual(got.returncode, 0, got.stdout)

    def test_every_shell_runs_help_from_anywhere(self):
        with tempfile.TemporaryDirectory() as tmp:
            want = subprocess.run([sys.executable, str(RMP_PY), "help"], cwd=tmp,
                                  capture_output=True, text=True).stdout
            self.assertTrue(want.startswith("usage: rmp"))
            for shell in self.shells():
                with self.subTest(shell=shell):
                    got = subprocess.run([shell, str(self.SH), "help"], cwd=tmp,
                                         capture_output=True, text=True)
                    self.assertEqual(got.stdout, want, got.stderr)

    def test_through_a_symlink_and_a_symlink_loop(self):
        with tempfile.TemporaryDirectory() as tmp:
            link = Path(tmp) / "rmp"
            link.symlink_to(self.SH)
            got = subprocess.run([str(link), "--mode"], cwd=REPO, capture_output=True, text=True)
            self.assertEqual(got.stdout, "framework\n")
            a, b = Path(tmp) / "a", Path(tmp) / "b"
            a.symlink_to(b)
            b.symlink_to(a)
            got = subprocess.run(["sh", str(a), "help"], cwd=tmp, capture_output=True,
                                 text=True, timeout=20)
            self.assertNotEqual(got.returncode, 0)

    def fake_bin(self, tmp, python3_works):
        bin_dir = Path(tmp) / "bin"
        bin_dir.mkdir()
        stub = bin_dir / "python3"
        if python3_works:
            stub.symlink_to(sys.executable)
        else:
            stub.write_text("#!/bin/sh\necho WRONG\nexit 1\n")
            stub.chmod(0o755)
        return bin_dir

    def test_a_python3_that_is_too_old_is_passed_over(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = self.fake_bin(tmp, python3_works=False)
            (bin_dir / "python3.12").symlink_to(sys.executable)
            got = subprocess.run(["/bin/sh", str(self.SH), "--mode"], cwd=REPO,
                                 capture_output=True, text=True,
                                 env=dict(os.environ, PATH=str(bin_dir)))
            self.assertEqual(got.stdout, "framework\n", got.stderr)

    def test_no_python_is_127_and_one_clear_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = self.fake_bin(tmp, python3_works=False)
            got = subprocess.run(["/bin/sh", str(self.SH), "help"], cwd=REPO,
                                 capture_output=True, text=True,
                                 env=dict(os.environ, PATH=str(bin_dir)))
            self.assertEqual(got.returncode, 127)
            self.assertEqual(got.stdout, "")
            self.assertIn("needs Python 3.11 or newer", got.stderr)

    def test_arguments_arrive_intact(self):
        with tempfile.TemporaryDirectory() as tmp:
            proj = fake_project(Path(tmp) / "p")
            shutil.copy(self.SH, proj / "rmp")
            (proj / "tools" / "rmp.py").write_text(
                "import json, sys\nprint(json.dumps(sys.argv[1:]))\n")
            got = subprocess.run(["sh", str(proj / "rmp"), "a b", "", "*", "$HOME"],
                                 cwd=tmp, capture_output=True, text=True)
            self.assertEqual(json.loads(got.stdout), ["a b", "", "*", "$HOME"])

    def test_the_names_agree_with_cmake(self):
        sh_names = re.search(r"for py in ([^;]+);", self.SH.read_text()).group(1).split()
        cmake = (REPO / "cmake" / "find_python.cmake").read_text()
        cmake_names = re.search(r"foreach\(_tpl_name ([^)]+)\)", cmake).group(1).split()
        self.assertEqual(sh_names, [n for n in cmake_names if n != "py"])
        ps1 = re.findall(r"'([^']+)'", re.search(r"foreach \(\$candidate in ([^)]+)\)",
                                                   (REPO / "rmp.ps1").read_text()).group(1))
        cmd = re.findall(r'"([^"]+)"', re.search(r"for %%P in \(([^)]+)\)",
                                                   (REPO / "rmp.cmd").read_text()).group(1))
        self.assertEqual(ps1, cmd)

    def test_pwsh_parses_and_runs_it_when_there(self):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("pwsh not installed; the Windows job runs it for real")
        got = subprocess.run([pwsh, "-NoProfile", "-File", str(REPO / "rmp.ps1"), "help"],
                             cwd=REPO, capture_output=True, text=True)
        self.assertTrue(got.stdout.startswith("usage: rmp"), got.stderr)


class WindowsCheckoutTest(unittest.TestCase):
    """On Windows a checkout has CRLF endings -- .gitattributes says
    `eol=native` for everything but the shell scripts -- and `rmp test config`
    has to say there what it says here. versions_check.sh read a CRLF
    FROZEN_VERSIONS.md as having no versions block at all."""

    # What versions_check.sh reads. Running the LF copy must print what the
    # real tree prints, which is what says this list is complete.
    READS = ("thirdparty/FROZEN_VERSIONS.md", "raymob/app/build.gradle",
             "raymob/build.gradle", "raymob/gradle/wrapper/gradle-wrapper.properties",
             "raymob/generated.properties", rmp.TOML, "tools/configure.py",
             "tools/license_db.py", rmp.FRAMEWORK_MARKER)

    def copy(self, root: Path, crlf: bool) -> Path:
        script = root / "tools" / "versions_check.sh"
        script.parent.mkdir(parents=True)
        shutil.copy(REPO / "tools" / "versions_check.sh", script)   # *.sh stays LF
        files = [Path(p) for p in self.READS if (REPO / p).is_file()]
        files += [p.relative_to(REPO) for p in (REPO / ".github" / "workflows").glob("*.yml")]
        for rel in files:
            data = (REPO / rel).read_bytes()
            if crlf:
                data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_bytes(data)
        return script

    def run_check(self, script: Path):
        got = subprocess.run([rmp.find_bash(), str(script)], capture_output=True, text=True)
        return got.returncode, got.stdout

    def test_a_crlf_checkout_says_what_an_lf_one_says(self):
        real = self.run_check(REPO / "tools" / "versions_check.sh")
        with tempfile.TemporaryDirectory() as lf, tempfile.TemporaryDirectory() as crlf:
            self.assertEqual(self.run_check(self.copy(Path(lf), crlf=False)), real,
                             "the LF copy is missing a file versions_check.sh reads")
            self.assertEqual(self.run_check(self.copy(Path(crlf), crlf=True)), real)


class ManifestTest(unittest.TestCase):
    """Every file the framework tracks is in rmp new's manifest, one way or
    the other -- so a new file forces the question of whether games get it --
    and no pattern in it is dead."""

    def entries(self):
        return rmp.tracked(REPO)

    def test_every_tracked_path_is_classified(self):
        unclassified = [p for _, _, p in self.entries() if rmp.classify(p) is None]
        self.assertEqual(unclassified, [])

    def test_every_test_target_a_game_lacks_is_guarded(self):
        """CMakePresets.json goes into every game, and its `lint` preset turns
        on BUILD_TESTS, BUILD_UI_TESTS and RMP_BUILD_EXAMPLES. In a game made
        with rmp new -- tests/ holds smoke_test.h and nothing else -- that
        configure stopped at "No SOURCES given to target" for unit_test,
        ui_layout_test and input_play. Every executable built from a file a
        game does not get is defined only when that file is there."""
        lines = (REPO / "CMakeLists.txt").read_text().splitlines()
        stack = []
        found = 0
        for line in lines:
            code = line.split("#", 1)[0].strip()
            if re.match(r"if\s*\(", code):
                stack.append(code)
            elif re.match(r"endif\s*\(", code) and stack:
                stack.pop()
            match = re.match(r"add_executable\((\w+)", code)
            if not match:
                continue
            target = match.group(1)
            if target not in ("unit_test", "ui_layout_test", "input_play", "platformer_play"):
                continue
            found += 1
            with self.subTest(target=target):
                self.assertTrue(any("EXISTS" in cond or "UNIT_TEST_SOURCES" in cond
                                    for cond in stack),
                                f"{target} is not guarded by the file it is built from")
        self.assertEqual(found, 4)

    def test_every_tool_and_workflow_speaks_english(self):
        """Talk to Omar in Spanish; code, comments and messages are English --
        and a game's author reads these. `rmp`, render_check.sh and the
        workflows a game runs printed the Spanish for "FAIL:", and once those
        spoke English the framework's own gates and workflows still did:
        header_check.sh, lint.sh, examples_build.sh, web-backends.yml and nine
        more. Every tool and workflow is read, a game's and the framework's
        alike, and the tests that run them -- a test expecting the old word
        keeps it alive. The word is spelt in two halves here, so that this
        test is not the one place left that says it."""
        old = "FAL" + "LA"
        scanned = game = 0
        for _, _, path in self.entries():
            if not path.startswith(("tools/", ".github/", "tests/")) and \
                    path not in ("rmp", "rmp.ps1", "rmp.cmd"):
                continue
            if path.startswith("tests/fixtures/"):
                continue
            scanned += 1
            if rmp.classify(path)[0] in ("include", "rename"):
                game += 1
            text = (REPO / path).read_text(errors="replace")
            for lineno, line in enumerate(text.splitlines(), 1):
                if re.search(rf"\b{old}\b", line):
                    with self.subTest(at=f"{path}:{lineno}"):
                        self.fail(f"{path}:{lineno} says {old}: {line.strip()}")
        self.assertGreater(game, 15)
        self.assertGreater(scanned, game + 40)

    def test_every_pattern_matches_something(self):
        used = {rmp.classify(p)[1] for _, _, p in self.entries()}
        every = set(rmp.INCLUDE) | set(rmp.RENAME) | set(rmp.GITLINKS) | set(rmp.FRAMEWORK_ONLY)
        self.assertEqual(sorted(every - used), [])

    def test_every_gitlink_is_listed(self):
        links = [p for m, _, p in self.entries() if m == "160000"]
        self.assertEqual(sorted(links), sorted(rmp.GITLINKS))

    def test_the_framework_marker_never_reaches_a_game(self):
        self.assertEqual(rmp.classify(rmp.FRAMEWORK_MARKER)[0], "framework")

    def test_every_framework_only_entry_says_why(self):
        for pattern, reason in rmp.FRAMEWORK_ONLY.items():
            with self.subTest(pattern=pattern):
                self.assertGreater(len(reason), 10)

    def test_no_symlink_is_copied(self):
        for mode, _, path in self.entries():
            if rmp.classify(path)[0] in ("include", "rename"):
                with self.subTest(path=path):
                    self.assertNotEqual(mode, "120000")


class NewTest(unittest.TestCase):
    """rmp new, end to end: the tree it writes, and the game's own checks."""

    @classmethod
    def setUpClass(cls):
        if shutil.which("git") is None:
            raise unittest.SkipTest("git not installed")
        cls._tmp = tempfile.TemporaryDirectory()
        cls.parent = Path(cls._tmp.name)
        cls.got = subprocess.run([sys.executable, str(RMP_PY), "new", "space-rocks"],
                                 cwd=cls.parent, capture_output=True, text=True)
        cls.game = cls.parent / "space-rocks"

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def git(self, *argv):
        return subprocess.run(["git", "-c", "safe.directory=*", *argv], cwd=self.game,
                              capture_output=True, text=True).stdout

    def test_it_succeeds_and_says_what_next(self):
        self.assertEqual(self.got.returncode, 0, self.got.stdout + self.got.stderr)
        for line in ("created space-rocks/", "com.example.space_rocks", "com.example.space-rocks",
                     "rmp run", "git submodule update --init", "a Google Play id is"):
            self.assertIn(line, self.got.stdout)

    def test_the_tree_is_the_manifest_and_nothing_else(self):
        entries = rmp.tracked(REPO)
        want = {rmp.RENAME.get(p, p) for _, _, p in entries
                if rmp.classify(p)[0] in ("include", "rename")}
        want |= {"README.md", "thirdparty/raylib-ios"}
        have = set(self.git("ls-files").split())
        self.assertEqual(have, want)

    def test_a_game_does_not_keep_the_frameworks_clang_pin(self):
        """A Windows runner ships clang-format 20, and the game's rmp test
        config failed on it. The pin is the framework's; in a game it is a skip,
        and in the framework it is still a DRIFT -- both seen here."""
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp)
            for tool in ("clang-format", "clang-tidy"):
                stub = bin_dir / tool
                stub.write_text("#!/bin/sh\necho 'Ubuntu clang-format version 20.1.8'\n")
                stub.chmod(0o755)
            env = dict(os.environ, PATH=f"{bin_dir}:{os.environ['PATH']}")
            game = subprocess.run(["bash", "tools/versions_check.sh"], cwd=self.game,
                                  capture_output=True, text=True, env=env)
            self.assertIn("a game formats with the clang-format it has", game.stdout)
            self.assertNotIn("DRIFT clang", game.stdout)
            framework = subprocess.run(["bash", "tools/versions_check.sh"], cwd=REPO,
                                       capture_output=True, text=True, env=env)
            self.assertIn("DRIFT clang-format", framework.stdout)
            self.assertNotEqual(framework.returncode, 0)

    def test_new_list_said_so_beforehand(self):
        listed = subprocess.run([sys.executable, str(RMP_PY), "new", "--list"], cwd=REPO,
                                capture_output=True, text=True).stdout.split()
        self.assertEqual(sorted(listed), sorted(self.git("ls-files").split()))

    def test_none_of_the_framework_comes_along(self):
        have = self.git("ls-files")
        for forbidden in ("CLAUDE.md", ".claude/", "examples/", "TECHNICAL.md", ".clang-tidy",
                          "tests/configure_test.py", "tests/rmp_test.py", "tests/unit_test.cpp",
                          "tests/fixtures", "canary.yml", "autofix.yml",
                          "tools/naming_check.sh"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, have)
        self.assertFalse((self.game / "LICENSE").exists(), "a root LICENSE makes GitHub call it MIT")
        self.assertTrue((self.game / "tests" / "smoke_test.h").is_file())
        # Of tests/, the CI hook and the game's own tests, and nothing else.
        self.assertEqual(sorted(p for p in have.split() if p.startswith("tests/")),
                         ["tests/game/main_menu_test.cpp", "tests/smoke_test.h"])

    def test_it_brings_its_own_tests_and_what_builds_them(self):
        """tests/game/ is the game's: doctest comes with it, and the CMake
        that builds it into game_test."""
        have = self.git("ls-files").split()
        for needed in ("tests/game/main_menu_test.cpp", "thirdparty/doctest/doctest.h",
                       "thirdparty/doctest/LICENSE", "cmake/game_tests.cmake"):
            with self.subTest(needed=needed):
                self.assertIn(needed, have)
        self.assertIn('include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/game_tests.cmake")',
                      (self.game / "CMakeLists.txt").read_text())
        got = subprocess.run([sys.executable, "tools/rmp.py", "help", "test"], cwd=self.game,
                             capture_output=True, text=True,
                             env=dict(os.environ, RMP_NO_DELEGATE="1"))
        self.assertRegex(got.stdout, r"(?m)^  unit +your tests in tests/game/")

    def test_untransformed_files_are_byte_for_byte_the_frameworks(self):
        changed = {rmp.TOML, "THIRD_PARTY_LICENSES.md"}
        for _, _, path in rmp.tracked(REPO):
            kind = rmp.classify(path)[0]
            if kind not in ("include", "rename") or path in changed:
                continue
            dest = self.game / rmp.RENAME.get(path, path)
            with self.subTest(path=path):
                self.assertEqual(dest.read_bytes(), (REPO / path).read_bytes())

    def test_the_toml_is_this_game_and_otherwise_the_frameworks(self):
        import tomllib
        mine = tomllib.loads((self.game / rmp.TOML).read_text())
        base = tomllib.loads((REPO / rmp.TOML).read_text())
        self.assertEqual(mine["project"]["name"], "space-rocks")
        self.assertEqual(mine["window"]["title"], "space-rocks")
        self.assertEqual(mine["android"]["application_id"], "com.example.space_rocks")
        self.assertEqual(mine["ios"]["bundle_id"], "com.example.space-rocks")
        self.assertNotEqual(mine["resources"]["rres_password"], base["resources"]["rres_password"])
        # Ads are opt-in: the framework keeps them on so its Android job builds
        # the ads path, and a game starts without the SDK, AD_ID and INTERNET.
        self.assertIs(base["android"]["admob"]["enabled"], True)
        self.assertIs(mine["android"]["admob"]["enabled"], False)
        # ES 2.0 for a game; the framework compiles the ES 3.0 path in its CI.
        self.assertEqual(base["android"]["gl_version"], "ES30")
        self.assertEqual(mine["android"]["gl_version"], "ES20")
        for table in ("project", "window", "android", "ios", "resources"):
            mine[table] = dict(mine[table])
        mine["android"]["admob"] = dict(mine["android"]["admob"], enabled=True)
        mine["android"]["gl_version"] = base["android"]["gl_version"]
        for key in ("name",):
            mine["project"][key] = base["project"][key]
        mine["window"]["title"] = base["window"]["title"]
        mine["android"]["application_id"] = base["android"]["application_id"]
        mine["ios"]["bundle_id"] = base["ios"]["bundle_id"]
        mine["resources"]["rres_password"] = base["resources"]["rres_password"]
        self.assertEqual(mine, base)
        self.assertEqual(len((self.game / rmp.TOML).read_text().splitlines()),
                         len((REPO / rmp.TOML).read_text().splitlines()))

    def test_the_readme_describes_the_game_it_is_in(self):
        """It said "Your game is src/main.cpp", and src/main.cpp is one line
        naming the first scene: the game is in src/scenes/. Every path it names
        is in the game, and every part of the game a person edits is named."""
        readme = (self.game / "README.md").read_text()
        self.assertNotIn("Your game is `src/main.cpp`", readme)
        named = set(re.findall(r"`([^`\s]+/?)`", readme))
        for path in sorted(named):
            if "/" in path or path.endswith((".toml", ".png", ".cpp")):
                with self.subTest(named=path):
                    self.assertTrue((self.game / path).exists(), f"{path} is not in the game")
        for part in ("src/main.cpp", "src/scenes/", "resources/", "branding/icon.png",
                     "include/", rmp.TOML, "src/rmp/", "include/rmp/"):
            with self.subTest(part=part):
                self.assertIn(f"`{part}`", readme)
        self.assertIn("RMP_GAME", readme)

    def test_the_games_own_checks_pass(self):
        for check in (["tools/configure.py", "--check"], ["tools/license_db.py", "--check"]):
            got = subprocess.run([sys.executable, *check], cwd=self.game, capture_output=True,
                                 text=True)
            with self.subTest(check=check):
                self.assertEqual(got.returncode, 0, got.stdout + got.stderr)

    def test_a_tag_build_refuses_its_placeholder_ids(self):
        got = subprocess.run([sys.executable, "tools/configure.py", "--print-config",
                              "--strict-release"], cwd=self.game, capture_output=True, text=True,
                             env=dict(os.environ, GITHUB_REF_TYPE="tag", GITHUB_REF_NAME="v1.0.0"))
        self.assertNotEqual(got.returncode, 0)
        self.assertIn("placeholder identifiers", got.stdout + got.stderr)

    def test_the_licences_credit_the_framework_and_keep_doctest_unshipped(self):
        """doctest is the game's now -- tests/game/ is built with it -- so its
        row and its pin stay, and it is linked into nothing that ships."""
        lic = (self.game / "THIRD_PARTY_LICENSES.md").read_text()
        self.assertRegex(lic, r"(?m)^raylib_multiplatform +thirdparty/raylib_multiplatform +MIT")
        self.assertTrue((self.game / "thirdparty" / "raylib_multiplatform" / "LICENSE").is_file())
        self.assertRegex(lic, r"(?m)^doctest +thirdparty/doctest +MIT +- +no +none +file$")
        self.assertIn("- **doctest**", lic)
        frozen = (self.game / "thirdparty" / "FROZEN_VERSIONS.md").read_text()
        self.assertRegex(frozen, r"(?m)^sha256_doctest +[0-9a-f]{64}$")

    def test_the_gitlink_and_the_exec_bits(self):
        pin = next(s for m, s, p in rmp.tracked(REPO) if p == "thirdparty/raylib-ios")
        self.assertIn(f"160000 {pin} 0\tthirdparty/raylib-ios",
                      self.git("ls-files", "-s", "thirdparty/raylib-ios"))
        self.assertTrue((self.game / "thirdparty" / "raylib-ios").is_dir())
        for path in ("rmp", "raymob/gradlew", "tools/render_check.sh"):
            with self.subTest(path=path):
                self.assertTrue(self.git("ls-files", "-s", path).startswith("100755"))
        status = self.git("status", "--porcelain")
        self.assertNotIn("?? ", status.replace("?? tools/__pycache__/", ""))
        self.assertNotIn(" D ", status)
        self.assertFalse((self.game / ".git" / "modules").exists())

    def test_it_is_a_game_and_its_rmp_knows(self):
        got = subprocess.run([sys.executable, "tools/rmp.py", "--mode"], cwd=self.game,
                             capture_output=True, text=True)
        self.assertEqual(got.stdout, "game\n")
        got = subprocess.run([sys.executable, "tools/rmp.py", "help"], cwd=self.game,
                             capture_output=True, text=True)
        self.assertNotIn("framework only", got.stdout)
        got = subprocess.run([sys.executable, "tools/rmp.py", "new", "x"], cwd=self.parent,
                             capture_output=True, text=True, env=dict(os.environ))
        got = subprocess.run([sys.executable, str(self.game / "tools" / "rmp.py"), "new", "y"],
                             cwd=self.parent, capture_output=True, text=True)
        self.assertEqual(got.returncode, 1)
        self.assertIn("belongs to the game", got.stdout)


class NewRefusesTest(unittest.TestCase):
    def new(self, cwd, *argv):
        return subprocess.run([sys.executable, str(RMP_PY), "new", *argv], cwd=cwd,
                              capture_output=True, text=True)

    def test_a_folder_that_is_not_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "full").mkdir()
            (Path(tmp) / "full" / ".hidden").write_text("x")
            got = self.new(tmp, "full")
            self.assertEqual(got.returncode, 1)
            self.assertIn("not empty", got.stdout)
            self.assertEqual(os.listdir(Path(tmp) / "full"), [".hidden"])

    def test_names_that_cannot_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            for name in ("2048", "my game", "rmp", "con", "unit_test"):
                with self.subTest(name=name):
                    got = self.new(tmp, name)
                    self.assertEqual(got.returncode, 2, got.stdout + got.stderr)
                    self.assertFalse((Path(tmp) / name).exists())

    def test_inside_the_framework(self):
        got = self.new(REPO / "examples", "nested_game")
        self.assertEqual(got.returncode, 1)
        self.assertIn("inside the framework", got.stdout)
        self.assertFalse((REPO / "examples" / "nested_game").exists())

    def test_a_failure_halfway_leaves_nothing_behind(self):
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp) / "bin"
            bin_dir.mkdir()
            real_git = shutil.which("git")
            (bin_dir / "git").write_text(
                "#!/bin/sh\n"
                'case "$*" in *update-index*) echo boom >&2; exit 1 ;; esac\n'
                f'exec "{real_git}" "$@"\n')
            (bin_dir / "git").chmod(0o755)
            env = dict(os.environ, PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}")
            for existed in (False, True):
                with self.subTest(existed=existed):
                    target = Path(tmp) / f"g{int(existed)}"
                    if existed:
                        target.mkdir()
                    got = subprocess.run([sys.executable, str(RMP_PY), "new", target.name],
                                         cwd=tmp, capture_output=True, text=True, env=env)
                    self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
                    if existed:
                        self.assertEqual(os.listdir(target), [])
                    else:
                        self.assertFalse(target.exists())


class StripComponentsTest(unittest.TestCase):
    """What FRAMEWORK_ONLY keeps under thirdparty/ leaves no record in a game:
    a licence row whose files are not there, or a pin with no row, fails the
    game's own license_db.py --check. Nothing is kept back today -- doctest
    ships, for tests/game/ -- so the strip is run here on a copy, with doctest
    standing in for the next test-only library."""

    def copy(self, root: Path) -> Path:
        (root / "thirdparty").mkdir(parents=True)
        shutil.copy(REPO / "THIRD_PARTY_LICENSES.md", root / "THIRD_PARTY_LICENSES.md")
        shutil.copy(REPO / "thirdparty" / "FROZEN_VERSIONS.md",
                    root / "thirdparty" / "FROZEN_VERSIONS.md")
        return root

    def test_every_trace_goes_and_nothing_else(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.copy(Path(tmp))
            rmp.strip_components(root, ["thirdparty/doctest"])
            for rel in ("THIRD_PARTY_LICENSES.md", "thirdparty/FROZEN_VERSIONS.md"):
                with self.subTest(file=rel):
                    after = (root / rel).read_text().splitlines()
                    before = (REPO / rel).read_text().splitlines()
                    self.assertNotIn("doctest", "\n".join(after))
                    # Lines taken out, none changed or added.
                    gone = [line for line in before if line not in after]
                    self.assertEqual(len(before) - len(after), len(gone))
                    self.assertTrue(all("doctest" in line or line.startswith("  ")
                                        for line in gone), gone)

    def test_a_record_that_is_not_there_is_a_bug_said_so(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = self.copy(Path(tmp))
            with self.assertRaises(rmp.Refused) as caught:
                rmp.strip_components(root, ["thirdparty/nothing_here"])
            self.assertIn("bug in the framework", str(caught.exception))
            rmp.strip_components(root, ["thirdparty/doctest"])
            with self.assertRaises(rmp.Refused):
                rmp.strip_components(root, ["thirdparty/doctest"])

    def test_the_components_are_read_from_framework_only(self):
        self.assertEqual(rmp.framework_only_components(), [])
        saved = dict(rmp.FRAMEWORK_ONLY)
        try:
            rmp.FRAMEWORK_ONLY["thirdparty/rapidcheck/"] = "a test-only library"
            self.assertEqual(rmp.framework_only_components(), ["thirdparty/rapidcheck"])
        finally:
            rmp.FRAMEWORK_ONLY.clear()
            rmp.FRAMEWORK_ONLY.update(saved)


class GameUnitStageTest(unittest.TestCase):
    """`rmp test unit` in a game builds tests/game/ into game_test and runs it;
    with no test there it says so and runs nothing. In the framework the unit
    stage runs the demo game's copy too, so the scaffold every game gets is
    tested on every `rmp test`."""

    def run_cmd(self, argv, cwd):
        out = io.StringIO()
        with recording({"--print-name": (0, "demo\n")}) as rec, \
                contextlib.redirect_stdout(out):
            code = rmp.main(argv, cwd=cwd)
        return code, [c for c, _ in rec.calls], out.getvalue()

    def test_with_a_test_it_builds_and_runs_game_test(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            (game / "tests" / "game").mkdir(parents=True)
            (game / "tests" / "game" / "rules_test.cpp").write_text("")
            code, calls, _ = self.run_cmd(["test", "unit"], game)
            self.assertEqual(code, 0)
            self.assertEqual(calls, [["cmake", "--preset", "debug", "-DBUILD_TESTS=ON"],
                                     ["cmake", "--build", "build", "--target", "game_test"],
                                     [str(game.resolve() / "build" / "game_test")]])

    def test_without_one_it_says_so_and_runs_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            (game / "tests" / "game").mkdir(parents=True)
            (game / "tests" / "game" / "notes.txt").write_text("")
            code, calls, out = self.run_cmd(["test", "unit"], game)
            self.assertEqual(code, 0)
            self.assertEqual(calls, [])
            self.assertIn("skip  nothing matches tests/game/*.cpp", out)

    def test_the_framework_runs_the_demo_games_copy(self):
        stage = next(s for s in rmp.stages_for("framework") if s.name == "unit")
        self.assertIn(("build", "game_test"), stage.steps)
        self.assertIn(("run", ["build/game_test"]), stage.steps)
        self.assertIsNone(stage.when)
        self.assertTrue(any((REPO / "tests" / "game").glob("*.cpp")))
        names = [s.name for s in rmp.stages_for("game")]
        self.assertEqual(len(names), len(set(names)), "two stages of one name in a game")
        names = [s.name for s in rmp.stages_for("framework")]
        self.assertEqual(len(names), len(set(names)), "two stages of one name here")

    def test_a_game_formats_its_tests_and_not_the_frameworks_hook(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            for rel in ("tests/game/rules_test.cpp", "tests/smoke_test.h", "src/main.cpp",
                        "src/rmp/app.cpp"):
                (game / rel).parent.mkdir(parents=True, exist_ok=True)
                (game / rel).write_text("")
            got = sorted(p.relative_to(game).as_posix() for p in rmp.our_sources(rmp.Ctx(game)))
            self.assertEqual(got, ["src/main.cpp", "tests/game/rules_test.cpp"])


class GameOnPushTest(unittest.TestCase):
    """[ci] on_push = false in a game made with rmp new, end to end and with
    nothing stood in for: the game's own configure.py writes ci.yml, the file
    differs from the framework's between the markers and nowhere else, its
    workflows still lint, --check refuses a ci.yml left behind, and its rmp
    push starts no run and asks about none. The rmp_new CI job does the same
    to the game it makes from every commit."""

    @classmethod
    def setUpClass(cls):
        if shutil.which("git") is None:
            raise unittest.SkipTest("git not installed")
        cls._tmp = tempfile.TemporaryDirectory()
        cls.parent = Path(cls._tmp.name)
        made = subprocess.run([sys.executable, str(RMP_PY), "new", "quiet_game"], cwd=cls.parent,
                              capture_output=True, text=True)
        assert made.returncode == 0, made.stdout + made.stderr
        cls.game = cls.parent / "quiet_game"
        cls.env = dict(os.environ, RMP_NO_DELEGATE="1", GIT_TERMINAL_PROMPT="0",
                       GIT_CONFIG_GLOBAL=str(cls.parent / "gitconfig"), GIT_CONFIG_NOSYSTEM="1",
                       GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
                       GIT_COMMITTER_EMAIL="t@t", GIT_ALLOW_PROTOCOL="file")
        # Generating writes ci.yml on a machine; inside GitHub Actions every
        # run only compares, and this suite runs there too.
        cls.env.pop("GITHUB_ACTIONS", None)
        toml = cls.game / rmp.TOML
        text = toml.read_text()
        assert text.count("\non_push = true\n") == 1
        toml.write_text(text.replace("\non_push = true\n", "\non_push = false\n"))
        cls.generated = cls.configure()

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    @classmethod
    def configure(cls, *argv):
        return subprocess.run([sys.executable, "tools/configure.py", *argv], cwd=cls.game,
                              capture_output=True, text=True, env=cls.env)

    def git(self, *argv):
        return subprocess.run(["git", "-c", "safe.directory=*", *argv], cwd=self.game,
                              capture_output=True, text=True, env=self.env)

    def ci(self) -> Path:
        return self.game / ".github" / "workflows" / "ci.yml"

    def test_generating_rewrote_ci_yml_and_said_so(self):
        self.assertEqual(self.generated.returncode, 0,
                         self.generated.stdout + self.generated.stderr)
        self.assertIn("rewrote the triggers of .github/workflows/ci.yml", self.generated.stdout)
        again = self.configure()
        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        self.assertNotIn("rewrote", again.stdout, "a second run rewrote it again")

    def test_only_the_toml_and_ci_yml_changed(self):
        changed = self.git("diff", "--name-only").stdout.split()
        self.assertEqual(sorted(changed), [".github/workflows/ci.yml", rmp.TOML])

    def test_ci_yml_is_the_frameworks_outside_the_markers(self):
        mine = self.ci().read_text().splitlines()
        theirs = (REPO / ".github" / "workflows" / "ci.yml").read_text().splitlines()
        begin = next(i for i, line in enumerate(theirs) if "# BEGIN generated" in line)
        end = next(i for i, line in enumerate(theirs) if "# END generated" in line)
        my_end = next(i for i, line in enumerate(mine) if "# END generated" in line)
        self.assertEqual(mine[:begin + 1], theirs[:begin + 1])
        self.assertEqual(mine[my_end:], theirs[end:])
        self.assertNotEqual(mine, theirs)
        inside = "\n".join(mine[begin:my_end])
        self.assertNotIn("branches:", inside)
        self.assertNotIn("pull_request", inside)
        self.assertIn("tags: ['v*']", inside)

    def test_the_workflows_still_lint(self):
        if shutil.which("actionlint") is None:
            if IN_BUILD_IMAGE:
                self.fail("actionlint is missing inside the build image")
            self.skipTest("actionlint not installed (it ships in the build image)")
        got = subprocess.run(["actionlint"], cwd=self.game, capture_output=True, text=True)
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        got = subprocess.run([rmp.find_bash(), str(REPO / "tools" / "workflow_check.sh"),
                              str(self.game)], capture_output=True, text=True)
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)

    def test_check_passes_and_a_ci_yml_left_behind_is_red(self):
        got = self.configure("--check")
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        mine = self.ci().read_bytes()
        try:
            self.ci().write_bytes((REPO / ".github" / "workflows" / "ci.yml").read_bytes())
            got = self.configure("--check")
            self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
            self.assertIn("on_push = false", got.stderr)   # the .toml line, quoted
            self.assertIn(".github/workflows/ci.yml", got.stderr)
            got = self.configure("--print-name")
            self.assertEqual(got.returncode, 1)
        finally:
            self.ci().write_bytes(mine)

    def test_its_push_starts_no_run_and_asks_about_none(self):
        work = self.parent / "pushed"
        if work.exists():
            shutil.rmtree(work)
        shutil.copytree(self.game, work, symlinks=True)
        origin = self.parent / "origin.git"
        subprocess.run(["git", "init", "-q", "--bare", str(origin)], check=True, env=self.env)
        for argv in (["add", "-A"], ["commit", "-q", "-m", "New game"],
                     ["remote", "add", "origin", str(origin)]):
            subprocess.run(["git", *argv], cwd=work, check=True, env=self.env,
                           capture_output=True)
        bin_dir = self.parent / "bin"
        bin_dir.mkdir(exist_ok=True)
        log = bin_dir / "gh.log"
        (bin_dir / "gh").write_text(f"#!/bin/sh\necho \"$@\" >> '{log}'\n"
                                    "echo '[{\"databaseId\": 9, \"status\": \"queued\"}]'\n")
        (bin_dir / "gh").chmod(0o755)
        got = subprocess.run([sys.executable, "tools/rmp.py", "push"], cwd=work,
                             capture_output=True, text=True,
                             env=dict(self.env, PATH=f"{bin_dir}{os.pathsep}{os.environ['PATH']}"))
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertIn("gh workflow run ci.yml", got.stdout)
        self.assertFalse(log.exists(), "gh was asked about runs this push cannot cancel")
        remote = subprocess.run(["git", "--git-dir", str(origin), "rev-parse", "main"],
                                capture_output=True, text=True).stdout.strip()
        self.assertEqual(remote, subprocess.run(["git", "rev-parse", "HEAD"], cwd=work,
                                                capture_output=True, text=True).stdout.strip())


class SourceTest(unittest.TestCase):
    def test_it_parses_as_python_3_8_so_old_pythons_get_the_sentence(self):
        ast.parse(RMP_PY.read_text(), feature_version=(3, 8))

    def test_it_is_ascii(self):
        self.assertTrue(RMP_PY.read_bytes().isascii())

    def test_an_old_python_is_told_which_one_it_is(self):
        text = RMP_PY.read_text()
        first_import = text.index("\nimport ")
        self.assertLess(text.index("sys.version_info < (3, 11)"), text.index("\nimport difflib"))
        self.assertGreater(first_import, 0)


class ModeTest(unittest.TestCase):
    def test_the_framework_and_a_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(rmp.mode_of(fake_project(Path(tmp) / "f", framework=True)),
                             "framework")
            self.assertEqual(rmp.mode_of(fake_project(Path(tmp) / "g")), "game")
        self.assertEqual(rmp.mode_of(REPO), "framework")

    def test_mode_prints_exactly_one_word(self):
        got = subprocess.run([sys.executable, str(RMP_PY), "--mode"], cwd=REPO,
                             capture_output=True, text=True)
        self.assertEqual(got.stdout, "framework\n")
        with tempfile.TemporaryDirectory() as tmp:
            game = fake_project(Path(tmp) / "g")
            got = subprocess.run([sys.executable, str(RMP_PY), "--mode"], cwd=game,
                                 capture_output=True, text=True,
                                 env=dict(os.environ, RMP_NO_DELEGATE="1"))
            self.assertEqual(got.stdout, "game\n")


# ---------------------------------------------------------------------------
# The installers -- tools/install.sh, tools/install.ps1 -- and the two commands
# they run, `rmp install` and `rmp update`.
# ---------------------------------------------------------------------------

INSTALL_SH = REPO / "tools" / "install.sh"
INSTALL_PS1 = REPO / "tools" / "install.ps1"

# What a framework is, as far as installing it goes: the launchers, rmp.py and
# the installers AS THEY ARE IN THIS WORKING TREE (a clone of REPO would test
# the last commit), plus the marker and a .toml.
INSTALLED_FILES = ("rmp", "rmp.ps1", "rmp.cmd", "tools/rmp.py", "tools/install.sh",
                   "tools/install.ps1")

# install.sh may run these and nothing else: InstallTest's PATH is one folder
# holding exactly them and a python3.
INSTALL_TOOLS = ("git", "uname", "mkdir", "rmdir")


def git_identity(tmp: Path) -> dict:
    (tmp / "gitconfig").write_text("[init]\n\tdefaultBranch = main\n")
    return {"GIT_CONFIG_GLOBAL": str(tmp / "gitconfig"), "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_TERMINAL_PROMPT": "0", "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
            "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}


def git_in(where: Path, env: dict, *argv) -> str:
    got = subprocess.run(["git", "-C", str(where), *argv], capture_output=True, text=True,
                         env=dict(os.environ, **env))
    if got.returncode != 0:
        raise AssertionError(f"git {' '.join(argv)}: {got.stderr}")
    return got.stdout.strip()


def make_framework_source(src: Path, env: dict) -> Path:
    for rel in INSTALLED_FILES:
        (src / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / rel, src / rel)
    (src / rmp.FRAMEWORK_MARKER).parent.mkdir(parents=True, exist_ok=True)
    (src / rmp.FRAMEWORK_MARKER).write_text("")
    (src / rmp.TOML).write_text('[project]\nname = "demo"\n')
    git_in(src, env, "init", "-q", "-b", "main")
    git_in(src, env, "add", "-A")
    git_in(src, env, "commit", "-q", "-m", "framework")
    return src


def tool_folder(where: Path, drain=False, without=(), stubs=None, python=None) -> Path:
    """One folder to be the whole PATH. `drain`: every tool is a stand-in that
    reads ALL of its stdin before it does its job -- what raylib's headless
    getchar() did to the BSD jobs' script. `stubs`: {name: sh script}."""
    where.mkdir(parents=True)
    real = {name: shutil.which(name) for name in INSTALL_TOOLS}
    real["python3"] = python or sys.executable
    cat = shutil.which("cat")
    for name, target in real.items():
        if name in without:
            continue
        if drain:
            (where / name).write_text(f'#!/bin/sh\n"{cat}" > /dev/null\nexec "{target}" "$@"\n')
            (where / name).chmod(0o755)
        else:
            (where / name).symlink_to(target)
    for name, text in (stubs or {}).items():
        (where / name).write_text(text)
        (where / name).chmod(0o755)
    return where


def home_snapshot(home: Path) -> dict:
    """Everything under HOME, except what any git fetch rewrites."""
    out = {}
    for path in sorted(home.rglob("*")):
        rel = path.relative_to(home).as_posix()
        if rel.startswith(".local/share/rmp/.git/") or "__pycache__" in rel:
            continue
        if path.is_symlink():
            out[rel] = "-> " + os.readlink(path)
        elif path.is_file():
            out[rel] = path.read_bytes()
        else:
            out[rel] = "folder"
    return out


class InstallFixture(unittest.TestCase):
    """A framework to install from, made once per class: a git repository on
    main holding what installing needs, from this working tree."""

    @classmethod
    def setUpClass(cls):
        if os.name == "nt":
            raise unittest.SkipTest("install.sh is POSIX; the Windows job runs install.ps1")
        if shutil.which("git") is None:
            raise unittest.SkipTest("git not installed")
        cls._tmp = tempfile.TemporaryDirectory()
        cls.tmp = Path(cls._tmp.name)
        cls.git_env = git_identity(cls.tmp)
        cls.src = make_framework_source(cls.tmp / "src", cls.git_env)
        cls.bin = tool_folder(cls.tmp / "bin")
        cls.count = 0

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def fresh(self, name="home") -> Path:
        type(self).count += 1
        home = self.tmp / f"{name}{self.count}"
        home.mkdir()
        return home

    def env_for(self, home: Path, path=None, **extra) -> dict:
        env = dict(self.git_env, HOME=str(home), PATH=str(path or self.bin), SHELL="/bin/bash",
                   LANG="C", RMP_INSTALL_SOURCE=str(self.src))
        env.update({k: v for k, v in extra.items() if v is not None})
        return env

    def install(self, home: Path, shell="sh", path=None, script=None, piped=False, cwd=None,
                **extra) -> subprocess.CompletedProcess:
        """install.sh read from stdin: `sh -s < file`, or (piped) through a
        pipe, which is what `curl | sh` hands the shell."""
        data = (script or INSTALL_SH).read_bytes()
        argv = [shutil.which(shell) or shell, "-s"]
        env = self.env_for(home, path, **extra)
        if piped:
            return subprocess.run(argv, input=data, capture_output=True, env=env,
                                  cwd=cwd or self.tmp, timeout=120)
        with open(script or INSTALL_SH, "rb") as stdin:
            return subprocess.run(argv, stdin=stdin, capture_output=True, env=env,
                                  cwd=cwd or self.tmp, timeout=120)

    @staticmethod
    def said(got) -> str:
        return got.stdout.decode(errors="replace") + got.stderr.decode(errors="replace")

    def clone_of(self, home: Path) -> Path:
        return home / ".local" / "share" / "rmp"


class InstallTest(InstallFixture):
    """`curl -fsSL .../install.sh | sh`, end to end, into a temporary HOME."""

    shells = LauncherTest.shells

    def test_it_clones_links_and_puts_it_on_path_under_every_shell(self):
        for shell in self.shells():
            with self.subTest(shell=shell):
                home = self.fresh()
                got = self.install(home, shell=shell)
                self.assertEqual(got.returncode, 0, self.said(got))
                clone = self.clone_of(home)
                self.assertEqual(git_in(clone, self.git_env, "rev-parse", "HEAD"),
                                 git_in(self.src, self.git_env, "rev-parse", "HEAD"))
                link = home / ".local" / "bin" / "rmp"
                self.assertTrue(link.is_symlink())
                self.assertEqual(Path(os.readlink(link)), clone.resolve() / "rmp")
                ran = subprocess.run([str(link), "help"], cwd=home, capture_output=True,
                                     text=True, env=dict(os.environ, HOME=str(home)))
                self.assertTrue(ran.stdout.startswith("usage: rmp"), ran.stderr)
                rc, _ = rmp.rc_file(home, {"SHELL": "/bin/bash"}, platform_system())
                self.assertEqual(rc.read_text().count(rmp.RC_BEGIN), 1)
                self.assertIn("linked", self.said(got))

    def test_a_second_run_changes_nothing(self):
        home = self.fresh()
        self.assertEqual(self.install(home).returncode, 0)
        before = home_snapshot(home)
        again = self.install(home)
        self.assertEqual(again.returncode, 0, self.said(again))
        self.assertEqual(home_snapshot(home), before)
        self.assertIn("up to date", self.said(again))
        self.assertIn("already adds", self.said(again))

    def test_with_local_bin_on_path_no_startup_file_is_touched(self):
        home = self.fresh()
        got = self.install(home, path=f"{home}/.local/bin{os.pathsep}{self.bin}")
        self.assertEqual(got.returncode, 0, self.said(got))
        self.assertEqual(sorted(p.name for p in home.iterdir()), [".local"])
        self.assertIn("is on PATH", self.said(got))

    def test_each_login_shell_gets_the_file_it_reads(self):
        cases = [
            ("/bin/bash", {}, None, ".bashrc" if platform_system() != "Darwin"
             else ".bash_profile", "sh"),
            ("/usr/bin/zsh", {}, None, ".zshrc", "sh"),
            ("/usr/bin/zsh", {"ZDOTDIR": "{home}/zdot"}, None, "zdot/.zshrc", "sh"),
            ("/usr/bin/fish", {}, None, ".config/fish/conf.d/rmp.fish", "fish"),
            ("/usr/bin/fish", {"XDG_CONFIG_HOME": "{home}/cfg"}, None,
             "cfg/fish/conf.d/rmp.fish", "fish"),
            ("/bin/csh", {}, None, ".cshrc", "csh"),
            ("/bin/tcsh", {}, None, ".cshrc", "csh"),
            ("/bin/tcsh", {}, ".tcshrc", ".tcshrc", "csh"),
            ("/bin/ksh", {}, None, ".profile", "sh"),
            ("/bin/mksh", {"ENV": "$HOME/.mkshrc"}, None, ".mkshrc", "sh"),
            ("/bin/dash", {}, None, ".profile", "sh"),
            ("/bin/sh", {}, None, ".profile", "sh"),
            (None, {}, None, ".profile", "sh"),
        ]
        for shell, extra, existing, want, flavour in cases:
            with self.subTest(shell=shell, extra=extra, existing=existing):
                home = self.fresh()
                if existing:
                    (home / existing).write_text("# mine\n")
                extra = {k: v.replace("{home}", str(home)) for k, v in extra.items()}
                env = dict(extra, SHELL=shell or "")
                got = self.install(home, **env)
                self.assertEqual(got.returncode, 0, self.said(got))
                rc = home / want
                text = rc.read_text()
                self.assertEqual(text.count(rmp.RC_BEGIN), 1)
                self.assertIn(f"{rmp.RC_BEGIN}\n{rmp.RC_LINES[flavour]}\n{rmp.RC_END}\n", text)
                if existing:
                    self.assertTrue(text.startswith("# mine\n"))
                written = [p for p in home.rglob("*") if p.is_file()
                           and ".local" not in p.relative_to(home).parts]
                self.assertEqual(written, [rc])

    def test_the_line_puts_rmp_on_path_once_however_often_it_is_read(self):
        home = self.fresh()
        self.assertEqual(self.install(home, SHELL="/bin/sh").returncode, 0)
        rc = home / ".profile"
        for shell in self.shells():
            with self.subTest(shell=shell):
                got = subprocess.run([shutil.which(shell), "-c",
                                      '. "$1"; . "$1"; command -v rmp; echo "$PATH"', "sh",
                                      str(rc)], capture_output=True, text=True,
                                     env={"HOME": str(home), "PATH": str(self.bin)})
                found, path = got.stdout.splitlines()
                self.assertEqual(found, f"{home}/.local/bin/rmp")
                self.assertEqual(path.split(":").count(f"{home}/.local/bin"), 1)
        for shell, line in (("fish", rmp.RC_LINES["fish"]), ("tcsh", rmp.RC_LINES["csh"]),
                            ("csh", rmp.RC_LINES["csh"])):
            if shutil.which(shell) is None:
                continue
            with self.subTest(shell=shell):
                got = subprocess.run([shutil.which(shell), "-c", f"{line}\necho $PATH"],
                                     capture_output=True,
                                     text=True, env={"HOME": str(home), "PATH": str(self.bin)})
                self.assertTrue(got.stdout.startswith(f"{home}/.local/bin"), got.stdout)

    def test_from_inside_a_game_it_installs_and_nothing_is_delegated(self):
        home = self.fresh()
        game = DelegationTest.game_with_recording_rmp(self, str(self.tmp / f"g{self.count}"))
        got = self.install(home, cwd=game)
        self.assertEqual(got.returncode, 0, self.said(got))
        self.assertNotIn('"argv"', self.said(got))
        again = subprocess.run([str(self.clone_of(home) / "rmp"), "install"], cwd=game,
                               capture_output=True, text=True, env=self.env_for(home))
        self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
        self.assertNotIn('"argv"', again.stdout)
        self.assertIn("is this framework's rmp", again.stdout)

    def assert_refused_and_nothing_installed(self, home, got, *says):
        self.assertEqual(got.returncode, 1, self.said(got))
        for text in says:
            self.assertIn(text, self.said(got))
        self.assertFalse(self.clone_of(home).exists(), "it cloned anyway")
        self.assertEqual(list(home.iterdir()), [], "it wrote into HOME anyway")

    def test_without_git_it_says_how_to_get_it_and_clones_nothing(self):
        bare = tool_folder(self.tmp / f"nogit{self.count}", without=("git",))
        home = self.fresh()
        self.assert_refused_and_nothing_installed(home, self.install(home, path=bare),
                                                  "rmp needs git")
        apt = tool_folder(self.tmp / f"nogit-apt{self.count}", without=("git",),
                          stubs={"apt-get": "#!/bin/sh\nexit 0\n", "sudo": "#!/bin/sh\nexit 0\n"})
        home = self.fresh()
        self.assert_refused_and_nothing_installed(home, self.install(home, path=apt),
                                                  "  sudo apt install git\n")

    def test_an_old_python_is_refused_by_name_and_nothing_is_cloned(self):
        # The real Python, told it is 3.9: it answers whatever it is asked the
        # way a 3.9 would, so a script that stopped asking would be let in.
        old = ("#!/bin/sh\n"
               'if [ "$1" = -c ]; then\n'
               f'    exec "{sys.executable}" -c \'import sys; code = sys.argv.pop(1); '
               'sys.version_info = (3, 9, 6, "final", 0); exec(code)\' "$2"\n'
               "fi\n"
               f'exec "{sys.executable}" "$@"\n')
        folder = tool_folder(self.tmp / f"py39-{self.count}", without=("python3",),
                             stubs={"python3": old, "dnf": "#!/bin/sh\nexit 0\n"})
        home = self.fresh()
        self.assert_refused_and_nothing_installed(
            home, self.install(home, path=folder), "Python 3.11 or newer",
            "python3 (3.9) is older", "Install it, as root, with:\n  dnf install python3.11\n")
        nothing = tool_folder(self.tmp / f"nopy-{self.count}", without=("python3",))
        home = self.fresh()
        self.assert_refused_and_nothing_installed(home, self.install(home, path=nothing),
                                                  "there is none on PATH")

    def test_a_windows_shell_is_sent_to_install_ps1(self):
        for system in ("MINGW64_NT-10.0-19045", "MSYS_NT-10.0-19045", "CYGWIN_NT-10.0"):
            with self.subTest(system=system):
                folder = tool_folder(self.tmp / f"win{self.count}", without=("uname",),
                                     stubs={"uname": f"#!/bin/sh\necho {system}\n"})
                home = self.fresh()
                self.assert_refused_and_nothing_installed(
                    home, self.install(home, path=folder),
                    "irm https://omardev29.github.io/rmp-docs/install.ps1 | iex")

    def strip_dev_null(self) -> Path:
        text = INSTALL_SH.read_text()
        stripped, count = re.subn(r"[ \t]*<[ \t]*/dev/null", "", text)
        self.assertGreaterEqual(count, 8, "the script stopped redirecting its commands' stdin")
        copy = self.tmp / f"stripped{self.count}.sh"
        copy.write_text(stripped + "echo RMP_INSTALL_TAIL\n")
        return copy

    def test_a_line_after_the_script_still_runs(self):
        """In `curl | sh` the script is the shell's stdin, and a command that
        reads stdin eats the rest of it. Every tool here reads ALL of its
        stdin, so the line appended after `main "$@"` prints only if each of
        them was handed /dev/null. Seen red: the same script without its
        redirections, under bash, which leaves the rest of a script on stdin
        for whoever reads it next (dash reads ahead into its own buffer and
        hides the mistake, which is why bash is the shell that must fail)."""
        drained = tool_folder(self.tmp / f"drain{self.count}", drain=True)
        copy = self.tmp / f"tail{self.count}.sh"
        copy.write_text(INSTALL_SH.read_text() + "echo RMP_INSTALL_TAIL\n")
        for shell in self.shells():
            for piped in (False, True):
                with self.subTest(shell=shell, piped=piped):
                    home = self.fresh()
                    got = self.install(home, shell=shell, path=drained, script=copy, piped=piped)
                    self.assertEqual(got.returncode, 0, self.said(got))
                    self.assertTrue(got.stdout.decode().endswith("RMP_INSTALL_TAIL\n"),
                                    self.said(got))
                    self.assertTrue((home / ".local" / "bin" / "rmp").is_symlink())
        red = self.strip_dev_null()
        for piped in (False, True):
            with self.subTest(red="bash", piped=piped):
                home = self.fresh()
                got = self.install(home, shell="bash", path=drained, script=red, piped=piped)
                self.assertNotIn("RMP_INSTALL_TAIL", self.said(got))

    def test_an_existing_clone_is_updated_and_not_cloned_again(self):
        home = self.fresh()
        self.assertEqual(self.install(home).returncode, 0)
        before = git_in(self.clone_of(home), self.git_env, "rev-parse", "--short", "HEAD")
        (self.src / "NEWS").write_text("one more\n")
        git_in(self.src, self.git_env, "add", "NEWS")
        git_in(self.src, self.git_env, "commit", "-q", "-m", "news")
        try:
            got = self.install(home)
            self.assertEqual(got.returncode, 0, self.said(got))
            after = git_in(self.clone_of(home), self.git_env, "rev-parse", "--short", "HEAD")
            self.assertEqual(git_in(self.clone_of(home), self.git_env, "rev-parse", "HEAD"),
                             git_in(self.src, self.git_env, "rev-parse", "HEAD"))
            self.assertIn(f"updated  {before} -> {after}, 1 commit(s)", self.said(got))
            self.assertNotIn("cloning", self.said(got))
        finally:
            git_in(self.src, self.git_env, "reset", "-q", "--hard", "HEAD~1")

    def test_a_folder_in_the_way_is_left_alone(self):
        home = self.fresh()
        dest = self.clone_of(home)
        dest.mkdir(parents=True)
        (dest / "mine.txt").write_text("keep me\n")
        got = self.install(home)
        self.assertEqual(got.returncode, 1, self.said(got))
        self.assertIn("is not a clone of the framework", self.said(got))
        self.assertEqual(os.listdir(dest), ["mine.txt"])
        self.assertFalse((home / ".local" / "bin").exists())
        (dest / "mine.txt").unlink()   # an empty folder is no obstacle
        got = self.install(home)
        self.assertEqual(got.returncode, 0, self.said(got))

    def test_what_a_build_still_needs_is_named_with_this_systems_command(self):
        yes, no = "#!/bin/sh\nexit 0\n", "#!/bin/sh\nexit 1\n"
        cases = [
            ({"apt-get": yes, "sudo": yes, "pkg-config": no},
             "this machine still needs: CMake, Ninja, a C++ compiler, the X11 development "
             "headers.\nInstall them with:\n  sudo apt install cmake ninja-build g++ libx11-dev "
             "libxrandr-dev libxi-dev libxcursor-dev libxinerama-dev libgl1-mesa-dev\n"),
            ({"pacman": yes, "doas": yes, "pkg-config": yes, "c++": yes},
             "still needs: CMake, Ninja.\nInstall them with:\n  doas pacman -S --needed cmake "
             "ninja\n"),
            ({"zypper": yes, "cmake": yes, "ninja": yes, "g++": yes, "pkg-config": no},
             "still needs: the X11 development headers.\nInstall them, as root, with:\n"
             "  zypper install libX11-devel libXrandr-devel libXi-devel libXcursor-devel "
             "libXinerama-devel Mesa-libGL-devel\n"),
            ({"uname": "#!/bin/sh\necho FreeBSD\n", "pkg-config": no, "cmake": yes},
             "still needs: Ninja, a C++ compiler, the X11 development headers.\nInstall them, "
             "as root, with:\n  pkg install ninja libX11 libXrandr libXi libXcursor "
             "libXinerama libglvnd mesa-libs\n"),
            ({"pkg-config": no}, "still needs: CMake, Ninja, a C++ compiler, the X11 "
             "development headers.\nInstall them with your system's package manager.\n"),
            ({"cmake": yes, "ninja-build": yes, "clang++": yes, "pkg-config": yes},
             "\nEverything a build needs is here: CMake, Ninja and a C++ compiler.\n"),
        ]
        for stubs, says in cases:
            with self.subTest(stubs=sorted(stubs)):
                folder = tool_folder(self.tmp / f"needs{self.count}",
                                     without=("uname",) if "uname" in stubs else (), stubs=stubs)
                type(self).count += 1
                home = self.fresh()
                got = self.install(home, path=folder)
                self.assertEqual(got.returncode, 0, self.said(got))
                self.assertTrue(got.stdout.decode().endswith(says), self.said(got))


def platform_system() -> str:
    import platform
    return platform.system()


class InstallerContractTest(unittest.TestCase):
    """The installers are served from the docs site at FRAMEWORK_REF, and the
    clone they make tracks main: what they ask of rmp has to keep working on
    both sides of that gap. And what they say is checked like the launchers."""

    def calls(self, pattern, text):
        return {m.group(1) for m in re.finditer(pattern, text)}

    def test_they_run_rmp_for_install_and_update_and_nothing_else(self):
        sh = self.calls(r'"\$dest/rmp" ([a-z-]+)', INSTALL_SH.read_text())
        ps1 = self.calls(r"& \$rmp ([a-z-]+)", INSTALL_PS1.read_text())
        self.assertEqual(sh, {"install", "update"})
        self.assertEqual(ps1, {"install", "update"})
        for name in sh:
            with self.subTest(command=name):
                spec = rmp.COMMANDS[name]
                self.assertFalse(spec.needs_project)
                self.assertFalse(spec.framework_only)
                self.assertTrue(all(w.startswith("[") for w in spec.usage.split()[1:]),
                                f"`rmp {name}` grew a required argument the installers do "
                                "not pass")
                self.assertIn(name, rmp.GLOBAL_COMMANDS)

    def test_the_default_source_is_the_frameworks_url_and_main(self):
        sh = INSTALL_SH.read_text()
        ps1 = INSTALL_PS1.read_text()
        self.assertEqual(re.search(r"(?m)^    url=(\S+)$", sh).group(1), rmp.FRAMEWORK_URL)
        self.assertIn("branch=${RMP_INSTALL_BRANCH:-main}", sh)
        self.assertIn("source=${RMP_INSTALL_SOURCE:-$url}", sh)
        self.assertEqual(re.search(r"\$url = '([^']+)'", ps1).group(1), rmp.FRAMEWORK_URL)
        self.assertIn("else { 'main' }", ps1)

    def test_they_look_for_python_like_the_launchers(self):
        launcher = re.search(r"for py in ([^;]+);", (REPO / "rmp").read_text()).group(1)
        self.assertEqual(re.search(r"for py in ([^;]+);", INSTALL_SH.read_text()).group(1),
                         launcher)
        pattern = r"foreach \(\$candidate in ([^)]+)\)"
        self.assertEqual(re.search(pattern, INSTALL_PS1.read_text()).group(1),
                         re.search(pattern, (REPO / "rmp.ps1").read_text()).group(1))

    def test_install_sh_is_ascii_lf_posix_and_one_function(self):
        data = INSTALL_SH.read_bytes()
        self.assertTrue(data.isascii())
        self.assertNotIn(b"\r", data)
        self.assertTrue(data.startswith(b"#!/bin/sh\n"))
        eol = subprocess.run(["git", "-c", "safe.directory=*", "check-attr", "eol", "--",
                              "tools/install.sh"], cwd=REPO, capture_output=True, text=True)
        self.assertIn("eol: lf", eol.stdout)
        # Nothing runs before the last line has arrived: outside main() there
        # are comments, and the one call at the very end.
        lines = data.decode().splitlines()
        start = lines.index("main() {")
        end = lines.index("}", start)
        outside = [line for line in lines[:start] + lines[end + 1:]
                   if line.strip() and not line.startswith("#")]
        self.assertEqual(outside, ['main "$@"'])
        self.assertEqual(lines[-1], 'main "$@"')
        self.assertTrue(all(line == "" or line.startswith("    ") for line in lines[start + 1:end]),
                        "a line inside main() is not indented: is main() closed early?")

    def test_every_shell_parses_install_sh(self):
        for shell in LauncherTest.shells(self):
            with self.subTest(shell=shell):
                got = subprocess.run([shell, "-n", str(INSTALL_SH)], capture_output=True,
                                     text=True)
                self.assertEqual(got.returncode, 0, got.stderr)

    def test_shellcheck_reads_install_sh_as_posix_sh(self):
        if shutil.which("shellcheck") is None:
            if IN_BUILD_IMAGE:
                self.fail("shellcheck is missing inside the build image")
            self.skipTest("shellcheck not installed")
        got = subprocess.run(["shellcheck", "-s", "sh", str(INSTALL_SH)], capture_output=True,
                             text=True)
        self.assertEqual(got.returncode, 0, got.stdout)

    def test_install_ps1_is_ascii_crlf_and_never_exits(self):
        """Under `irm | iex` an exit closes the window the user typed into."""
        data = INSTALL_PS1.read_bytes()
        self.assertTrue(data.isascii())
        self.assertFalse(data.startswith(b"\xef\xbb\xbf"))
        eol = subprocess.run(["git", "-c", "safe.directory=*", "check-attr", "eol", "--",
                              "tools/install.ps1"], cwd=REPO, capture_output=True, text=True)
        self.assertIn("eol: crlf", eol.stdout)
        if b"\r\n" in data:   # a checkout made with the attribute
            self.assertEqual(data.count(b"\n"), data.count(b"\r\n"))
        code = [line for line in data.decode().splitlines()
                if line.strip() and not line.lstrip().startswith("#")]
        # The statement, not Python's sys.exit( inside the probe's string.
        exits = [line for line in code if re.search(r"(?i)(?<![\w.$-])exit\b(?!\s*\()", line)]
        self.assertEqual(exits, [])
        self.assertEqual((code[0].strip(), code[-1].strip()), ("& {", "}"),
                         "all of it is one script block, so iex leaves no variable behind")
        self.assertGreaterEqual(sum("throw " in line for line in code), 4)

    def test_pwsh_parses_install_ps1_when_there(self):
        pwsh = shutil.which("pwsh")
        if pwsh is None:
            self.skipTest("pwsh not installed; the Windows job runs it for real")
        got = subprocess.run([pwsh, "-NoProfile", "-NonInteractive", "-Command",
                              "$t = $null; $e = $null; [void][System.Management.Automation."
                              f"Language.Parser]::ParseFile('{INSTALL_PS1}', [ref]$t, [ref]$e); "
                              "$e.Count"], capture_output=True, text=True)
        self.assertEqual(got.stdout.strip(), "0", got.stdout + got.stderr)


class InstallPs1UnderIexTest(InstallFixture):
    """install.ps1 the way `irm | iex` runs it, where a pwsh is at hand. On
    Linux a stand-in rmp.cmd (an executable sh script) records what it was
    asked; the Windows job runs the real one, the registry and cmd.exe."""

    @classmethod
    def setUpClass(cls):
        if shutil.which("pwsh") is None:
            raise unittest.SkipTest("pwsh not installed; the Windows job runs install.ps1")
        super().setUpClass()
        fake = cls.src / "rmp.cmd"
        fake.write_text('#!/bin/sh\necho "rmp.cmd $*" >> "$LOCALAPPDATA/calls.txt"\n')
        fake.chmod(0o755)
        git_in(cls.src, cls.git_env, "add", "-A")
        git_in(cls.src, cls.git_env, "commit", "-q", "-m", "a stand-in rmp.cmd")

    def iex(self, local: Path, path=None):
        env = dict(os.environ, **self.git_env, LOCALAPPDATA=str(local),
                   RMP_INSTALL_SOURCE=str(self.src))
        if path:
            env["PATH"] = path
        return subprocess.run([shutil.which("pwsh"), "-NoProfile", "-NonInteractive", "-Command",
                               f"Get-Content -Raw '{INSTALL_PS1}' | Invoke-Expression; "
                               "'RMP_AFTER_IEX'"], capture_output=True, text=True, env=env,
                              timeout=180)

    def test_it_returns_to_its_caller_and_runs_install_then_update(self):
        local = self.fresh("local")
        got = self.iex(local)
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertTrue(got.stdout.rstrip().endswith("RMP_AFTER_IEX"), got.stdout)
        self.assertTrue((local / "rmp" / ".git").is_dir())
        self.assertEqual((local / "calls.txt").read_text(), "rmp.cmd install\n")
        again = self.iex(local)
        self.assertTrue(again.stdout.rstrip().endswith("RMP_AFTER_IEX"), again.stdout)
        self.assertEqual((local / "calls.txt").read_text(),
                         "rmp.cmd install\nrmp.cmd update\nrmp.cmd install\n")

    def test_without_git_it_throws_and_clones_nothing(self):
        local = self.fresh("local")
        folder = tool_folder(self.tmp / f"ps-nogit{self.count}", without=("git",))
        got = self.iex(local, path=f"{folder}{os.pathsep}{Path(shutil.which('pwsh')).parent}")
        self.assertNotEqual(got.returncode, 0)
        self.assertNotIn("RMP_AFTER_IEX", got.stdout)
        self.assertIn("winget install --id Git.Git -e", got.stdout)
        self.assertFalse((local / "rmp").exists())


class UpdateTest(unittest.TestCase):
    """`rmp update`, on a clone made by git: main moves forward to origin's,
    or nothing moves and it says why."""

    def setUp(self):
        if shutil.which("git") is None:
            self.skipTest("git not installed")
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.env = git_identity(self.tmp)
        self.src = make_framework_source(self.tmp / "src", self.env)
        git_in(self.tmp, self.env, "clone", "-q", str(self.src), "clone")
        self.clone = self.tmp / "clone"

    def tearDown(self):
        self._tmp.cleanup()

    def update(self, clone=None):
        return subprocess.run([sys.executable, str((clone or self.clone) / "tools" / "rmp.py"),
                               "update"], cwd=self.tmp, capture_output=True, text=True,
                              env=dict(os.environ, **self.env), stdin=subprocess.DEVNULL)

    def head(self, where=None):
        return git_in(where or self.clone, self.env, "rev-parse", "HEAD")

    def commit_in(self, where, name="NEWS"):
        (where / name).write_text("more\n")
        git_in(where, self.env, "add", name)
        git_in(where, self.env, "commit", "-q", "-m", name)

    def refused(self, *says):
        before = self.head()
        got = self.update()
        self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
        for text in says:
            self.assertIn(text, got.stdout)
        self.assertEqual(self.head(), before, "it moved anyway")

    def test_up_to_date_says_so(self):
        got = self.update()
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        short = git_in(self.clone, self.env, "rev-parse", "--short", "HEAD")
        self.assertIn(f"up to date at {short}", got.stdout)

    def test_a_new_commit_on_origin_is_a_fast_forward_and_says_from_what_to_what(self):
        before = git_in(self.clone, self.env, "rev-parse", "--short", "HEAD")
        self.commit_in(self.src)
        self.commit_in(self.src, "MORE")
        got = self.update()
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertEqual(self.head(), self.head(self.src))
        after = git_in(self.clone, self.env, "rev-parse", "--short", "HEAD")
        self.assertIn(f"updated  {before} -> {after}, 2 commit(s)", got.stdout)
        self.assertEqual(len(git_in(self.clone, self.env, "log", "--merges", "--oneline")), 0)

    def test_changes_of_its_own_are_refused(self):
        self.commit_in(self.src)
        (self.clone / "rmp.cmd").write_text("changed\n")
        self.refused("changes of its own", "rmp.cmd", "stash")

    def test_an_untracked_file_is_no_change(self):
        self.commit_in(self.src)
        (self.clone / "notes.txt").write_text("mine\n")
        got = self.update()
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)
        self.assertEqual(self.head(), self.head(self.src))

    def test_another_branch_or_no_branch_is_refused(self):
        self.commit_in(self.src)
        git_in(self.clone, self.env, "switch", "-q", "-c", "topic")
        self.refused("is on topic", "only moves main", "switch main")
        git_in(self.clone, self.env, "switch", "-q", "--detach", "main")
        self.refused("a detached HEAD")

    def test_no_origin_is_refused(self):
        git_in(self.clone, self.env, "remote", "remove", "origin")
        self.refused("no origin", f"remote add origin {rmp.FRAMEWORK_URL}")

    def test_commits_origin_does_not_have_are_never_merged(self):
        self.commit_in(self.clone, "LOCAL")
        self.refused("1 commit(s) that origin's main does not", "never merges")
        self.commit_in(self.src)    # diverged, not only ahead
        self.refused("never merges")

    def test_an_unreachable_origin_is_refused(self):
        git_in(self.clone, self.env, "remote", "set-url", "origin", str(self.tmp / "gone"))
        self.refused("could not fetch main from origin")

    def test_a_copy_that_is_not_a_clone_is_refused(self):
        copy = self.tmp / "copy"
        shutil.copytree(self.clone, copy, ignore=shutil.ignore_patterns(".git"))
        got = self.update(copy)
        self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
        self.assertIn("is not a git clone", got.stdout)

    def test_a_games_copy_refuses_install_and_update(self):
        """A game keeps the framework it was made with: its own tools/rmp.py
        never moves it, and never puts itself on PATH."""
        game = fake_project(self.tmp / "game")
        shutil.copy2(RMP_PY, game / "tools" / "rmp.py")
        for command in ("update", "install"):
            with self.subTest(command=command):
                got = subprocess.run([sys.executable, str(game / "tools" / "rmp.py"), command],
                                     cwd=game, capture_output=True, text=True,
                                     env=dict(os.environ, HOME=str(self.tmp / "home")))
                self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
                self.assertIn("belongs to the game", got.stdout)
                self.assertFalse((self.tmp / "home").exists())


class GlobalCommandsTest(unittest.TestCase):
    """new, install and update are run by the rmp they are typed into, never
    handed to the game's copy that a framework rmp hands everything else to."""

    def test_they_are_three_and_need_no_project(self):
        self.assertEqual(rmp.GLOBAL_COMMANDS, ("new", "install", "update"))
        for name in rmp.GLOBAL_COMMANDS:
            with self.subTest(command=name):
                self.assertFalse(rmp.COMMANDS[name].needs_project)
                self.assertFalse(rmp.COMMANDS[name].framework_only)

    def test_none_of_them_is_delegated_inside_a_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = DelegationTest.game_with_recording_rmp(self, tmp)
            for name in rmp.GLOBAL_COMMANDS:
                with self.subTest(command=name):
                    got = DelegationTest.run_in(self, game, name, "--help")
                    self.assertNotIn('"argv"', got.stdout)
                    self.assertTrue(got.stdout.startswith(f"rmp {rmp.COMMANDS[name].usage}"),
                                    got.stdout + got.stderr)
            got = DelegationTest.run_in(self, game, "build", "--help")
            self.assertIn('"argv"', got.stdout, "and everything else still is")


@contextlib.contextmanager
def environment(**values):
    """os.environ with these set (None: removed), for an in-process main()."""
    saved = dict(os.environ)
    try:
        for key, value in values.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        yield
    finally:
        os.environ.clear()
        os.environ.update(saved)


class InstallCommandTest(unittest.TestCase):
    """`rmp install` in-process, run by this framework: the POSIX link and the
    rc file, and the Windows shim and User PATH with the registry stood in."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.home = self.tmp / "home"
        self.home.mkdir()
        self.link = self.home / ".local" / "bin" / "rmp"

    def tearDown(self):
        self._tmp.cleanup()

    def run_install(self, *args, path=None, shell="/bin/zsh"):
        with environment(HOME=str(self.home), PATH=path or "/usr/bin:/bin", SHELL=shell,
                         ZDOTDIR=None, ENV=None, XDG_CONFIG_HOME=None):
            return call(["install", *args])

    def test_it_links_this_frameworks_launcher(self):
        code, out, err = self.run_install()
        self.assertEqual(code, 0, out + err)
        self.assertEqual(Path(os.readlink(self.link)), REPO / "rmp")
        self.assertIn("linked   ~/.local/bin/rmp ->", out)
        self.assertEqual((self.home / ".zshrc").read_text().count(rmp.RC_BEGIN), 1)
        self.assertIn("open a new terminal", out)

    def test_something_else_in_the_way_is_refused_and_force_replaces_it(self):
        self.link.parent.mkdir(parents=True)
        for kind in ("file", "link", "dangling", "loop"):
            with self.subTest(kind=kind):
                if self.link.is_symlink() or self.link.exists():
                    self.link.unlink()
                if kind == "file":
                    self.link.write_text("#!/bin/sh\necho mine\n")
                elif kind == "link":
                    self.link.symlink_to(self.tmp / "other")
                    (self.tmp / "other").write_text("x")
                elif kind == "dangling":
                    self.link.symlink_to(self.tmp / "gone")
                else:   # Python 3.12's resolve() raises on it
                    self.link.symlink_to(self.tmp / "loop")
                    (self.tmp / "loop").symlink_to(self.link)
                code, out, _ = self.run_install()
                self.assertEqual(code, 1, out)
                self.assertIn("install force", out)
                self.assertNotEqual(os.readlink(self.link) if self.link.is_symlink() else "",
                                    str(REPO / "rmp"))
                code, out, _ = self.run_install("force")
                self.assertEqual(code, 0, out)
                self.assertEqual(Path(os.readlink(self.link)), REPO / "rmp")

    def test_a_link_that_ends_at_this_launcher_is_already_installed(self):
        self.link.parent.mkdir(parents=True)
        hop = self.tmp / "hop"
        hop.symlink_to(REPO / "rmp")
        self.link.symlink_to(hop)
        code, out, _ = self.run_install()
        self.assertEqual(code, 0, out)
        self.assertIn("is this framework's rmp", out)
        self.assertEqual(os.readlink(self.link), str(hop))

    def test_on_path_already_it_writes_no_rc_file_and_names_a_shadowing_rmp(self):
        other = self.tmp / "first"
        other.mkdir()
        (other / "rmp").write_text("#!/bin/sh\n")
        (other / "rmp").chmod(0o755)
        bin_dir = self.home / ".local" / "bin"
        loop = self.tmp / "loop"    # a PATH entry Python 3.12 cannot resolve
        loop.symlink_to(loop)
        code, out, _ = self.run_install(path=f"{loop}{os.pathsep}{other}{os.pathsep}{bin_dir}/")
        self.assertEqual(code, 0, out)
        self.assertEqual([p.name for p in self.home.iterdir()], [".local"])
        self.assertIn(f"another rmp comes first on PATH: {other / 'rmp'}", out)
        code, out, _ = self.run_install(path=str(bin_dir))
        self.assertNotIn("another rmp", out)
        self.assertIn("rmp is ready", out)

    def test_the_rc_block_goes_in_once_and_after_what_is_there(self):
        rc = self.tmp / "rc"
        rc.write_text("alias x=y")     # no newline at the end
        self.assertTrue(rmp.add_rc_block(rc, "sh"))
        self.assertFalse(rmp.add_rc_block(rc, "sh"))
        self.assertEqual(rc.read_text(), f"alias x=y\n\n{rmp.RC_BEGIN}\n{rmp.RC_LINES['sh']}\n"
                                         f"{rmp.RC_END}\n")
        real = self.tmp / "dotfiles" / "zshrc"
        real.parent.mkdir()
        real.write_text("# mine\n")
        linked = self.tmp / ".zshrc"
        linked.symlink_to(real)
        self.assertTrue(rmp.add_rc_block(linked, "sh"))
        self.assertTrue(linked.is_symlink(), "the dotfiles link was replaced by a file")
        self.assertIn(rmp.RC_BEGIN, real.read_text())

    def test_a_bash_on_a_mac_keeps_the_file_its_login_already_reads(self):
        home = self.tmp / "mac"
        home.mkdir()
        self.assertEqual(rmp.rc_file(home, {"SHELL": "/bin/bash"}, "Darwin")[0],
                         home / ".bash_profile")
        for name in (".profile", ".bash_login", ".bash_profile"):
            (home / name).write_text("")
            with self.subTest(exists=name):
                self.assertEqual(rmp.rc_file(home, {"SHELL": "/bin/bash"}, "Darwin")[0],
                                 home / name)
        self.assertEqual(rmp.rc_file(home, {"SHELL": "/usr/local/bin/bash"}, "Linux")[0],
                         home / ".bashrc")
        self.assertEqual(rmp.rc_file(home, {"SHELL": "/bin/ksh", "ENV": "relative"}, "OpenBSD"),
                         (home / ".profile", "sh"))
        self.assertEqual(rmp.rc_file(home, {"SHELL": "/bin/ksh", "ENV": "~/.kshrc"}, "OpenBSD"),
                         (home / ".kshrc", "sh"))

    @contextlib.contextmanager
    def registry(self, value="", kind=2):
        state = {"value": value, "kind": kind, "writes": [], "broadcasts": 0}
        saved = (rmp.user_path_read, rmp.user_path_write, rmp.broadcast_environment,
                 rmp.on_windows)

        def write(new, new_kind):
            state["writes"].append((new, new_kind))
            state["value"], state["kind"] = new, new_kind

        def broadcast():
            state["broadcasts"] += 1
        rmp.user_path_read = lambda: (state["value"], state["kind"])
        rmp.user_path_write = write
        rmp.broadcast_environment = broadcast
        rmp.on_windows = lambda: True
        try:
            yield state
        finally:
            (rmp.user_path_read, rmp.user_path_write, rmp.broadcast_environment,
             rmp.on_windows) = saved

    def run_windows(self, *args, local=None):
        local = local or str(self.tmp / "Local")
        with environment(LOCALAPPDATA=local, HOME=str(self.home)):
            return call(["install", *args])

    def test_windows_writes_a_cmd_shim_and_appends_its_folder_to_the_user_path(self):
        folder = self.tmp / "Local" / "Programs" / "rmp"
        for kind in (2, 1):   # REG_EXPAND_SZ, REG_SZ: kept as they were
            with self.subTest(kind=kind), self.registry(r"%USERPROFILE%\bin;C:\tools", kind) as reg:
                code, out, _ = self.run_windows()
                self.assertEqual(code, 0, out)
                self.assertEqual(reg["writes"], [(rf"%USERPROFILE%\bin;C:\tools;{folder}", kind)])
                self.assertEqual(reg["broadcasts"], 1)
                shim = (folder / "rmp.cmd").read_bytes()
                self.assertEqual(shim, rmp.shim_text(REPO).encode("ascii"))
                self.assertIn(f'call "{REPO / "rmp.cmd"}" %*\r\n'.encode(), shim)
                self.assertTrue(shim.endswith(b"exit /b %ERRORLEVEL%\r\n"))
                self.assertNotIn(b"\n", shim.replace(b"\r\n", b""))
                self.assertFalse(self.link.exists(), "the POSIX link on Windows")
                code, out, _ = self.run_windows()
                self.assertEqual(len(reg["writes"]), 1, "a second run wrote the PATH again")
                self.assertEqual(reg["broadcasts"], 1)
                self.assertIn("is on your user PATH", out)
            (folder / "rmp.cmd").unlink()

    def test_windows_knows_its_folder_however_the_path_spells_it(self):
        local = str(self.tmp / "Local")
        folder = f"{local}/Programs/rmp"
        for spelling in (folder.upper(), folder + "\\", folder.replace("/", "\\"),
                         "%LOCALAPPDATA%/Programs/rmp", f'"{folder}"'):
            with self.subTest(spelling=spelling), self.registry(f"C:\\x;{spelling};") as reg:
                code, out, _ = self.run_windows(local=local)
                self.assertEqual(code, 0, out)
                self.assertEqual(reg["writes"], [])
        with self.registry("") as reg:
            self.run_windows(local=local)
            self.assertEqual(reg["writes"], [(str(Path(folder)), 2)])

    def test_windows_refuses_another_shim_without_force_and_a_missing_localappdata(self):
        folder = self.tmp / "Local" / "Programs" / "rmp"
        folder.mkdir(parents=True)
        (folder / "rmp.cmd").write_text("@echo off\r\necho mine\r\n")
        with self.registry() as reg:
            code, out, _ = self.run_windows()
            self.assertEqual(code, 1, out)
            self.assertIn("install force", out)
            self.assertEqual(reg["writes"], [])
            code, out, _ = self.run_windows("force")
            self.assertEqual(code, 0, out)
            self.assertEqual((folder / "rmp.cmd").read_bytes().decode("ascii"),
                             rmp.shim_text(REPO))
        with self.registry() as reg, environment(LOCALAPPDATA=None):
            code, out, _ = call(["install"])
            self.assertEqual(code, 1)
            self.assertIn("LOCALAPPDATA is not set", out)

    def test_a_shim_names_its_folder_by_variable_and_refuses_what_cmd_cannot_read(self):
        """cmd.exe reads a .cmd in the console's code page: an accented user
        name has to reach it through %LOCALAPPDATA%, not spelled out."""
        user = "/x/Jos\u00e9"
        with environment(LOCALAPPDATA=f"{user}/Local", USERPROFILE=user):
            text = rmp.shim_text(Path(f"{user}/Local/rmp"))
            self.assertIn('call "%LOCALAPPDATA%/rmp/rmp.cmd" %*', text)
            self.assertTrue(text.isascii())
            self.assertIn('call "%USERPROFILE%/src/rmp/rmp.cmd" %*',
                          rmp.shim_text(Path(f"{user}/src/rmp")))
            with self.assertRaises(rmp.Refused):
                rmp.shim_text(Path("/elsewhere/Jos\u00e9/rmp"))
        with environment(LOCALAPPDATA="/x/Local", USERPROFILE=None):
            # A name that only starts like the folder is another folder.
            self.assertIn('call "/x/LocalOther/rmp/rmp.cmd" %*',
                          rmp.shim_text(Path("/x/LocalOther/rmp")))


class InstallerCiTest(unittest.TestCase):
    """The installers run for real where they are meant to: install.ps1 under
    iex in the Windows job, install.sh under /bin/sh and zsh on the Mac. Both
    steps are the framework's: a game has no installer of its own."""

    def step(self, workflow, job, needle):
        steps = load_workflow(self, workflow)["jobs"][job]["steps"]
        found = [s for s in steps if needle in str(s.get("run", ""))]
        self.assertEqual(len(found), 1, f"{workflow}: {job} has no step running {needle}")
        self.assertTrue(gated(found[0].get("if")), f"{workflow}: the {needle} step is not gated")
        return code_lines(found[0]["run"])

    def test_apple_is_told_which_it_is(self):
        apple = load_workflow(self, "_apple.yml")
        on = apple.get("on", apple.get(True))
        spec = on["workflow_call"]["inputs"]["framework"]
        self.assertEqual((spec["type"], spec["required"], "default" in spec),
                         ("boolean", True, False))
        ci = load_workflow(self, "ci.yml")["jobs"]["apple"]["with"]["framework"]
        self.assertEqual(ci, "${{ needs.config.outputs.framework == 'true' }}")
        canary = load_workflow(self, "canary.yml")["jobs"]["apple"]["with"]["framework"]
        self.assertIs(canary, True)

    def test_the_mac_runs_install_sh_under_sh_and_zsh_from_stdin(self):
        run = self.step("_apple.yml", "macos", "tools/install.sh")
        for needle in ("/bin/sh", "zsh", "RMP_INSTALL_TAIL", "RMP_INSTALL_SOURCE", "HOME=",
                       "git archive", ".bash_profile", ".zshrc", "help"):
            with self.subTest(needle=needle):
                self.assertIn(needle, run)

    def test_windows_runs_install_ps1_under_iex_in_both_powershells(self):
        run = self.step("_windows.yml", "rmp", "install.ps1")
        for needle in ("Invoke-Expression", "RMP_AFTER_IEX", "'powershell', 'pwsh'",
                       "LOCALAPPDATA", "RMP_INSTALL_SOURCE", "git archive",
                       "HKCU:\\Environment", "GetValueKind", "cmd /c rmp help", "where rmp"):
            with self.subTest(needle=needle):
                self.assertIn(needle, run)


if __name__ == "__main__":
    unittest.main()
