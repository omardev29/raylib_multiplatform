"""tools/api_change.py and the `api` job of ci.yml: what a pull request
changes of the framework's API, and the label only Omar adds.

Run with:  python3 -m unittest discover -s tests -p api_change_test.py

Against a real git, in scratch repositories, with a stand-in gh on PATH that
answers with the labels and the events it is given -- the shapes GitHub's REST
API returns -- and writes down every call it gets. The job's own step is
played too, on a merge commit as GitHub checks one out for a pull request.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOLS = REPO / "tools"
SCRIPT = TOOLS / "api_change.py"
CI = REPO / ".github" / "workflows" / "ci.yml"
TEMPLATE = REPO / ".github" / "pull_request_template.md"
IN_BUILD_IMAGE = Path("/etc/raylib-build-image.json").is_file()
FRAMEWORK_GATE = "needs.config.outputs.framework == 'true'"


def load_tool(name: str):
    """tools/<name>.py as `tools.<name>`, the name mutmut gives it."""
    spec = importlib.util.spec_from_file_location(f"tools.{name}", TOOLS / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


api = load_tool("api_change")

# A git that reads nobody's configuration: a global commit.gpgsign would make
# every commit here ask for a key.
GIT_ENV = {"GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
           "GIT_COMMITTER_EMAIL": "t@t", "GIT_CONFIG_GLOBAL": os.devnull,
           "GIT_CONFIG_NOSYSTEM": "1"}

CONFIGURE = '''"""A stand-in for tools/configure.py: DEFAULTS, and code around it."""

DEFAULTS: dict = {
    "window": {"title": "x", "fps": 0},
    "ios": {"bundle_id": "com.example.t", "settings": {}},
}


def validate(cfg):
    return cfg
'''

TOML = '''[window]
title = "x"
fps = 0

[ios.settings]
CODE_SIGN_STYLE = "Automatic"
'''

HEADER = "#pragma once\n// The score, as a game reads it.\nint score();\n"

OWNER = "omardev29"


class Scratch:
    """A repository shaped like the framework where it matters here: a public
    header, configure.py's DEFAULTS, the .toml, and a source file."""

    def __init__(self, root: Path, files: dict | None = None):
        self.root = root
        root.mkdir(parents=True)
        self.git("init", "-q", "-b", "main")
        self.write({"include/rmp/game.h": HEADER, "tools/configure.py": CONFIGURE,
                    "raylib_multiplatform.toml": TOML,
                    "src/game.cpp": "int score() { return 0; }\n", **(files or {})})
        self.commit("base")

    def git(self, *argv: str) -> str:
        return subprocess.run(["git", *argv], cwd=self.root, env={**os.environ, **GIT_ENV},
                              check=True, capture_output=True, text=True).stdout

    def write(self, files: dict) -> None:
        for path, text in files.items():
            target = self.root / path
            if text is None:
                target.unlink()
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(text)

    def commit(self, message: str) -> None:
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", message)

    def pull_request(self, files: dict, name: str = "pr") -> None:
        """A branch `name` off main with `files` changed; main stays checked out."""
        self.git("checkout", "-q", "-b", name)
        self.write(files)
        self.commit(name)
        self.git("checkout", "-q", "main")

    def edit(self, path: str, old: str, new: str) -> str:
        text = (self.root / path).read_text()
        assert text.count(old) == 1, (path, old)
        return text.replace(old, new)


class Fixture(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.repo = Scratch(self.dir / "repo")

    def changes(self, files: dict) -> list[str]:
        """What a pull request of `files` changes. Its branch is gone after,
        whatever the case asserts, so the next case can make its own."""
        self.repo.pull_request(files)
        try:
            return api.changes("main", "pr", cwd=str(self.repo.root))
        finally:
            self.repo.git("branch", "-q", "-D", "pr")


class ChangesTest(Fixture):
    """What counts as a change of the API, read from git alone."""

    def test_a_change_elsewhere_is_none(self):
        """A source file, configure.py's code outside DEFAULTS, a value of
        the .toml and a key of [ios.settings] -- the game's own table."""
        configure = self.repo.edit("tools/configure.py", "    return cfg\n",
                                   "    cfg = dict(cfg)\n    return cfg\n")
        toml = self.repo.edit("raylib_multiplatform.toml", "fps = 0", "fps = 60")
        toml = toml.replace('CODE_SIGN_STYLE = "Automatic"\n',
                            'CODE_SIGN_STYLE = "Automatic"\nDEVELOPMENT_TEAM = "X"\n')
        self.assertEqual(self.changes({"src/game.cpp": "int score() { return 1; }\n",
                                       "tools/configure.py": configure,
                                       "raylib_multiplatform.toml": toml}), [])

    def test_any_change_to_a_header_counts_a_comment_too(self):
        header = HEADER.replace("as a game reads it", "as the game reads it")
        self.assertEqual(self.changes({"include/rmp/game.h": header}),
                         ["include/rmp/game.h: modified"])

    def test_a_header_added_deleted_or_renamed(self):
        self.assertEqual(self.changes({"include/rmp/extra.h": "#pragma once\n"}),
                         ["include/rmp/extra.h: added"])
        self.assertEqual(self.changes({"include/rmp/game.h": None}),
                         ["include/rmp/game.h: deleted"])
        self.assertEqual(self.changes({"include/rmp/game.h": None,
                                       "include/rmp/play.h": HEADER}),
                         ["include/rmp/play.h: renamed from include/rmp/game.h"])

    def test_a_key_of_defaults_added_removed_or_given_another_default(self):
        cases = [
            ('"fps": 0}', '"fps": 0, "vsync": True}',
             ["[window.vsync]: a new key, default true (DEFAULTS in tools/configure.py)"]),
            ('"title": "x", "fps": 0}', '"title": "x"}',
             ["[window.fps]: no longer a key (DEFAULTS in tools/configure.py)"]),
            ('"fps": 0}', '"fps": 60}',
             ["[window.fps]: its default was 0 and is 60 (DEFAULTS in tools/configure.py)"]),
            # The same number to Python, another type to the .toml.
            ('"fps": 0}', '"fps": 0.0}',
             ["[window.fps]: its default was 0 and is 0.0 (DEFAULTS in tools/configure.py)"]),
            # A free-form table's default is a value like any other.
            ('"settings": {}', '"settings": {"A": "1"}',
             ['[ios.settings]: its default was {} and is {"A": "1"} '
              "(DEFAULTS in tools/configure.py)"]),
        ]
        for old, new, says in cases:
            with self.subTest(new=new):
                configure = self.repo.edit("tools/configure.py", old, new)
                self.assertEqual(self.changes({"tools/configure.py": configure}), says)

    def test_a_key_of_the_toml_added_or_removed(self):
        added = self.repo.edit("raylib_multiplatform.toml", "fps = 0\n", "fps = 0\nvsync = true\n")
        self.assertEqual(self.changes({"raylib_multiplatform.toml": added}),
                         ["[window.vsync]: a new key in raylib_multiplatform.toml"])
        removed = self.repo.edit("raylib_multiplatform.toml", "fps = 0\n", "")
        self.assertEqual(self.changes({"raylib_multiplatform.toml": removed}),
                         ["[window.fps]: no longer in raylib_multiplatform.toml"])

    def test_what_main_did_after_the_branch_began_is_not_this_change(self):
        """The base moved on -- a header changed there -- and the pull request
        touched a source file. A two-dot diff calls that header this change."""
        self.repo.pull_request({"src/game.cpp": "int score() { return 2; }\n"})
        self.repo.write({"include/rmp/game.h": HEADER + "int lives();\n"})
        self.repo.commit("main moves on")
        self.assertEqual(api.changes("main", "pr", cwd=str(self.repo.root)), [])

    def test_configure_py_gone_takes_every_key_with_it(self):
        self.assertEqual(self.changes({"tools/configure.py": None}), [
            "[ios.bundle_id]: no longer a key (DEFAULTS in tools/configure.py)",
            "[ios.settings]: no longer a key (DEFAULTS in tools/configure.py)",
            "[window.fps]: no longer a key (DEFAULTS in tools/configure.py)",
            "[window.title]: no longer a key (DEFAULTS in tools/configure.py)"])

    def test_what_it_cannot_read_is_never_a_pass(self):
        cases = {
            "DEFAULTS that is not a literal":
                {"tools/configure.py": CONFIGURE.replace('"fps": 0}', '"fps": int("0")}')},
            "no DEFAULTS at all":
                {"tools/configure.py": CONFIGURE.replace("DEFAULTS: dict =", "SETTINGS: dict =")},
            "a configure.py that does not parse": {"tools/configure.py": "DEFAULTS = {\n"},
            "a .toml that does not parse": {"raylib_multiplatform.toml": "[window\n"},
        }
        for what, files in cases.items():
            with self.subTest(what), self.assertRaises(api.Unsure):
                self.changes(files)
        with self.assertRaisesRegex(api.Unsure, "no-such-branch is not a commit"):
            api.changes("main", "no-such-branch", cwd=str(self.repo.root))


# ---------------------------------------------------------------------------
# The pull request: the labels, and who added them
# ---------------------------------------------------------------------------

GH = r'''#!/bin/sh
printf '%s\n' "$*" >> "$GH_CALLS"
if [ -n "$GH_FAIL" ]; then
  case "$*" in *"$GH_FAIL"*) echo "HTTP 403: Resource not accessible by integration" >&2; exit 1 ;; esac
fi
case "$*" in
  *"--method POST"*) cat >> "$GH_POSTED" ;;
  *"/labels"*) cat "$GH_LABELS" ;;
  *"/events"*) cat "$GH_EVENTS" ;;
  *) echo "gh stand-in: nothing for: $*" >&2; exit 9 ;;
esac
'''


def labelled(name: str, who: str, event: str = "labeled") -> dict:
    """An issue event as GitHub's REST API returns one."""
    return {"id": 1, "event": event, "actor": {"login": who, "type": "User"},
            "label": {"name": name, "color": "ededed"}, "created_at": "2026-10-11T12:00:00Z"}


class Github:
    """The stand-in gh, and what it was asked."""

    def __init__(self, root: Path):
        self.root = root
        bin_dir = root / "bin"
        bin_dir.mkdir()
        gh = bin_dir / "gh"
        gh.write_text(GH)
        gh.chmod(0o755)
        self.calls, self.posted = root / "calls", root / "posted"
        self.labels_file, self.events_file = root / "labels.json", root / "events.json"
        self.summary = root / "summary.md"
        self.env = {**os.environ, **GIT_ENV, "PATH": f"{bin_dir}:{os.environ['PATH']}",
                    "GH_CALLS": str(self.calls), "GH_POSTED": str(self.posted),
                    "GH_LABELS": str(self.labels_file), "GH_EVENTS": str(self.events_file),
                    "GH_FAIL": "", "GH_TOKEN": "t",
                    "GITHUB_REPOSITORY": f"{OWNER}/raylib_multiplatform",
                    "GITHUB_REPOSITORY_OWNER": OWNER, "GITHUB_STEP_SUMMARY": str(self.summary)}

    def answer(self, labels: list[str], events: list | str = ()) -> None:
        self.labels_file.write_text(json.dumps([{"id": 1, "name": n} for n in labels]))
        self.events_file.write_text(events if isinstance(events, str) else json.dumps(events))
        for f in (self.calls, self.posted, self.summary):
            f.write_text("")

    def asked(self) -> list[str]:
        return self.calls.read_text().splitlines()


class PullRequestTest(Fixture):
    """The script as the `api` job runs it: --pr, and a gh to ask."""

    def setUp(self):
        super().setUp()
        self.gh = Github(self.dir)

    def run_pr(self, *extra: str, env: dict | None = None):
        got = subprocess.run([sys.executable, str(SCRIPT), "--base", "main", "--head", "pr",
                              *extra], cwd=self.repo.root, env={**self.gh.env, **(env or {})},
                             capture_output=True, text=True)
        return got.returncode, got.stdout + got.stderr

    def header_change(self):
        self.repo.pull_request({"include/rmp/game.h": HEADER + "int lives();\n"})

    def test_a_header_change_without_the_label_is_labelled_and_fails(self):
        self.header_change()
        self.gh.answer([])
        code, out = self.run_pr("--pr", "7")
        self.assertEqual(code, 1, out)
        self.assertIn("include/rmp/game.h: modified", out)
        self.assertIn("waits for Omar", out)
        self.assertIn(f"adds the label api-approved -- only {OWNER} may", out)
        self.assertEqual(json.loads(self.gh.posted.read_text()), {"labels": ["api-change"]})
        self.assertEqual(self.gh.asked(), [
            f"api repos/{OWNER}/raylib_multiplatform/issues/7/labels?per_page=100",
            f"api --method POST repos/{OWNER}/raylib_multiplatform/issues/7/labels --input -"])
        self.assertIn("include/rmp/game.h: modified", self.gh.summary.read_text())

    def test_a_label_already_there_is_not_added_twice(self):
        self.header_change()
        self.gh.answer(["api-change"])
        code, out = self.run_pr("--pr", "7")
        self.assertEqual(code, 1, out)
        self.assertEqual(self.gh.posted.read_text(), "")

    def test_the_owners_approval_passes(self):
        self.header_change()
        self.gh.answer(["api-change", "api-approved"],
                       [labelled("api-change", "github-actions[bot]"),
                        labelled("api-approved", OWNER)])
        code, out = self.run_pr("--pr", "7")
        self.assertEqual(code, 0, out)
        self.assertIn(f"PASS: approved -- api-approved was added by {OWNER}.", out)
        self.assertIn("include/rmp/game.h: modified", out)
        self.assertEqual(self.gh.posted.read_text(), "")
        self.assertTrue(any("--paginate" in c and "/events" in c for c in self.gh.asked()))

    def test_anybody_elses_approval_is_refused(self):
        self.header_change()
        for who in ("claude[bot]", "a-collaborator"):
            with self.subTest(who=who):
                self.gh.answer(["api-change", "api-approved"], [labelled("api-approved", who)])
                code, out = self.run_pr("--pr", "7")
                self.assertEqual(code, 1, out)
                self.assertIn(f"api-approved was added by {who}, and only {OWNER}", out)

    def test_the_last_one_to_add_it_is_who_approved(self):
        self.header_change()
        removed_and_put_back = [labelled("api-approved", OWNER),
                                labelled("api-approved", OWNER, "unlabeled"),
                                labelled("api-approved", "claude[bot]")]
        self.gh.answer(["api-approved"], removed_and_put_back)
        self.assertEqual(self.run_pr("--pr", "7")[0], 1)
        self.gh.answer(["api-approved"], [labelled("api-approved", "claude[bot]"),
                                          labelled("api-approved", "claude[bot]", "unlabeled"),
                                          labelled("api-approved", OWNER)])
        self.assertEqual(self.run_pr("--pr", "7")[0], 0)

    def test_every_page_of_events_is_read(self):
        """gh --paginate prints one JSON array a page, one after the other;
        the approval is on the second."""
        self.header_change()
        first = [{"id": i, "event": "commented", "actor": {"login": "x"}} for i in range(100)]
        self.gh.answer(["api-approved"],
                       json.dumps(first) + "\n" + json.dumps([labelled("api-approved", OWNER)]))
        code, out = self.run_pr("--pr", "7")
        self.assertEqual(code, 0, out)

    def test_the_owner_is_a_login_whatever_its_case(self):
        self.header_change()
        self.gh.answer(["api-approved"], [labelled("api-approved", "OmarDev29")])
        self.assertEqual(self.run_pr("--pr", "7")[0], 0)

    def test_a_label_nobody_added_is_not_a_pass(self):
        self.header_change()
        self.gh.answer(["api-approved"], [labelled("api-change", OWNER)])
        code, out = self.run_pr("--pr", "7")
        self.assertEqual(code, 2, out)
        self.assertIn("no event of it says who added it", out)

    def test_a_change_elsewhere_asks_github_nothing(self):
        self.repo.pull_request({"src/game.cpp": "int score() { return 3; }\n"})
        self.gh.answer([])
        code, out = self.run_pr("--pr", "7")
        self.assertEqual(code, 0, out)
        self.assertIn("PASS: no public header and no key of the .toml changed.", out)
        self.assertEqual(self.gh.asked(), [])

    def test_a_github_that_says_no_is_not_a_pass(self):
        self.header_change()
        for refused in ("/labels?per_page", "--method POST", "/events"):
            with self.subTest(refused=refused):
                self.gh.answer(["api-approved"] if refused == "/events" else [],
                               [labelled("api-approved", OWNER)])
                code, out = self.run_pr("--pr", "7", env={"GH_FAIL": refused})
                self.assertEqual(code, 2, out)
                self.assertIn("could not tell", out)
                self.assertIn("HTTP 403", out)

    def test_without_the_repository_it_cannot_judge(self):
        self.header_change()
        self.gh.answer([])
        code, out = self.run_pr("--pr", "7", env={"GITHUB_REPOSITORY": ""})
        self.assertEqual(code, 2, out)
        self.assertEqual(self.gh.asked(), [])

    def test_without_pr_it_only_says_what_it_found(self):
        self.header_change()
        self.gh.answer([])
        code, out = self.run_pr()
        self.assertEqual(code, 0, out)
        self.assertIn("include/rmp/game.h: modified", out)
        self.assertIn("fails until Omar adds api-approved", out)
        self.assertEqual(self.gh.asked(), [])

    def test_what_it_cannot_read_fails_the_job(self):
        self.repo.pull_request({"raylib_multiplatform.toml": "[window\n"})
        self.gh.answer([])
        code, out = self.run_pr("--pr", "7")
        self.assertEqual(code, 2, out)
        self.assertIn("is not valid TOML", out)


# ---------------------------------------------------------------------------
# The real tree: the keys it reads are configure.py's
# ---------------------------------------------------------------------------

class RealTreeTest(unittest.TestCase):

    def test_the_keys_are_read_as_configure_py_defines_them(self):
        """Read from the source, as the gate reads both sides of a pull
        request, and imported, as configure.py itself uses them: the same."""
        configure = load_tool("configure")
        read = api.leaves(api.defaults_of((TOOLS / "configure.py").read_text(), "configure.py"))
        self.assertEqual(read, api.leaves(configure.DEFAULTS))
        for key in ("window.width", "android.admob.enabled", "ci.on_push", "ios.settings"):
            self.assertIn(key, read)

    def test_the_free_form_tables_are_the_ones_configure_py_passes_through(self):
        """deep_merge() takes such a table as written, `v if here == "..."`;
        a second one there is a second table whose keys are the game's."""
        configure = load_tool("configure")
        source = (TOOLS / "configure.py").read_text()
        through = re.findall(r'\bv if here == "([\w.]+)" else deep_merge\(', source)
        self.assertEqual(through, list(api.FREE_FORM))
        for table in through:
            section, key = table.split(".")
            self.assertIsInstance(configure.DEFAULTS[section][key], dict)

    def test_the_frameworks_toml_has_no_key_defaults_lacks(self):
        configure = load_tool("configure")
        known = set(api.leaves(configure.DEFAULTS))
        keys = api.toml_keys((REPO / "raylib_multiplatform.toml").read_text(), "the .toml")
        self.assertIn("window.title", keys)
        self.assertEqual(sorted(keys - known), [])


# ---------------------------------------------------------------------------
# The job, the template and the words they share
# ---------------------------------------------------------------------------

def require_yaml(case):
    try:
        import yaml
    except ImportError:
        if IN_BUILD_IMAGE:
            case.fail("PyYAML is missing inside the build image")
        case.skipTest("PyYAML not installed (it ships in the build image)")
    return yaml


class ApiJobTest(unittest.TestCase):
    """The `api` job: on a pull request only, in the framework only, and the
    base's own copy of the script judging -- a pull request cannot change the
    judge."""

    def job(self) -> dict:
        return require_yaml(self).safe_load(CI.read_text())["jobs"]["api"]

    def test_it_runs_on_a_pull_request_and_in_the_framework(self):
        job = self.job()
        self.assertEqual(job["needs"], "config")
        # A push, a tag and a dispatch have no pull request: skipped, which
        # a required check counts as passed.
        self.assertEqual(job.get("if"), "github.event_name == 'pull_request'")
        for step in job["steps"]:
            with self.subTest(step=step.get("name", step.get("uses"))):
                self.assertEqual(step.get("if"), FRAMEWORK_GATE)

    def test_it_asks_for_what_it_uses_and_no_more(self):
        self.assertEqual(self.job()["permissions"],
                         {"contents": "read", "pull-requests": "write", "issues": "read"})

    def test_it_checks_out_the_merge_and_its_parents(self):
        checkout = self.job()["steps"][0]
        self.assertTrue(checkout["uses"].startswith("actions/checkout@"))
        self.assertEqual(checkout["with"]["fetch-depth"], 2)
        self.assertIs(checkout["with"]["persist-credentials"], False)

    def step(self) -> str:
        steps = [s for s in self.job()["steps"] if "api_change.py" in str(s.get("run", ""))]
        self.assertEqual(len(steps), 1)
        self.assertEqual(steps[0]["env"]["PR"], "${{ github.event.pull_request.number }}")
        return steps[0]["run"]

    def play(self, files: dict, base_has_it: bool = True) -> subprocess.CompletedProcess:
        """The step's script, on the merge commit GitHub makes of a pull
        request: HEAD^1 is the base as it is now, HEAD the two merged."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo = Scratch(root / "repo",
                           {"tools/api_change.py": SCRIPT.read_text()} if base_has_it else {})
            repo.pull_request(files)
            repo.git("checkout", "-q", "--detach", "main")
            repo.git("merge", "-q", "--no-ff", "-m", "Merge pr into main", "pr")
            gh = Github(root)
            gh.answer([])
            (root / "runner").mkdir()
            return subprocess.run(["bash", "-e", "-c", self.step()], cwd=repo.root,
                                  env={**gh.env, "PR": "7", "RUNNER_TEMP": str(root / "runner")},
                                  capture_output=True, text=True)

    def test_the_step_fails_a_header_change_and_passes_the_rest(self):
        got = self.play({"include/rmp/game.h": HEADER + "int lives();\n"})
        self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
        self.assertIn("include/rmp/game.h: modified", got.stdout)
        got = self.play({"src/game.cpp": "int score() { return 4; }\n"})
        self.assertEqual(got.returncode, 0, got.stdout + got.stderr)

    def test_a_pull_request_that_rewrites_the_judge_is_judged_by_the_old_one(self):
        got = self.play({"include/rmp/game.h": HEADER + "int lives();\n",
                         "tools/api_change.py": "import sys\nsys.exit(0)\n"})
        self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
        self.assertIn("include/rmp/game.h: modified", got.stdout)

    def test_a_base_without_the_judge_lends_it_the_pull_requests_and_says_so(self):
        """A base branch from before the gate: the copy the pull request
        brings judges it, and the log says whose it was."""
        got = self.play({"include/rmp/game.h": HEADER + "int lives();\n",
                         "tools/api_change.py": SCRIPT.read_text()}, base_has_it=False)
        self.assertEqual(got.returncode, 1, got.stdout + got.stderr)
        self.assertIn("the pull request's own copy judges", got.stdout)
        self.assertIn("include/rmp/game.h: modified", got.stdout)


class SharedWordsTest(unittest.TestCase):
    """The labels are spelled one way: in the script, in deep.yml's
    tools/finding.py, in CLAUDE.md and in the pull-request template."""

    def test_the_labels_are_named_alike_everywhere(self):
        finding = load_tool("finding")
        self.assertEqual(finding.LABEL, "nightly-finding")
        for path in (REPO / "CLAUDE.md", TEMPLATE):
            text = path.read_text()
            for label in (api.CHANGE_LABEL, api.APPROVED_LABEL, finding.LABEL):
                with self.subTest(file=path.name, label=label):
                    self.assertTrue(f"`{label}`" in text, f"{path.name} never names `{label}`")

    def test_claude_md_says_how_an_agent_works_here(self):
        text = (REPO / "CLAUDE.md").read_text()
        heading = "\n## How an agent works here\n"
        self.assertTrue(heading in text, "CLAUDE.md has no section How an agent works here")
        section = " ".join(text.split(heading, 1)[1].split("\n## ", 1)[0].split())
        for words in ("`.github/pull_request_template.md`", "`tools/api_change.py`",
                      "**Never push to `main`**", "**Never weaken a gate to get green.**",
                      "`nightly-finding`", "`api-approved`"):
            with self.subTest(words=words):
                self.assertTrue(words in section, f"the section does not say {words}")


class TemplateTest(unittest.TestCase):
    """.github/pull_request_template.md: what every pull request says, and
    every command in it one that rmp runs."""

    SECTIONS = ("## What changed, and why", "## The ladder", "## Seen red first",
                "## Not verified", "## Checklist")

    def test_it_asks_for_each_part(self):
        text = TEMPLATE.read_text()
        for heading in self.SECTIONS:
            with self.subTest(heading=heading):
                self.assertIn(f"\n{heading}\n", text)
        self.assertEqual([line for line in text.splitlines() if line.startswith("## ")],
                         list(self.SECTIONS))

    def test_every_command_it_names_is_one_rmp_runs(self):
        rmp = load_tool("rmp")
        stages = {s.name for s in rmp.STAGES}
        named = re.findall(r"`rmp ([a-z-]+)(?: ([a-z-]+))?", TEMPLATE.read_text())
        self.assertGreaterEqual(len(named), 6, named)
        for command, word in named:
            with self.subTest(command=f"rmp {command} {word}".strip()):
                self.assertIn(command, rmp.COMMANDS)
                if command == "test" and word:
                    self.assertIn(word, stages)


if __name__ == "__main__":
    unittest.main()
