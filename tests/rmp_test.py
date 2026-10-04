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


def job_block(path: Path, job: str) -> str:
    text = path.read_text()
    m = re.search(rf"(?m)^  {re.escape(job)}:\n(.*?)(?=^  [A-Za-z_-]+:\n|\Z)", text, re.S)
    return m.group(1) if m else ""


class StagesAgreeWithLintTest(unittest.TestCase):
    """Every stage of `rmp test` runs in the CI lint job too: a stage that
    exists in one place and not the other is how the UI layout test ran on
    laptops only, for four phases."""

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
        lint = job_block(REPO / ".github" / "workflows" / "ci.yml", "lint")
        self.assertIn("actionlint", lint, "the lint job did not parse")
        for stage in rmp.STAGES:
            if stage.name in ("examples", "render-update"):
                continue  # the examples job, and a recording -- not a check
            for token in self.tokens(stage):
                with self.subTest(stage=stage.name, token=token):
                    self.assertTrue(token in lint, f"`rmp test {stage.name}` runs {token} "
                                    "and the CI lint job does not")


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
        mode = subprocess.run(["git", "ls-files", "-s", "rmp"], cwd=REPO,
                              capture_output=True, text=True).stdout.split()[:1]
        if mode:   # tracked
            self.assertEqual(mode[0], "100755")
        eol = subprocess.run(["git", "check-attr", "eol", "--", "rmp"], cwd=REPO,
                             capture_output=True, text=True).stdout
        self.assertIn("eol: lf", eol)

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
