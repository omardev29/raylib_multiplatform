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
    """A git repository in a temp folder, with a bare one as its origin."""

    def __init__(self, tmp: Path):
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
            "sys.exit(int(os.environ.get('FAKE_CONFIGURE_EXIT', '0')))\n")
        (self.root / ".gitignore").write_text("tools/calls.json\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "first")
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
            if path.startswith("thirdparty/") or path == "tests/rmp_test.py":
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
                          "thirdparty/doctest", "tests/configure_test.py", "tests/rmp_test.py",
                          "tests/fixtures", "canary.yml", "autofix.yml",
                          "tools/naming_check.sh"):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, have)
        self.assertFalse((self.game / "LICENSE").exists(), "a root LICENSE makes GitHub call it MIT")
        self.assertTrue((self.game / "tests" / "smoke_test.h").is_file())

    def test_untransformed_files_are_byte_for_byte_the_frameworks(self):
        changed = {rmp.TOML, "THIRD_PARTY_LICENSES.md", "thirdparty/FROZEN_VERSIONS.md"}
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
        for table in ("project", "window", "android", "ios", "resources"):
            mine[table] = dict(mine[table])
        for key in ("name",):
            mine["project"][key] = base["project"][key]
        mine["window"]["title"] = base["window"]["title"]
        mine["android"]["application_id"] = base["android"]["application_id"]
        mine["ios"]["bundle_id"] = base["ios"]["bundle_id"]
        mine["resources"]["rres_password"] = base["resources"]["rres_password"]
        self.assertEqual(mine, base)
        self.assertEqual(len((self.game / rmp.TOML).read_text().splitlines()),
                         len((REPO / rmp.TOML).read_text().splitlines()))

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

    def test_the_licences_credit_the_framework_and_not_doctest(self):
        lic = (self.game / "THIRD_PARTY_LICENSES.md").read_text()
        self.assertNotIn("doctest", lic)
        self.assertRegex(lic, r"(?m)^raylib_multiplatform +thirdparty/raylib_multiplatform +MIT")
        self.assertTrue((self.game / "thirdparty" / "raylib_multiplatform" / "LICENSE").is_file())
        self.assertNotIn("doctest", (self.game / "thirdparty" / "FROZEN_VERSIONS.md").read_text())

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


if __name__ == "__main__":
    unittest.main()
