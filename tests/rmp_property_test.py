"""tools/rmp.py held to its promises for ANY command line, environment and
answer from the programs it starts.

`rmp` is the one command, and the first thing a newcomer types: its promise
(the docstring) is an exit code -- 0 done, 1 refused, 2 a wrong command line,
130 interrupted, and the game's own when it started one -- and a sentence,
never a traceback (R7 of the deep campaign). These run main() as the program
does, on a machine made up for each example:

  - a framework and a game in a temporary folder, or no project at all;
  - a home folder, a /dev and an environment of that folder's own: RMP_DRM,
    SUDO_USER, SHELL, DISPLAY, EMSDK, LOCALAPPDATA..., values a real one has
    and values it should not;
  - every program rmp starts -- git, gh, cmake, configure.py, the game --
    answered with an exit code and an output Hypothesis picks, gh's included
    with JSON of any shape (rmp says it survives an answer that is not JSON);
  - stdout as a UTF-8 terminal has it, strict, and stderr backslashreplace;
    argv as Python decodes bytes that are not UTF-8 (surrogateescape).

and with the rules CLAUDE.md states as rules: rmp never runs sudo unless
`sudo` was typed; under sudo it refuses every command but help and clean
before running anything; `rmp push` does not push over a CI run in flight.

Then the manifest `rmp new` copies from: classify() is the longest pattern
that matches, and a game gets exactly the INCLUDE paths -- no FRAMEWORK_ONLY
one, whatever the framework tracks. And `rmp install`'s line in a startup
file goes in once, after what is there.

Every property was seen red; the commit that added this file says how, and
each counterexample is a named case in tests/rmp_test.py.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hypothesis_profiles  # noqa: E402  (first: the profile, or the skip)
from hypothesis import given, settings, strategies as st  # noqa: E402

# As `tools.rmp`: the name mutmut gives the file (tools/mutation.sh).
_spec = importlib.util.spec_from_file_location("tools.rmp",
                                               hypothesis_profiles.REPO / "tools" / "rmp.py")
rmp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rmp)

FRAMEWORK = Path(rmp.__file__).resolve().parents[1]

# ---------------------------------------------------------------------------
# What a person types
# ---------------------------------------------------------------------------

COMMAND_NAMES = sorted(rmp.COMMANDS)
STAGE_NAMES = sorted({s.name for s in rmp.STAGES})
WORDS = ["release", "debug", "force", "sudo", "check", "fix", "write", "list", "--list",
         "--json", "-h", "--help", "--mode", "help", "1.2.0", "v1.2.0", "my_game", "01_pong",
         "games/01_pong", "examples/games/01_pong/", "ui", "..", ".", "/", "", " ", "-"]

# What sys.argv can hold: no NUL (a C string ends there), and a byte that is
# not UTF-8 as Python decodes it, a lone surrogate.
TYPED = st.text(alphabet=st.characters(exclude_characters="\0", exclude_categories=("Cs",)),
                max_size=12)
UNDECODABLE = st.binary(min_size=1, max_size=6).map(
    lambda b: b.decode("utf-8", "surrogateescape"))
WORD = st.one_of(st.sampled_from(COMMAND_NAMES + STAGE_NAMES + WORDS), TYPED, UNDECODABLE)
ARGV = st.one_of(
    st.tuples(st.sampled_from(COMMAND_NAMES), st.lists(WORD, max_size=3)).map(
        lambda t: [t[0], *t[1]]),
    st.lists(WORD, max_size=4))

# ---------------------------------------------------------------------------
# What the programs rmp starts answer
# ---------------------------------------------------------------------------

# Mostly success, so that a command gets past its first program to the next.
CODES = st.sampled_from([0, 0, 0, 0, 0, 1, 2, 127, 128])
# A game's exit status: its own, or a signal's (negative, as subprocess says).
GAME_CODES = st.sampled_from([0, 0, 1, 3, 139, -11, -9, 255])
# What a program prints: text, without the NUL no tool rmp reads ever writes.
SHORT = st.text(alphabet=st.characters(exclude_characters="\0"), max_size=40)
JSON_VALUES = st.recursive(
    st.none() | st.booleans() | st.integers() | st.text(max_size=8),
    lambda inner: st.lists(inner, max_size=3) | st.dictionaries(st.text(max_size=8), inner,
                                                                max_size=3),
    max_leaves=8)
RUN = st.fixed_dictionaries({}, optional={
    "databaseId": st.integers(1, 10**11) | st.text(max_size=6),
    "status": st.sampled_from(["completed", "in_progress", "queued", "requested", "waiting",
                               "pending"]) | st.text(max_size=6),
    "event": st.sampled_from(["push", "pull_request", "workflow_dispatch"])})
GH_LIST = st.one_of(st.lists(RUN, max_size=3).map(json.dumps), JSON_VALUES.map(json.dumps),
                    SHORT)
# What `git ls-files -s -z` prints: mode, sha, stage, a tab and the path. The
# paths, the framework's real ones and others nobody classified.
TRACKED = st.one_of(
    st.sampled_from(["CMakeLists.txt", "src/main.cpp", "tools/rmp.py", "tools/fuzz.sh",
                     "tests/game/main_menu_test.cpp", "tests/rmp_test.py", "README.md",
                     "LICENSE", "thirdparty/rapidcheck/README.md", "thirdparty/raylib-ios",
                     rmp.TOML, "examples/games/01_pong/src/main.cpp"]),
    st.text(alphabet="abcdefgh/._-", min_size=1, max_size=20))
LS_FILES = st.lists(st.tuples(st.sampled_from(["100644", "100755", "160000"]), TRACKED),
                    max_size=6).map(
    lambda entries: "".join(f"{mode} {'a' * 40} 0\t{path}\0" for mode, path in entries))
NAMES = st.sampled_from(["game", "demo", "my_game", ""]) | SHORT


def answers_for(argv: list[str]):
    """(exit code, stdout) of one program rmp starts, as a strategy."""
    program = Path(argv[0]).name
    if program == "gh":
        return st.tuples(st.sampled_from([0, 0, 1, 4]), GH_LIST)
    if program == "git" and "ls-files" in argv:
        return st.tuples(st.just(0), LS_FILES)
    if program == "git" and "--show-toplevel" in argv:
        # Never the framework: with it, `rmp new` would copy the real one.
        return st.tuples(CODES, st.sampled_from(["", "/somewhere/else\n"]) | SHORT)
    if "--print-name" in argv:
        return st.tuples(st.sampled_from([0, 0, 1]), NAMES)
    if "--print-ci" in argv:
        return st.tuples(st.sampled_from([0, 0, 1]),
                         st.sampled_from(["on_push=true\n", "on_push=false\n", ""]) | SHORT)
    return st.tuples(CODES, SHORT)


# ---------------------------------------------------------------------------
# A machine for one example
# ---------------------------------------------------------------------------

SHELLS = ["", "/bin/sh", "/bin/bash", "/usr/bin/zsh", "/usr/bin/fish", "/bin/csh",
          "/bin/tcsh", "/bin/ksh", "/bin/mksh", "nushell"]


@st.composite
def machines(draw):
    """Everything about the machine main() runs on that is not the command
    line: where it is started, the environment (folders in it are the
    example's own, or names that are not folders), what is installed and who
    runs it."""
    return {
        "where": draw(st.sampled_from(["framework", "framework", "game", "deep in the game",
                                       "nowhere"])),
        "env": draw(st.fixed_dictionaries({}, optional={
            "RMP_DRM": st.sampled_from(["", "0", "1", "2", "yes", " 1"]),
            "SUDO_USER": st.sampled_from(["", "root", "omar"]),
            "SHELL": st.sampled_from(SHELLS),
            "ENV": st.sampled_from(["", "~/.kshrc", "$HOME/.mkshrc", "${HOME}/x", "rel"]),
            "DISPLAY": st.sampled_from(["", ":0"]),
            "WAYLAND_DISPLAY": st.sampled_from(["", "wayland-0"]),
            "XDG_SESSION_TYPE": st.sampled_from(["", "tty", "x11", "wayland"]),
            "SSH_CONNECTION": st.sampled_from(["", "10.0.0.1 22 10.0.0.2 22"]),
            "EMSDK": st.sampled_from(["", "{tmp}/emsdk", "/nonexistent/emsdk"]),
            "LOCALAPPDATA": st.sampled_from(["", "{tmp}/local"]),
            "XDG_CONFIG_HOME": st.sampled_from(["", "{tmp}/config", "relative"]),
            "ZDOTDIR": st.sampled_from(["", "{tmp}/zdot"]),
            "PATH": st.sampled_from(["/usr/bin:/bin", "{tmp}/home/.local/bin:/usr/bin", ""]),
            "USER": TYPED,
        })),
        "system": draw(st.sampled_from(["Linux", "Linux", "Darwin", "Windows", "FreeBSD",
                                        "MSYS_NT-10.0-19045"])),
        "windows": draw(st.booleans()),
        "terminal": draw(st.sampled_from([None, "/dev/tty1", "/dev/pts/3", "/dev/ttyS0"])),
        "euid": draw(st.sampled_from([1000, 1000, 0])),
        # Each there more often than not: a missing tool ends a command early.
        "installed": {tool for tool in ("git", "gh", "clang-format", "bash", "sudo", "rmp")
                      if draw(st.sampled_from([True, True, True, False]))},
        "cards": draw(st.sampled_from([(), ("card0",), ("card1", "card0")])),
        "master": draw(st.sampled_from([None, True, False])),
        "access": draw(st.booleans()),
    }


class Machine:
    """main() on a made-up machine: the projects, the folders, the doors to
    the real one stood in for, and every program it starts recorded and
    answered by `data`."""

    def __init__(self, data, spec, answers=None):
        self.data = data
        self.spec = spec
        # A program's answers of the test's own choosing: by program name, or
        # a function of the whole command line (None: the usual answer).
        self.answers = answers or {}
        self.calls: list[tuple[list[str], int, str]] = []
        self.envs: list[dict] = []

    @contextlib.contextmanager
    def up(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.ExitStack() as stack:
            root = Path(tmp)
            framework = self.project(root / "framework", framework=True)
            game = self.project(root / "game", framework=False)
            (game / "src" / "scenes").mkdir(parents=True)
            (root / "home").mkdir()
            (root / "emsdk").mkdir()
            dev = root / "dev"
            (dev / "dri").mkdir(parents=True)
            (dev / "input").mkdir()
            for card in self.spec["cards"]:
                (dev / "dri" / card).write_text("")
            (dev / "input" / "event0").write_text("")
            self.where = {"framework": framework, "game": game, "nowhere": root,
                          "deep in the game": game / "src" / "scenes"}[self.spec["where"]]
            env = {"HOME": str(root / "home")}
            for key, value in self.spec["env"].items():
                env[key] = value.replace("{tmp}", tmp)
            installed = self.spec["installed"]
            previous = os.getcwd()
            os.chdir(self.where)
            stack.callback(os.chdir, previous)
            for target, value in (
                    ("system_name", lambda: self.spec["system"]),
                    ("on_windows", lambda: self.spec["windows"]),
                    ("terminal_name", lambda: self.spec["terminal"]),
                    ("effective_uid", lambda: self.spec["euid"]),
                    ("drm_master", lambda card: self.spec["master"]),
                    ("node_access", lambda path, mode: self.spec["access"]),
                    ("DEV", dev)):
                stack.enter_context(unittest.mock.patch.object(rmp, target, value))
            stack.enter_context(unittest.mock.patch.object(
                rmp.TerminalState, "save", classmethod(lambda cls, fd=0: None)))
            stack.enter_context(unittest.mock.patch(
                "shutil.which", lambda name, *a, **k: f"/usr/bin/{name}" if name in installed
                else None))
            stack.enter_context(unittest.mock.patch("subprocess.run", self.run))
            stack.enter_context(unittest.mock.patch.dict(os.environ, env, clear=True))
            yield

    @staticmethod
    def project(folder: Path, framework: bool) -> Path:
        folder.mkdir(parents=True)
        (folder / rmp.TOML).write_text('[project]\nname = "demo"\n')
        (folder / "tools").mkdir()
        if framework:
            (folder / rmp.FRAMEWORK_MARKER).parent.mkdir()
            (folder / rmp.FRAMEWORK_MARKER).write_text("")
            # 01_menu twice: a bare name that is not unique.
            for example in ("games/01_pong", "ui/01_menu", "games/01_menu"):
                main = folder / "examples" / example / "src" / "main.cpp"
                main.parent.mkdir(parents=True)
                main.write_text("")
        return folder

    def run(self, argv, *args, **kwargs):
        argv = [str(a) for a in argv]
        program = Path(argv[0]).name
        game = program in ("demo", "game", "my_game") or program.startswith("example_") \
            or argv[:2] == ["sudo", "--"]
        chosen = self.answers(argv) if callable(self.answers) else self.answers.get(program)
        if chosen is not None:
            code, out = self.data.draw(chosen, label=" ".join(argv))
        elif game:
            code, out = self.data.draw(st.tuples(GAME_CODES, SHORT), label=" ".join(argv))
        else:
            code, out = self.data.draw(answers_for(argv), label=" ".join(argv))
        self.calls.append((argv, code, out))
        self.envs.append(dict(kwargs.get("env") or {}))
        captured = kwargs.get("capture_output") or kwargs.get("stdout") == subprocess.PIPE
        return subprocess.CompletedProcess(argv, code, out if captured else None,
                                           "" if captured else None)


def terminal():
    """stdout and stderr as Python opens them on a UTF-8 terminal: strict, and
    backslashreplace."""
    out = io.TextIOWrapper(io.BytesIO(), encoding="utf-8", errors="strict")
    err = io.TextIOWrapper(io.BytesIO(), encoding="utf-8", errors="backslashreplace")
    return out, err


def run_main(machine: Machine, argv: list[str]):
    """main(argv) the way `python3 tools/rmp.py` runs it: (code, stdout, stderr)."""
    out, err = terminal()
    with machine.up(), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rmp.streams()
        code = rmp.main(list(argv), cwd=machine.where)
        sys.stdout.flush()
        sys.stderr.flush()
    return code, out.buffer.getvalue().decode("utf-8"), err.buffer.getvalue().decode("utf-8")


def starts_a_game(argv) -> bool:
    return bool(argv) and argv[0] in ("run", "example")


def vocabulary(name: str) -> list[str]:
    """The words a command takes, as its own help writes them: its usage and
    every example line -- and the stages, for `rmp test`."""
    command = rmp.COMMANDS[name]
    words = [w.strip("[]") for w in command.usage.split()[1:]]
    for line, _note in command.examples:
        parts = line.split()
        if parts[:2] == ["rmp", name]:
            words += parts[2:]
    return sorted(set(words) | (set(STAGE_NAMES) if name == "test" else set()))


def holds_its_exit_promise(case, argv, machine, code, out, err):
    # The game's own status passes through `rmp run` and `rmp example`: a
    # crash is not a success. Whatever answered, then.
    started = {c for _a, c, _o in machine.calls} if starts_a_game(argv) else set()
    case.assertIn(code, {0, 1, 2, 130} | started,
                  f"{argv!r}\nstdout:\n{out}\nstderr:\n{err}")
    if code == 2:
        case.assertTrue(err.strip(), f"{argv!r}: a usage error that says nothing")


class MainIsTotalTest(unittest.TestCase):
    """main() returns 0, 1, 2 or 130 -- or the game's own status, for a
    command that started one -- for any command line on any machine, and
    lets nothing else out. Once over any command line, and once a command,
    with the words its own help shows beside any others: a search spread over
    every command at once reaches the second program of none of them."""

    @given(ARGV, machines(), st.data())
    def test_any_command_line(self, argv, spec, data):
        machine = Machine(data, spec)
        holds_its_exit_promise(self, argv, machine, *run_main(machine, argv))


def _command_test(name: str):
    words = st.lists(st.sampled_from(vocabulary(name) or ["-"]) | WORD, max_size=3)

    # A quarter of the profile's examples each: 18 commands at the full 200
    # would be most of a minute on every push.
    @settings(max_examples=max(25, settings.default.max_examples // 4))
    @given(words, machines(), st.data())
    def test(self, rest, spec, data):
        argv = [name, *rest]
        machine = Machine(data, spec)
        holds_its_exit_promise(self, argv, machine, *run_main(machine, argv))
    test.__name__ = f"test_rmp_{name}"
    test.__doc__ = f"The command {name!r}, and any words after it."
    return test


for _name in COMMAND_NAMES:
    setattr(MainIsTotalTest, f"test_rmp_{_name}", _command_test(_name))


class StreamsTest(unittest.TestCase):
    """What rmp prints reaches the terminal, whatever it is: a word typed in
    bytes that are not UTF-8 comes into argv as a lone surrogate, and rmp
    says it back -- `rmp deploy` names the tag it would make. A strict UTF-8
    stdout, which is what Python opens on a UTF-8 terminal, raises on it, and
    the sentence that should have said FAIL was a traceback instead."""

    @given(st.text(alphabet=st.characters(exclude_characters="\0"), max_size=20) | UNDECODABLE)
    def test_anything_printed_is_written(self, text):
        out, err = terminal()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rmp.streams()
            print(f"FAIL: {text}")
            sys.stdout.flush()
        written = out.buffer.getvalue().decode("utf-8")
        self.assertTrue(written.startswith("FAIL: "), written)
        self.assertTrue(written.endswith("\n"), written)


class LiveRunsTest(unittest.TestCase):
    """live_runs() asks gh which CI runs a push would cancel, and survives any
    answer: it says so and pushes unchecked when gh fails or answers what is
    not JSON -- and JSON of another shape than a list of runs is the same
    case. An answer that is a list of runs gives exactly the ones not
    completed."""

    @given(st.sampled_from([0, 0, 1, 4]), GH_LIST)
    def test_any_answer_from_gh(self, code, answer):
        with tempfile.TemporaryDirectory() as tmp, \
                unittest.mock.patch("shutil.which", lambda name, *a, **k: f"/usr/bin/{name}"), \
                unittest.mock.patch.object(rmp.Ctx, "run", lambda self, argv, **k:
                                           subprocess.CompletedProcess(argv, code, answer, "")), \
                contextlib.redirect_stdout(io.StringIO()) as said:
            ctx = rmp.Ctx(Path(tmp))
            live = rmp.live_runs(ctx)
        self.assertIsInstance(live, list)
        try:
            runs = json.loads(answer or "[]")   # nothing at all is no run
        except json.JSONDecodeError:
            runs = None
        if code == 0 and isinstance(runs, list) and all(isinstance(r, dict) for r in runs):
            self.assertEqual(live, [r for r in runs if r.get("status") != "completed"])
        else:
            self.assertEqual(live, [])
            self.assertIn("pushing without checking", said.getvalue())


class NewListTest(unittest.TestCase):
    """`rmp new --list` prints what a new game gets, for whatever the
    framework's index holds -- or refuses, naming the file nobody classified,
    as `rmp new` itself does. The manifest is the thing a framework developer
    is asked to update, and the command that shows it was a traceback the
    moment it was out of date."""

    @given(LS_FILES)
    def test_the_manifest_or_the_path_it_lacks(self, index):
        entries = [entry.split("\t", 1)[1] for entry in index.split("\0") if entry]
        out, err = terminal()
        with unittest.mock.patch("subprocess.run", lambda argv, *a, **k:
                                 subprocess.CompletedProcess(argv, 0, index, "")), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rmp.streams()
            code = rmp.main(["new", "--list"], cwd=FRAMEWORK)
            sys.stdout.flush()
        said = out.buffer.getvalue().decode("utf-8")
        unclassified = [path for path in entries if rmp.classify(path) is None]
        if unclassified:
            self.assertEqual(code, rmp.FAILED, said)
            self.assertIn(f"{unclassified[0]} is not in rmp new's manifest", said)
        else:
            self.assertEqual(code, rmp.OK, said)
            self.assertEqual(said.splitlines(),
                             rmp.new_game_paths([("100644", "0" * 40, p) for p in entries]))


class SudoRulesTest(unittest.TestCase):
    """CLAUDE.md, on a Linux console: rmp never runs sudo unless `sudo` was
    typed, and never builds as root -- under sudo, every command but help and
    clean is refused in main(), before anything runs."""

    @given(st.sampled_from([["run"], ["run", "sudo"], ["example", "01_pong"],
                            ["example", "games/01_pong", "sudo"], ["test", "smoke"]]) | ARGV,
           machines(), st.sampled_from(["1", "1", ""]), st.data())
    def test_sudo_only_when_typed(self, argv, spec, drm, data):
        # Mostly a Linux console, where a game is started at all with sudo,
        # and builds that work, so that one is started.
        spec["system"] = "Linux"
        spec["env"]["RMP_DRM"] = drm
        machine = Machine(data, spec, answers={
            "cmake": st.tuples(st.sampled_from([0, 0, 0, 0, 0, 1]), SHORT)})
        run_main(machine, argv)
        if "sudo" not in argv:
            for called, _code, _out in machine.calls:
                self.assertNotEqual(Path(called[0]).name, "sudo", f"{argv!r} ran {called}")

    @given(ARGV, machines(), st.data())
    def test_under_sudo_nothing_runs(self, argv, spec, data):
        spec["euid"] = 0
        spec["env"]["SUDO_USER"] = "omar"
        machine = Machine(data, spec)
        code, out, _err = run_main(machine, argv)
        command = argv[0] if argv else "help"
        if command in rmp.COMMANDS and command not in rmp.ROOT_COMMANDS \
                and not any(a in rmp.HELP_FLAGS for a in argv[1:]):
            self.assertEqual(code, rmp.FAILED, f"{argv!r}\n{out}")
            self.assertEqual(machine.calls, [], f"{argv!r} ran something as root")


def push_answers(argv):
    words = " ".join(argv)
    if argv[0] == "gh":
        # gh answering, mostly, with runs.
        return st.tuples(st.sampled_from([0, 0, 0, 1]),
                         st.lists(RUN, max_size=3).map(json.dumps) | GH_LIST)
    if "--print-ci" in argv:
        return st.tuples(st.sampled_from([0, 0, 0, 1]),
                         st.sampled_from(["on_push=true\n", "on_push=false\n", "x\non_push=false\n",
                                          "on_push=false \n", ""]))
    if argv[0] != "git":
        return None
    if argv[1:3] == ["diff", "--quiet"]:
        return st.tuples(st.sampled_from([0, 1]), st.just(""))
    if "symbolic-ref" in words:
        return st.tuples(st.just(0), st.sampled_from(["main\n", "main\n", "feature\n", ""]))
    if argv[1:2] == ["config"]:
        return st.tuples(st.sampled_from([0, 1]), st.just(""))
    if argv[1:] == ["remote"]:
        return st.tuples(st.just(0), st.sampled_from(["origin\n", "origin\nfork\n", "fork\n",
                                                      ""]))
    return None


class PushTest(unittest.TestCase):
    """`rmp push` does not push while a CI run is in flight: pushing cancels
    it. Whatever gh answers, a push happens only where gh could not be asked,
    could not answer, or named no run that is not completed. And the first
    push of a branch sets its upstream on origin -- or, with no remote at
    all, says how to add one -- while any other push is a plain `git push`."""

    @given(st.sampled_from([["push"], ["push"], ["push", "force"]]), machines(), st.data())
    def test_no_push_over_a_live_run(self, argv, spec, data):
        spec.update(where="game", euid=1000)   # not under sudo: SudoRulesTest's
        spec["env"].pop("SUDO_USER", None)
        spec["installed"] |= {"git"}
        machine = Machine(data, spec, answers=push_answers)
        code, out, err = run_main(machine, argv)
        pushes = [a for a, _c, _o in machine.calls if a[:2] == ["git", "push"]]
        self.assertLessEqual(len(pushes), 1, pushes)
        # Whether gh is asked at all: a push that starts a CI run -- [ci]
        # on_push, or a ci.yml whose changes are not committed yet -- unless
        # forced; and configure refusing the project is no push.
        ci = answered(machine, "tools/configure.py", "--print-ci")
        if argv == ["push"]:
            self.assertIsNotNone(ci)
            if ci[0] != 0:
                self.assertEqual((code, pushes), (rmp.FAILED, []), out)
                return
            starts = "on_push=false" not in ci[1].splitlines() or \
                answered(machine, "diff", "--quiet", "HEAD")[0] != 0
            self.assertEqual(ran(machine, "gh", "run", "list") or "gh is not installed" in out,
                             starts, out)
        else:
            self.assertIsNone(ci)
        if pushes:
            branch = answered(machine, "symbolic-ref", "-q", "--short", "HEAD")[1].strip()
            config = answered(machine, "config", f"branch.{branch}.merge") if branch else None
            remotes = answered(machine, "git", "remote")
            if config is not None and config[0] != 0 and "origin" in remotes[1].split():
                self.assertEqual(pushes[0], ["git", "push", "-u", "origin", branch])
            else:
                self.assertEqual(pushes[0], ["git", "push"])
        elif ran(machine, "symbolic-ref") and code == rmp.FAILED and "no remote" in out:
            remotes = answered(machine, "git", "remote")
            self.assertEqual(remotes[1].split(), [], out)
        asked = [(c, o) for a, c, o in machine.calls if Path(a[0]).name == "gh"]
        if not pushes or argv == ["push", "force"] or not asked:
            return
        code, out = asked[0]
        try:
            runs = json.loads(out or "[]")   # nothing at all is no run
        except json.JSONDecodeError:
            return
        if code != 0 or not isinstance(runs, list) or not all(isinstance(r, dict) for r in runs):
            return   # nothing could be checked, and that is said (LiveRunsTest)
        live = [r for r in runs if r.get("status") != "completed"]
        self.assertEqual(live, [], f"pushed over {live} (gh answered {out!r})")


def answered(machine, *words):
    """(code, stdout) of the first call holding `words` in order, or None."""
    for argv, code, out in machine.calls:
        joined = " " + " ".join(argv) + " "
        if " " + " ".join(words) + " " in joined:
            return code, out
    return None


def ran(machine, *words) -> bool:
    return answered(machine, *words) is not None


# What git answers, one subcommand at a time, for deploy and update: mostly
# what lets the command go on, so that every check is the one that decides
# in some example.
COMMIT = st.sampled_from(["4f1c2a9\n", "4f1c2a9\n", "b07e3d1\n"])


def deploy_answers(argv):
    words = " ".join(argv)
    if argv[0] != "git":
        if "--print-config" in argv:
            return st.tuples(st.sampled_from([0, 0, 0, 1]), SHORT)
        return None
    if "diff --quiet" in words or "diff --cached --quiet" in words:
        return st.tuples(st.sampled_from([0, 0, 0, 0, 1]), st.just(""))
    if "--verify refs/tags/" in words:
        return st.tuples(st.sampled_from([1, 1, 1, 0]), st.just(""))
    if "ls-remote" in words:
        return st.tuples(st.sampled_from([2, 2, 2, 0, 128]), SHORT)
    if "--abbrev-ref HEAD" in words:
        return st.tuples(st.just(0), st.sampled_from(["main\n", "main\n", "release\n"]))
    if "-q --verify origin/" in words:
        return st.tuples(st.sampled_from([0, 0, 0, 1]), st.just(""))
    if argv[1:3] == ["rev-parse", "HEAD"] or (argv[1] == "rev-parse" and "origin/" in words):
        return st.tuples(st.just(0), COMMIT)
    if argv[1:3] == ["tag", "-a"]:
        return st.tuples(st.sampled_from([0, 0, 0, 1]), st.just(""))
    return st.tuples(st.sampled_from([0, 0, 0, 1]), SHORT)


class DeployTest(unittest.TestCase):
    """`rmp deploy VERSION` tags and pushes only a release that can ship
    (cmd_deploy's comments say why each): a tree with nothing uncommitted, a
    tag that exists neither here nor on origin -- whose `ls-remote
    --exit-code` says 2, "no such ref", and nothing else -- a branch origin
    has, at the commit HEAD is, and a configure that passes the tag CI will
    see, strict in a game. Whatever git answers, the tag is made exactly
    then, pushed exactly when made, and the exit says which."""

    @given(st.sampled_from(["1.2.0", "v1.2.0", "0.0.1", "v10.0.0-rc1"]) | TYPED.filter(bool),
           st.sampled_from(["framework", "game"]), machines(), st.data())
    def test_a_tag_only_for_what_can_ship(self, version, where, spec, data):
        spec.update(where=where, euid=1000)
        spec["env"].pop("SUDO_USER", None)
        spec["installed"] |= {"git"}
        machine = Machine(data, spec, answers=deploy_answers)
        code, out, err = run_main(machine, ["deploy", version])
        tag = version if version.startswith("v") else "v" + version
        head, upstream = answered(machine, "rev-parse", "HEAD"), None
        branch = answered(machine, "--abbrev-ref", "HEAD")
        if branch:
            upstream = answered(machine, "rev-parse", f"origin/{branch[1].strip()}")
        checks = [answered(machine, "diff", "--quiet"), answered(machine, "--cached", "--quiet"),
                  answered(machine, f"refs/tags/{tag}"),
                  answered(machine, "ls-remote", "--exit-code", "--tags", "origin", tag),
                  answered(machine, "-q", "--verify", f"origin/{branch[1].strip()}")
                  if branch else None]
        configure = answered(machine, "--print-config")
        ships = (None not in checks and checks[0][0] == 0 and checks[1][0] == 0
                 and checks[2][0] != 0 and checks[3][0] == 2 and checks[4][0] == 0
                 and head is not None and upstream is not None
                 and head[1].strip() == upstream[1].strip()
                 and configure is not None and configure[0] == 0)
        tagged = answered(machine, "tag", "-a", tag)
        self.assertEqual(tagged is not None, ships, f"{version!r}: {machine.calls}")
        pushed = answered(machine, "push", "origin", tag)
        self.assertEqual(pushed is not None, ships and tagged[0] == 0)
        self.assertEqual(code, 0 if pushed is not None and pushed[0] == 0 else 1, out + err)
        if configure is not None:
            argv, env = next((a, e) for (a, _c, _o), e in zip(machine.calls, machine.envs)
                             if "--print-config" in a)
            self.assertEqual((env.get("GITHUB_REF_TYPE"), env.get("GITHUB_REF_NAME")),
                             ("tag", tag))
            self.assertEqual("--strict-release" in argv, where == "game")
        for argv, _c, _o in machine.calls:
            self.assertNotIn(argv[1:2], (["reset"], ["stash"], ["checkout"]), argv)


def update_answers(argv):
    if argv[0] != "git":
        return None
    words = " ".join(argv)
    if "--show-toplevel" in words:
        return st.tuples(st.just(0), st.sampled_from([f"{FRAMEWORK}\n"] * 4 + ["/elsewhere\n"]))
    if "symbolic-ref" in words:
        return st.tuples(st.just(0), st.sampled_from(["main\n", "main\n", "main\n", "dev\n",
                                                      ""]))
    if argv[3:4] == ["remote"]:
        return st.tuples(st.just(0), st.sampled_from(["origin\n", "origin\n", "upstream\n",
                                                      ""]))
    if "status --porcelain" in words:
        return st.tuples(st.just(0), st.sampled_from(["", "", "", " M tools/rmp.py\n"]))
    if "rev-parse --short HEAD" in words:
        return st.tuples(st.just(0), COMMIT)
    if "rev-list --count" in words:
        return st.tuples(st.just(0), st.sampled_from(["0\n", "3\n"]))
    return st.tuples(st.sampled_from([0, 0, 0, 1]), SHORT)


class UpdateTest(unittest.TestCase):
    """`rmp update` moves the framework's own clone forward, and nothing
    else: it fast-forwards main from origin's main exactly when the clone is
    the framework's, on main, with an origin, nothing of its own changed,
    the fetch worked and HEAD is an ancestor of what came -- and it never
    merges otherwise, never resets, stashes or checks anything out."""

    @given(machines(), st.data())
    def test_a_fast_forward_or_nothing(self, spec, data):
        spec.update(euid=1000)
        spec["env"].pop("SUDO_USER", None)
        spec["installed"] |= {"git"}
        machine = Machine(data, spec, answers=update_answers)
        code, out, err = run_main(machine, ["update"])
        top = answered(machine, "--show-toplevel")
        branch = answered(machine, "symbolic-ref")
        remotes = answered(machine, str(FRAMEWORK), "remote")
        changed = answered(machine, "status", "--porcelain")
        fetched = answered(machine, "fetch", "--quiet", "origin", "main")
        ancestor = answered(machine, "merge-base", "--is-ancestor", "HEAD", "FETCH_HEAD")
        should = (top is not None and Path(top[1].strip()) == FRAMEWORK
                  and branch is not None and branch[1].strip() == "main"
                  and remotes is not None and "origin" in remotes[1].split()
                  and changed is not None and not changed[1].strip()
                  and fetched is not None and fetched[0] == 0
                  and ancestor is not None and ancestor[0] == 0)
        merged = answered(machine, "merge", "--ff-only", "--quiet", "FETCH_HEAD")
        self.assertEqual(merged is not None, should, f"{machine.calls}")
        self.assertEqual(code, 0 if merged is not None and merged[0] == 0 else 1, out + err)
        for argv, _c, _o in machine.calls:
            self.assertEqual(argv[:3], ["git", "-C", str(FRAMEWORK)], argv)
            self.assertNotIn(argv[3], ("reset", "stash", "checkout", "pull", "rebase", "push"))
            if argv[3] == "merge":
                self.assertIn("--ff-only", argv)


# ---------------------------------------------------------------------------
# rmp new: the manifest
# ---------------------------------------------------------------------------

PATTERNS = sorted(set(rmp.INCLUDE) | set(rmp.RENAME) | set(rmp.GITLINKS) |
                  set(rmp.FRAMEWORK_ONLY))
PIECE = st.text(alphabet="abcxyz_.-", min_size=1, max_size=6).filter(
    lambda piece: piece not in (".", ".."))
PATHS = st.one_of(
    # What RENAME renames, and where it puts it: a path the framework could
    # also track under that name.
    st.sampled_from(sorted(rmp.RENAME) + sorted(rmp.RENAME.values())),
    st.sampled_from(PATTERNS),
    st.builds(lambda p, rest: p + "/".join(rest),
              st.sampled_from([p for p in PATTERNS if p.endswith("/")]),
              st.lists(PIECE, min_size=1, max_size=3)),
    st.builds(lambda p, rest: p + rest, st.sampled_from(PATTERNS), PIECE),
    st.lists(PIECE, min_size=1, max_size=3).map("/".join))


def matches(path: str, pattern: str) -> bool:
    return path == pattern or (pattern.endswith("/") and path.startswith(pattern))


class ClassifyTest(unittest.TestCase):
    """The longest pattern that matches wins, from whichever list."""

    @given(PATHS)
    def test_the_longest_match_wins(self, path):
        lists = (("include", rmp.INCLUDE), ("rename", tuple(rmp.RENAME)),
                 ("gitlink", rmp.GITLINKS), ("framework", tuple(rmp.FRAMEWORK_ONLY)))
        hits = [(len(p), kind, p) for kind, patterns in lists for p in patterns
                if matches(path, p)]
        got = rmp.classify(path)
        if not hits:
            self.assertIsNone(got)
            return
        longest = max(length for length, _k, _p in hits)
        self.assertEqual([(k, p) for n, k, p in hits if n == longest], [got],
                         f"{path!r}: one longest pattern, and it is the one classify() gave")


class ManifestCopyTest(unittest.TestCase):
    """make_game copies what the manifest says a game gets, and nothing else:
    for any set of files the framework tracks, the game holds exactly the
    INCLUDE and RENAME ones, renamed, each byte for byte, the executable ones
    listed -- and no FRAMEWORK_ONLY path."""

    @given(st.lists(st.tuples(PATHS, st.sampled_from(["100644", "100755"])), max_size=12,
                    unique_by=lambda t: t[0]))
    def test_a_game_gets_the_manifest_and_nothing_else(self, files):
        files = [(path, mode) for path, mode in files
                 if not path.endswith("/") and rmp.classify(path) is not None
                 and rmp.classify(path)[0] != "gitlink"]
        # A file and a folder of one name cannot both be on disk.
        files = [(path, mode) for path, mode in files
                 if not any(other.startswith(path + "/") for other, _m in files)]
        copies = [rmp.RENAME.get(path, path) for path, _m in files
                  if rmp.classify(path)[0] in ("include", "rename")]
        with tempfile.TemporaryDirectory() as tmp:
            source, target = Path(tmp) / "framework", Path(tmp) / "game"
            entries = []
            for path, mode in files:
                (source / path).parent.mkdir(parents=True, exist_ok=True)
                (source / path).write_text(f"{path} {mode}\n")
                entries.append((mode, "0" * 40, path))
            target.mkdir()
            if len(set(copies)) != len(copies):
                # Two of the framework's files, one place in the game: which
                # one a game gets is a bug in the manifest, said so.
                with self.assertRaises(rmp.Refused):
                    rmp.copy_manifest(source, target, entries)
                return
            executable = rmp.copy_manifest(source, target, entries)
            copied = sorted(p.relative_to(target).as_posix() for p in target.rglob("*")
                            if p.is_file())
            want = sorted(set(rmp.new_game_paths(entries)) - {"README.md", rmp.GITLINKS[0]})
            self.assertEqual(copied, want)
            for path, mode in files:
                if rmp.classify(path)[0] == "framework":
                    self.assertNotIn(path, copied)
                    continue
                dest = rmp.RENAME.get(path, path)
                self.assertEqual((target / dest).read_text(), f"{path} {mode}\n")
                self.assertEqual(dest in executable, mode == "100755", dest)


# ---------------------------------------------------------------------------
# rmp new: the lines of the .toml it rewrites
# ---------------------------------------------------------------------------

KEY = st.from_regex(r"[a-z_]{1,12}", fullmatch=True)
PAD = st.sampled_from(["", " ", "  ", "\t"])
COMMENT = st.text(alphabet=st.characters(exclude_characters="\r\n#"), max_size=15)
QUOTED = st.text(alphabet=st.characters(exclude_characters='"\r\n'), max_size=12)


class TomlLineTest(unittest.TestCase):
    """make_game() rewrites one line per setting of a game's .toml: the value
    changes, and the key, its alignment and any comment after it stay."""

    @given(KEY, PAD, st.sampled_from(["true", "1", '["address"]']), st.sampled_from(
        ["false", "[]", "0"]), st.none() | COMMENT, st.booleans())
    def test_a_bare_value_is_replaced_and_its_comment_kept(self, key, pad, old, new, comment,
                                                           newline):
        tail = f"  # {comment}" if comment is not None else ""
        line = f"{key}{pad} = {old}{tail}" + ("\n" if newline else "")
        got = rmp.assign_raw(new)(line)
        want = f"{key}{pad} = {new}" + ("  " + f"# {comment}".rstrip() if comment is not None
                                        else "") + ("\n" if newline else "")
        self.assertEqual(got, want)
        self.assertTrue(rmp.setting(key)(line))

    @given(KEY, PAD, QUOTED, QUOTED, st.none() | COMMENT)
    def test_a_quoted_value_is_replaced_and_the_rest_kept(self, key, pad, old, new, comment):
        tail = f"  # {comment}\n" if comment is not None else "\n"
        line = f'{key}{pad} = "{old}"{tail}'
        self.assertEqual(rmp.assign(new)(line), f'{key}{pad} = "{new}"{tail}')

    @given(KEY, KEY, PAD)
    def test_a_setting_is_its_key_and_no_other(self, key, other, pad):
        self.assertEqual(rmp.setting(key)(f"{other}{pad}= 1\n"), other == key)
        self.assertFalse(rmp.setting(key)(f"# {key} = 1\n"))
        self.assertFalse(rmp.setting(key)(f"{key}\n"))

    @given(st.lists(st.sampled_from(["[dev]", "[android]", "[android.admob]", "[targets]"]),
                    min_size=1, max_size=6), st.sampled_from(["dev", "android", "android.admob"]))
    def test_a_setting_in_a_table_is_found_there_only(self, tables, wanted):
        lines = []
        for table in tables:
            lines += [table + "\n", "enabled = true\n"]
        match = rmp.setting_in(wanted, "enabled")
        hits = [i for i, line in enumerate(lines) if match(line)]
        self.assertEqual(hits, [i + 1 for i, line in enumerate(lines) if line == f"[{wanted}]\n"])


# ---------------------------------------------------------------------------
# rmp install: the startup file
# ---------------------------------------------------------------------------

def expected_rc(home: Path, env: dict, system: str, existing: set) -> tuple[Path, str]:
    """rc_file()'s docstring and its comments, as a table: the startup file
    the login shell ($SHELL) reads, and which syntax."""
    shell = Path(env.get("SHELL") or "sh").name
    if shell == "zsh":
        return Path(env.get("ZDOTDIR") or home) / ".zshrc", "sh"
    if shell == "bash":
        if system == "Darwin":
            first = next((n for n in (".bash_profile", ".bash_login", ".profile")
                          if n in existing), ".bash_profile")
            return home / first, "sh"
        return home / ".bashrc", "sh"
    if shell == "fish":
        config = env.get("XDG_CONFIG_HOME") or ""
        base = Path(config) if config.startswith("/") else home / ".config"
        return base / "fish" / "conf.d" / "rmp.fish", "fish"
    if shell == "tcsh" and ".tcshrc" in existing:
        return home / ".tcshrc", "csh"
    if shell in ("csh", "tcsh"):
        return home / ".cshrc", "csh"
    if shell in rmp.KSH_NAMES:
        named = (env.get("ENV") or "").replace("${HOME}", str(home)).replace("$HOME", str(home))
        if named.startswith("~/"):
            named = str(home) + named[1:]
        if named.startswith("/"):
            return Path(named), "sh"
    return home / ".profile", "sh"


class RcFileTest(unittest.TestCase):
    """The startup file `rmp install` writes its line into is the one the
    person's login shell reads -- zsh's ZDOTDIR, a Mac's bash reading the
    first of three that exists, fish's conf.d, tcsh reading .tcshrc INSTEAD
    of .cshrc, a ksh's $ENV -- for any shell, system and home."""

    @given(st.sampled_from(SHELLS + ["/bin/oksh", "/usr/local/bin/ksh93", "/opt/pdksh",
                                     "/usr/bin/lksh"]),
           st.sampled_from(["Linux", "Darwin", "FreeBSD", "OpenBSD"]),
           st.sets(st.sampled_from([".bash_profile", ".bash_login", ".profile", ".tcshrc",
                                    ".cshrc"])),
           st.fixed_dictionaries({}, optional={
               "ZDOTDIR": st.sampled_from(["", "/z/dot"]),
               "XDG_CONFIG_HOME": st.sampled_from(["", "/x/config", "relative/config"]),
               "ENV": st.sampled_from(["", "~/.kshrc", "$HOME/.mkshrc", "${HOME}/k/rc",
                                       "/etc/kshrc", "relative/rc", "~user/rc"])}))
    def test_the_file_the_login_shell_reads(self, shell, system, existing, env):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            for name in existing:
                (home / name).write_text("")
            env = dict(env, SHELL=shell)
            self.assertEqual(rmp.rc_file(home, env, system),
                             expected_rc(home, env, system, existing))

class StartupFileTest(unittest.TestCase):
    """The block that puts ~/.local/bin on PATH goes in once, after what is
    there, whatever is there: a second install changes nothing."""

    @given(st.text(max_size=80), st.sampled_from(sorted(rmp.RC_LINES)))
    def test_once_and_after_what_is_there(self, before, flavour):
        def read(path):  # every byte, line endings as written (3.12 has no read_text(newline))
            with open(path, encoding="utf-8", newline="") as f:
                return f.read()
        with tempfile.TemporaryDirectory() as tmp:
            rc = Path(tmp) / "conf" / "rc"
            rc.parent.mkdir()
            rc.write_text(before, encoding="utf-8", newline="")
            first = rmp.add_rc_block(rc, flavour)
            after = read(rc)
            second = rmp.add_rc_block(rc, flavour)
            self.assertEqual(read(rc), after)
            self.assertFalse(second)
            if rmp.RC_BEGIN in before:
                self.assertFalse(first)
                self.assertEqual(after, before)
                return
            self.assertTrue(first)
            self.assertTrue(after.startswith(before), "what was there is kept, first")
            block = after[len(before):]
            self.assertEqual(block.lstrip("\n").splitlines(),
                             [rmp.RC_BEGIN, rmp.RC_LINES[flavour], rmp.RC_END])


if __name__ == "__main__":
    unittest.main()
