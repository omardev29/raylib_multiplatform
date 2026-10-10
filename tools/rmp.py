#!/usr/bin/env python3
"""rmp -- build, run, test and ship a raylib_multiplatform project.

`rmp help` lists the commands and `rmp help <command>` explains one. This file
is the whole implementation; the `rmp`, `rmp.ps1` and `rmp.cmd` launchers at
the root of a project only find a Python 3.11+ and hand it every argument.

Inside a project, a framework `rmp` on PATH runs that project's OWN
tools/rmp.py, so a game keeps the rmp it was made with when the framework moves
on (RMP_NO_DELEGATE=1 turns that off). `rmp new` is the exception: it uses the
framework that ships the rmp being run.

The project is the nearest folder, from here upwards, holding
raylib_multiplatform.toml. It is the framework itself when it also holds
tests/configure_test.py (which `rmp new` never copies), and a game otherwise;
`python3 tools/rmp.py --mode` prints which, and CI reads it.

Exit codes: 0 done (or help), 1 a check failed or something was refused,
2 the command line was wrong, 130 interrupted.

Written for Python 3.11+, and kept parseable by 3.8 so that an older Python
gets the sentence below instead of a SyntaxError.
"""

from __future__ import annotations

import sys

if sys.version_info < (3, 11):
    sys.stderr.write("rmp needs Python 3.11 or newer; this is %d.%d (%s)\n"
                     % (sys.version_info[0], sys.version_info[1], sys.executable))
    sys.exit(1)

import difflib
import json
import os
import platform
import shutil
import subprocess
import textwrap
import time
from pathlib import Path

TOML = "raylib_multiplatform.toml"
FRAMEWORK_MARKER = "tests/configure_test.py"
FRAMEWORK_URL = "https://github.com/omardev29/raylib_multiplatform"

OK, FAILED, USAGE, INTERRUPTED = 0, 1, 2, 130

# What `rmp clean` deletes: build output and everything generated from the
# .toml. tests/rmp_test.py checks this against the generated block of
# .gitignore. The icons stay: making them again needs Pillow.
CLEAN_PATHS = ("build", "raymob/app/generated", "raymob/generated.properties",
               "cmake/generated", "include/rmp/generated", "ios/project.yml")

# What `rmp fmt` formats. A game formats its own code and not the framework's:
# a different clang-format would rewrite src/rmp/ for no reason of its own.
FMT_ROOTS = {"framework": ("include", "src", "tests", "examples"),
             "game": ("include", "src")}
FMT_SKIP = ("src/rmp/", "include/rmp/")


class Usage(Exception):
    """A command line rmp cannot run. Printed, and exit code 2."""


class Refused(Exception):
    """A check that failed or a precondition that does not hold. Exit code 1."""


# ---------------------------------------------------------------------------
# Where we are
# ---------------------------------------------------------------------------

def find_root(start: Path) -> Path | None:
    """The nearest folder, from `start` upwards, holding the project's .toml."""
    here = start.resolve()
    for folder in (here, *here.parents):
        if (folder / TOML).is_file():
            return folder
    return None


def mode_of(root: Path) -> str:
    return "framework" if (root / FRAMEWORK_MARKER).is_file() else "game"


def on_windows() -> bool:
    # The same rule as configure.py's: MSYS2 and Cygwin are Windows too, and
    # neither reports itself as "Windows".
    system = platform.system()
    return os.name == "nt" or system.startswith(("MSYS", "MINGW", "CYGWIN"))


class Ctx:
    """The project rmp is working on, and the one door every command goes
    through to run something -- which is what lets the tests record what a
    command would have run, without a compiler."""

    def __init__(self, root: Path):
        self.root = root
        self.mode = mode_of(root)
        self.python = sys.executable

    def run(self, argv, cwd=None, env=None, check=True, capture=False):
        full_env = dict(os.environ)
        if env:
            full_env.update(env)
        try:
            done = subprocess.run([str(a) for a in argv], cwd=str(cwd or self.root),
                                  env=full_env, text=True,
                                  stdout=subprocess.PIPE if capture else None,
                                  stderr=subprocess.STDOUT if capture else None)
        except FileNotFoundError:
            raise Refused(f"{argv[0]} is not installed, or not on PATH")
        if check and done.returncode != 0:
            if capture and done.stdout:
                sys.stdout.write(done.stdout)
            raise Refused(f"`{' '.join(str(a) for a in argv)}` failed (exit {done.returncode})")
        return done

    def name(self) -> str:
        """[project] name: the executable, and every artefact's name."""
        got = self.run([self.python, "tools/configure.py", "--print-name"],
                       check=False, capture=True)
        name = (got.stdout or "").strip()
        return name if got.returncode == 0 and name else "game"

    def exe(self, path: str) -> Path:
        path = self.root / path
        return path.with_name(path.name + ".exe") if on_windows() else path

    def bash(self) -> str:
        return find_bash()


def find_bash() -> str:
    """A bash for the tools/*.sh scripts. On Windows, Git's: System32's
    bash.exe is the WSL launcher, which runs a different machine."""
    if on_windows() and os.name == "nt":
        git = shutil.which("git")
        if git:
            got = subprocess.run([git, "--exec-path"], capture_output=True, text=True)
            exec_path = Path(got.stdout.strip())
            # <git>/mingw64/libexec/git-core -> <git>/bin/bash.exe
            for candidate in (exec_path.parents[2] / "bin" / "bash.exe",
                              exec_path.parents[2] / "usr" / "bin" / "bash.exe"):
                if candidate.is_file():
                    return str(candidate)
        found = shutil.which("bash")
        if found and "system32" not in found.lower():
            return found
        raise Refused("this needs bash: install Git for Windows, which brings one")
    return shutil.which("bash") or "bash"


# ---------------------------------------------------------------------------
# Building
# ---------------------------------------------------------------------------

def reconfigure(ctx: Ctx, want: str) -> None:
    """Delete build/ when it was configured as another build type.

    The debug and release presets share build/, and switching presets there
    keeps the compiler [dev] wrote into the cache with FORCE -- the release
    build would quietly use your [dev] compiler. build/ is the only place the
    stale answer lives, and switching build type rebuilds everything anyway."""
    cache = ctx.root / "build" / "CMakeCache.txt"
    if not cache.is_file():
        return
    have = ""
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_BUILD_TYPE:"):
            have = line.split("=", 1)[1] if "=" in line else ""
            break
    if have == want:
        return
    print(f"  build/ was configured as {have or 'unknown'}, and this is {want}.")
    print("  removing build/ -- the compiler is cached and would carry over.")
    remove_tree(ctx.root / "build")


def remove_tree(path: Path) -> None:
    def make_writable(func, target, _info):
        os.chmod(target, 0o700)
        func(target)
    if path.is_dir():
        if sys.version_info >= (3, 12):
            shutil.rmtree(path, onexc=make_writable)
        else:
            shutil.rmtree(path, onerror=make_writable)
    elif path.exists():
        path.unlink()


def build(ctx: Ctx, release: bool = False) -> str:
    kind = "Release" if release else "Debug"
    preset = "release" if release else "debug"
    reconfigure(ctx, kind)
    ctx.run(["cmake", "--preset", preset])
    name = ctx.name()
    # --target is not a detail: without it this builds whatever else the
    # directory has configured, and a test binary in a release tree cannot
    # read raylib's LTO archive.
    ctx.run(["cmake", "--build", "build", "--target", name])
    return name


# ---------------------------------------------------------------------------
# Test stages, as data: tests/rmp_test.py compares them with the CI lint job.
# ---------------------------------------------------------------------------

class Stage:
    def __init__(self, name, summary, steps, scope="framework", in_all=True):
        self.name = name
        self.summary = summary
        self.steps = steps
        self.scope = scope  # "framework", "game" or "both"
        self.in_all = in_all


def run_step(ctx: Ctx, step) -> None:
    kind = step[0]
    if kind == "run":
        argv = [ctx.python if a == "{python}" else ctx.bash() if a == "bash" else a
                for a in step[1]]
        argv = [str(ctx.exe(a)) if isinstance(a, str) and a.startswith("build/") else a
                for a in argv]
        env = step[2] if len(step) > 2 else None
        ctx.run(argv, env=env)
    elif kind == "configure":
        reconfigure(ctx, "Debug")
        ctx.run(["cmake", "--preset", "debug", *step[1]], capture=True)
    elif kind == "build":
        ctx.run(["cmake", "--build", "build", *(["--target", step[1]] if step[1] else [])],
                capture=True)
    elif kind == "smoke":
        name = ctx.name()
        got = ctx.run([ctx.exe(f"build/{name}")], env={"RAY_TEST_MAX_FRAMES": "10"},
                      check=False, capture=True)
        out = got.stdout or ""
        if "RAY_TEST_BOOT_OK assets_failed=0 " not in out:
            sys.stdout.write(out)
            raise Refused("the game did not boot, or an asset failed to load")
        if "RAY_TEST_RENDER_OK" not in out:
            sys.stdout.write(out)
            raise Refused("the game booted and drew nothing")
        print("  ok    booted and rendered")
    elif kind == "fmt-check":
        fmt(ctx, check=True)
    elif kind == "render":
        ctx.run(["sh", "tools/render_check.sh", "Ninja", "", ctx.name(), step[1]])
    else:  # pragma: no cover - a typo in STAGES
        raise AssertionError(f"unknown step {kind!r}")


LOCALES_ENV = {"LOCPATH": "build/test-locales"}

STAGES = [
    Stage("fmt", "every file is clang-format clean", [("fmt-check",)]),
    Stage("config", "the .toml, the pinned versions and every licence",
          [("run", ["{python}", "tools/configure.py", "--check"]),
           ("run", ["bash", "tools/versions_check.sh"]),
           ("run", ["{python}", "tools/license_db.py", "--check"])], scope="both"),
    Stage("repo", "the index tracks sources, not build output",
          [("run", ["bash", "tools/repo_check.sh"])]),
    Stage("seam", "nothing in src/rmp reads the clock, input or rand() itself",
          [("run", ["bash", "tools/seam_check.sh"])]),
    Stage("workflows", "every reusable workflow gets what it asks for",
          [("run", ["bash", "tools/workflow_check.sh"])]),
    Stage("portable", "the scripts run on macOS's bash 3.2 and the BSDs",
          [("run", ["bash", "tools/portable_check.sh"])]),
    Stage("shell", "no `A && B || C` in the shell",
          [("run", ["bash", "tools/shell_pattern_check.sh"])]),
    Stage("naming", "the names follow the convention, every #if branch",
          [("run", ["bash", "tools/naming_check.sh"])]),
    Stage("ownership", "no owning raw pointer in the framework",
          [("run", ["bash", "tools/ownership_check.sh"])]),
    Stage("headers", "every public header stands alone",
          [("run", ["bash", "tools/header_check.sh"])]),
    Stage("cost", "what each public header costs to include",
          [("run", ["bash", "tools/header_cost_check.sh"])]),
    Stage("configure", "tools/configure.py, every rejection",
          [("run", ["{python}", "-m", "unittest", "discover", "-s", "tests", "-p",
                    "configure_test.py"])]),
    Stage("rmp", "this command itself",
          [("run", ["{python}", "-m", "unittest", "discover", "-s", "tests", "-p",
                    "rmp_test.py"])]),
    Stage("binaries", "the readers that judge a shipped .exe and Mac binary",
          [("run", ["{python}", "-m", "unittest", "discover", "-s", "tests", "-p",
                    "binary_check_test.py"])]),
    Stage("unit", "the unit tests, in two orders",
          [("configure", ["-DBUILD_TESTS=ON"]),
           ("build", "unit_test"),
           ("run", ["bash", "tools/test_locales.sh", "build/test-locales"]),
           ("run", ["build/unit_test"], LOCALES_ENV),
           ("run", ["build/unit_test", "--order-by=rand", "--rand-seed=1337",
                    "--test-suite-exclude=audio: device"], LOCALES_ENV)]),
    Stage("layout", "the UI layout at four resolutions, headless",
          [("configure", ["-DBUILD_UI_TESTS=ON"]),
           ("build", "ui_layout_test"),
           ("run", ["build/ui_layout_test"])]),
    Stage("render", "draw a frame in software and check it",
          [("render", "check")], scope="both"),
    Stage("smoke", "build the game, boot it, see it draw",
          [("configure", []), ("build", None), ("smoke",)], scope="both"),
    Stage("examples", "build and boot every example, and play the platformer",
          [("run", ["{python}", "tools/configure.py"]),
           ("run", ["bash", "tools/examples_build.sh"])], in_all=False),
    Stage("sanitize", "the unit tests again under ASan and UBSan",
          [("run", ["bash", "tools/test_locales.sh", "build/test-locales"]),
           ("run", ["bash", "tools/sanitize_check.sh"])], in_all=False),
    Stage("render-update", "record the frame as the new golden hash",
          [("render", "update")], in_all=False),
]


def stages_for(mode: str) -> list[Stage]:
    return [s for s in STAGES if s.scope in (mode, "both")]


def all_stages(mode: str) -> list[Stage]:
    return [s for s in stages_for(mode) if s.in_all]


# ---------------------------------------------------------------------------
# The commands
# ---------------------------------------------------------------------------

def no_args(args):
    if args:
        raise Usage(f"unexpected: {' '.join(args)}")


def one_of(args, allowed, default):
    if not args:
        return default
    if len(args) > 1 or args[0] not in allowed:
        raise Usage(f"expected one of {', '.join(allowed)}, not {' '.join(args)}")
    return args[0]


def cmd_run(ctx, args):
    no_args(args)
    name = build(ctx)
    # The game's own exit code is rmp's: a crash is not a success.
    return ctx.run([ctx.exe(f"build/{name}")], check=False).returncode


def cmd_build(ctx, args):
    release = one_of(args, ("release",), "debug") == "release"
    name = build(ctx, release)
    print(f"  built  {ctx.exe(f'build/{name}').relative_to(ctx.root)}")
    return OK


def cmd_web(ctx, args):
    no_args(args)
    emsdk = os.environ.get("EMSDK", "")
    if not emsdk or not Path(emsdk).is_dir():
        raise Refused("the web build needs the emsdk: activate it first (source "
                      "emsdk_env.sh, or emsdk_env.ps1 on Windows), so that EMSDK is set")
    ctx.run(["cmake", "--preset", "web"])
    ctx.run(["cmake", "--build", "--preset", "web"])
    name = ctx.name()
    print(f"build/web/{name}.html -- serve it, do not open the file directly:")
    print(f"  {Path(ctx.python).name} -m http.server 8000 --directory build/web")
    return OK


def cmd_android(ctx, args):
    no_args(args)
    start = time.time()
    ctx.run([ctx.python, "tools/configure.py"])
    raymob = ctx.root / "raymob"
    if os.name == "nt":
        ctx.run([str(raymob / "gradlew.bat"), "assembleDebug"], cwd=raymob)
    else:
        # sh ./gradlew and not ./gradlew: a checkout without the exec bit
        # (a zip, a copy) still builds.
        ctx.run(["sh", "./gradlew", "assembleDebug"], cwd=raymob)
    apks = sorted(raymob.rglob("*.apk"))
    if not apks:
        raise Refused("Gradle finished and wrote no APK")
    for apk in apks:
        mark = "new " if apk.stat().st_mtime >= start - 1 else "    "
        print(f"  {mark} {apk.relative_to(ctx.root)}")
    return OK


def cmd_pack(ctx, args):
    no_args(args)
    reconfigure(ctx, "Debug")
    ctx.run(["cmake", "--preset", "debug"])
    ctx.run(["cmake", "--build", "build", "--target", "pack_resources"])
    return OK


def cmd_unpack(ctx, args):
    no_args(args)
    pack = ctx.root / "resources" / "resources.rres"
    if pack.exists():
        pack.unlink()
        print("  removed resources/resources.rres: the game reads resources/ again")
    else:
        print("  there was no pack: the game already reads resources/")
    return OK


def cmd_clean(ctx, args):
    no_args(args)
    for rel in CLEAN_PATHS:
        remove_tree(ctx.root / rel)
    print("clean. the next configure regenerates all of it.")
    return OK


def our_sources(ctx) -> list[Path]:
    files = []
    for root in FMT_ROOTS[ctx.mode]:
        for path in sorted((ctx.root / root).rglob("*")):
            rel = path.relative_to(ctx.root).as_posix()
            if path.suffix not in (".h", ".cpp", ".c") or "generated" in path.parts:
                continue
            if ctx.mode == "game" and rel.startswith(FMT_SKIP):
                continue
            files.append(path)
    return files


def fmt(ctx, check: bool) -> int:
    if shutil.which("clang-format") is None:
        raise Refused("clang-format is not installed (the version pinned in "
                      "thirdparty/FROZEN_VERSIONS.md)")
    files = our_sources(ctx)
    if not check:
        for i in range(0, len(files), 100):
            ctx.run(["clang-format", "-i", *files[i:i + 100]])
        print(f"formatted {len(files)} files")
        return OK
    bad = []
    for path in files:
        got = ctx.run(["clang-format", path], capture=True)
        if got.stdout != path.read_text(encoding="utf-8"):
            bad.append(path.relative_to(ctx.root).as_posix())
    for rel in bad:
        print(f"  unformatted  {rel}")
    if bad:
        raise Refused("run `rmp fmt` and commit the result")
    print("  ok    every file is formatted")
    return OK


def cmd_fmt(ctx, args):
    return fmt(ctx, check=one_of(args, ("check",), "write") == "check")


def cmd_lint(ctx, args):
    what = one_of(args, ("fix",), "check")
    ctx.run([ctx.bash(), "tools/lint.sh", what])
    if what == "fix":
        fmt(ctx, check=False)
    return OK


def cmd_test(ctx, args):
    stages = {s.name: s for s in stages_for(ctx.mode)}
    if len(args) > 1:
        raise Usage("one stage at a time; `rmp help test` names them")
    if args and args[0] not in stages:
        raise Usage(f"no stage called {args[0]!r}; `rmp help test` names them")
    chosen = all_stages(ctx.mode) if not args else [stages[args[0]]]
    for stage in chosen:
        print(f"== {stage.name} ==")
        for step in stage.steps:
            run_step(ctx, step)
    print("PASS")
    return OK


def examples(ctx) -> list[str]:
    found = set()
    for main in list(ctx.root.glob("examples/**/src/main.cpp")) + \
            list(ctx.root.glob("examples/**/src/main.c")):
        found.add(main.parent.parent.relative_to(ctx.root / "examples").as_posix())
    return sorted(found)


def cmd_example(ctx, args):
    every = examples(ctx)
    if not args or args == ["list"]:
        print("\n".join(every))
        return OK
    if len(args) > 1:
        raise Usage("one example at a time")
    want = args[0].removeprefix("examples/").rstrip("/")
    matches = [e for e in every if e == want] or \
        [e for e in every if e.endswith("/" + want)]
    if len(matches) != 1:
        raise Refused(f"no single example called {args[0]!r}. These exist:\n  "
                      + "\n  ".join(every))
    target = "example_" + matches[0].replace("/", "_")
    ctx.run(["cmake", "-S", ".", "-B", "build/examples", "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Debug", "-DPRODUCTION_BUILD=OFF", "-DRMP_BUILD_EXAMPLES=ON"],
            capture=True)
    ctx.run(["cmake", "--build", "build/examples", "--target", target])
    print(f"  built  build/examples/{target}")
    return ctx.run([ctx.exe(f"build/examples/{target}")], check=False).returncode


def live_runs(ctx) -> list[dict]:
    """The CI runs a push would cancel: every one not completed. cancel-in-
    progress kills queued, requested, pending and waiting runs too, and the BSD
    legs sit queued for minutes -- exactly when somebody pushes again."""
    if shutil.which("gh") is None:
        print("  note: gh is not installed, so a running CI could not be checked")
        return []
    got = ctx.run(["gh", "run", "list", "--workflow", "ci.yml", "--limit", "20",
                   "--json", "databaseId,status,event"], check=False, capture=True)
    if got.returncode != 0:
        print("  warning: gh could not list the CI runs; pushing without checking")
        return []
    try:
        runs = json.loads(got.stdout or "[]")
    except json.JSONDecodeError:
        print("  warning: gh answered something that is not JSON; pushing without checking")
        return []
    return [r for r in runs if r.get("status") != "completed"]


def push_starts_a_run(ctx) -> bool:
    """Whether the commit being pushed starts a CI run: [ci] on_push.

    Asked of configure.py, which refuses an invalid .toml and a ci.yml whose
    triggers disagree with it -- CI's first step would refuse that run too, so
    the push stops here. And of git: the run follows the ci.yml that is
    COMMITTED, so one rewritten for on_push = false and not committed yet still
    runs, and is checked like any other."""
    got = ctx.run([ctx.python, "tools/configure.py", "--print-ci"], check=False, capture=True)
    if got.returncode != 0:
        sys.stdout.write(got.stdout or "")
        raise Refused("tools/configure.py refused the project, and CI's first step would too")
    if "on_push=false" not in (got.stdout or "").splitlines():
        return True
    if git_out(ctx, "diff", "--quiet", "HEAD", "--", ".github/workflows/ci.yml").returncode != 0:
        print("  note: .github/workflows/ci.yml has changes that are not committed, and the")
        print("        commit being pushed may still run CI on a push: checking for a run")
        return True
    return False


def cmd_push(ctx, args):
    force = one_of(args, ("force",), "") == "force"
    if not force and not push_starts_a_run(ctx):
        # Nothing to cancel: concurrency cancels a run only by starting another.
        print("  [ci] on_push = false: a push starts no CI run, so none is checked.")
        print("  gh workflow run ci.yml     # runs the fast lane, when you want it")
    elif not force:
        live = live_runs(ctx)
        if live:
            print("a CI run is in flight, and pushing cancels it:")
            for r in live:
                print(f"  {r.get('databaseId')}  {r.get('status')}  {r.get('event')}")
            print()
            print(f"  gh run watch {live[0].get('databaseId')}")
            print("  rmp push force     # if killing it is what you meant")
            return FAILED
    # The first push of a branch sets its upstream. A game from `rmp new` has
    # `main` and, after `git remote add origin URL`, a remote, but nothing that
    # ties the two: a bare `git push` stops at "has no upstream branch".
    branch = git_out(ctx, "symbolic-ref", "-q", "--short", "HEAD").stdout.strip()
    if branch and git_out(ctx, "config", f"branch.{branch}.merge").returncode != 0:
        remotes = git_out(ctx, "remote").stdout.split()
        if "origin" in remotes:
            ctx.run(["git", "push", "-u", "origin", branch])
            return OK
        if not remotes:
            raise Refused("there is no remote to push to. Make the repository on GitHub, then:\n"
                          "  git remote add origin https://github.com/YOU/GAME.git\n"
                          "  rmp push")
    ctx.run(["git", "push"])
    return OK


def git_out(ctx, *argv) -> subprocess.CompletedProcess:
    return ctx.run(["git", *argv], check=False, capture=True)


def cmd_deploy(ctx, args):
    if len(args) != 1:
        raise Usage("rmp deploy VERSION, for example: rmp deploy 1.2.0")
    tag = args[0] if args[0].startswith("v") else "v" + args[0]

    # A tag on a dirty tree is a lie about what shipped: CI builds the commit.
    if git_out(ctx, "diff", "--quiet").returncode != 0 or \
            git_out(ctx, "diff", "--cached", "--quiet").returncode != 0:
        print(git_out(ctx, "status", "--short").stdout or "", end="")
        raise Refused("uncommitted changes. Commit or stash them first.")
    if git_out(ctx, "rev-parse", "-q", "--verify", f"refs/tags/{tag}").returncode == 0:
        raise Refused(f"the tag {tag} already exists here.\n"
                      f"  git tag -d {tag}     # if it was never pushed")
    remote = git_out(ctx, "ls-remote", "--exit-code", "--tags", "origin", tag)
    if remote.returncode == 0:
        raise Refused(f"{tag} is already on the remote. Releases are not re-cut; bump the version.")
    if remote.returncode != 2:
        raise Refused("could not ask origin whether the tag exists:\n"
                      + (remote.stdout or "").strip())

    # CI checks out the tag from the remote, so a tag on a commit that only
    # exists here builds nothing -- or the wrong thing.
    branch = git_out(ctx, "rev-parse", "--abbrev-ref", "HEAD").stdout.strip()
    if git_out(ctx, "rev-parse", "-q", "--verify", f"origin/{branch}").returncode != 0:
        raise Refused(f"origin/{branch} does not exist. Push the branch first:\n"
                      "  rmp push")
    head = git_out(ctx, "rev-parse", "HEAD").stdout.strip()
    upstream = git_out(ctx, "rev-parse", f"origin/{branch}").stdout.strip()
    if head != upstream:
        raise Refused(f"HEAD is not what origin/{branch} points at (as of your last fetch).\n"
                      "  rmp push     # then try again")

    # The gate CI runs on a tag: an invalid .toml, a tag that is not
    # vMAJOR.MINOR.PATCH and, in a game, a placeholder id (a Play id is
    # permanent) all fail here instead of twenty minutes and one dead tag
    # later. The framework's demo keeps its com.example id on purpose: no
    # store ever gets it, and every game starts from it.
    print(f"== checking {tag} the way CI will ==")
    strict = ["--strict-release"] if ctx.mode == "game" else []
    ctx.run([ctx.python, "tools/configure.py", "--print-config", *strict],
            env={"GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": tag}, capture=True)
    print(f"  ok    {tag} is release-ready")
    ctx.run(["git", "tag", "-a", tag, "-m", f"Release {tag}"])
    ctx.run(["git", "push", "origin", tag])
    print()
    print(f"pushed {tag}. CI builds every target and attaches them to the release.")
    print("  gh run watch")
    print()
    print("to undo, if you were quick enough:")
    print(f"  git push --delete origin {tag} && git tag -d {tag}")
    return OK


# ---------------------------------------------------------------------------
# rmp new: a game, made from this framework
# ---------------------------------------------------------------------------
#
# The copy is an explicit list of what a game needs, not "everything except".
# An exclude list fails silently and for good: the next gate, fixture or
# canary script added to the framework would ride along into every game and
# nothing would say so. This one fails loudly instead -- a file a game needs
# and does not get is a game that does not build in the rmp_new CI job -- and
# tests/rmp_test.py makes every file the framework tracks be classified here,
# so a new one forces the question. The longest pattern that matches wins.

INCLUDE = (
    ".clang-format", ".clang-format-ignore", ".clangd", ".gitattributes", ".gitignore",
    ".gitmodules", ".github/scripts/web_boot_test.js",
    ".github/workflows/ci.yml", ".github/workflows/_android.yml",
    ".github/workflows/_apple.yml", ".github/workflows/_bsd.yml",
    ".github/workflows/_firebase.yml", ".github/workflows/_itch.yml",
    ".github/workflows/_linux.yml", ".github/workflows/_release.yml",
    ".github/workflows/_web.yml", ".github/workflows/_windows.yml",
    "CMakeLists.txt", "CMakePresets.json", "THIRD_PARTY_LICENSES.md", TOML,
    "branding/", "cmake/configure_hook.cmake", "cmake/find_python.cmake",
    "cmake/game_resources.cmake",
    "cmake/toolchain-riscv64-linux.cmake", "cmake/web/",
    "generate_android_commands.ps1", "generate_android_commands.sh",
    "update_clangd.ps1", "update_clangd.sh",
    "include/", "ios/ANGLE-LICENSE.txt", "ios/README.md",
    "package.json", "package-lock.json", "raymob/", "resources/", "src/",
    "tests/smoke_test.h", "thirdparty/",
    "tools/configure.py", "tools/license_db.py", "tools/rres_pack.c", "tools/md5.c",
    "tools/md5.h", "tools/linux_build.sh", "tools/glibc_check.sh", "tools/upx_pack.sh",
    "tools/render_check.sh", "tools/shipped_check.sh", "tools/ship_resources.sh",
    "tools/versions_check.sh",
    "tools/dev_shell.sh",
    "tools/android_release_check.py", "tools/binary_check.py", "tools/rmp.py",
    "rmp", "rmp.ps1", "rmp.cmd",
)

# The framework's MIT licence travels as a component of the game, where every
# other one lives: its row in THIRD_PARTY_LICENSES.md puts it in LICENSES.txt.
# Not a LICENSE at the root, which would make GitHub call every game MIT.
RENAME = {"LICENSE": "thirdparty/raylib_multiplatform/LICENSE"}

GITLINKS = ("thirdparty/raylib-ios",)

FRAMEWORK_ONLY = {
    "README.md": "a game gets its own, written by rmp new",
    "TECHNICAL.md": "the framework's notes for whoever maintains it",
    "CLAUDE.md": "the framework's instructions for an agent working on it",
    ".claude/": "the framework's agent skills",
    ".clang-tidy": "the framework's lint rules, run by its own lint job",
    ".github/dependabot.yml": "bumps the framework's pins; a game takes them from the framework",
    ".github/known-breakage.md": "the framework's canary",
    ".github/scripts/": "the framework's canary scripts",
    ".github/workflows/": "the framework's canary, autofix and web-backends workflows",
    "examples/": "the framework's examples",
    "tests/": "the framework's tests",
    "thirdparty/doctest/": "the framework's unit-test library",
    "tools/": "the framework's gates and generators",
}


def classify(path: str):
    """(kind, pattern) for a tracked path: the longest pattern that matches."""
    best = None
    for kind, patterns in (("include", INCLUDE), ("rename", tuple(RENAME)),
                           ("gitlink", GITLINKS), ("framework", tuple(FRAMEWORK_ONLY))):
        for pattern in patterns:
            hit = path == pattern or (pattern.endswith("/") and path.startswith(pattern))
            if hit and (best is None or len(pattern) > len(best[1])):
                best = (kind, pattern)
    return best


def tracked(framework: Path):
    """(mode, sha, path) for every path the framework's index holds."""
    got = subprocess.run(["git", "-C", str(framework), "ls-files", "-s", "-z"],
                         capture_output=True, text=True)
    if got.returncode != 0:
        raise Refused("rmp new needs a git clone of the framework: " + got.stderr.strip())
    out = []
    for entry in got.stdout.split("\0"):
        if not entry:
            continue
        meta, path = entry.split("\t", 1)
        mode, sha, _stage = meta.split()
        out.append((mode, sha, path))
    return out


def load_configure(framework: Path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("rmp_new_configure",
                                                  framework / "tools" / "configure.py")
    module = importlib.util.module_from_spec(spec)
    sys.path.insert(0, str(framework / "tools"))
    try:
        spec.loader.exec_module(module)
    finally:
        sys.path.remove(str(framework / "tools"))
    return module


def edit_lines(path: Path, edits) -> None:
    """Apply (match, replace) pairs to a file, line by line, line endings kept.
    `match(line)` must be true of exactly one line, or nothing is written:
    a template that drifted is a bug to report, not to paper over. `replace`
    returns the new line, or None to delete it."""
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    for match, replace in edits:
        hits = [i for i, line in enumerate(lines) if match(line)]
        if len(hits) != 1:
            raise Refused(f"rmp new: expected one line to change in {path.name}, "
                          f"found {len(hits)}. This is a bug in the framework.")
        new = replace(lines[hits[0]])
        if new is None:
            del lines[hits[0]]
        else:
            lines[hits[0]] = new
    path.write_text("".join(lines), encoding="utf-8")


def drop_bullet(path: Path, start: str) -> None:
    """Remove one `- **x**` bullet from a Markdown list, all of its lines."""
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    first = [i for i, line in enumerate(lines) if line.startswith(start)]
    if len(first) != 1:
        raise Refused(f"rmp new: expected one bullet starting {start!r} in {path.name}")
    end = first[0] + 1
    while end < len(lines) and lines[end].startswith("  "):
        end += 1
    del lines[first[0]:end]
    path.write_text("".join(lines), encoding="utf-8")


def game_readme(name: str) -> str:
    return f"""# {name}

A game made with [raylib_multiplatform]({FRAMEWORK_URL}).

    rmp run          build it and play it
    rmp test         check it: the config, the boot, the pixels
    rmp help         everything else

What is yours:

    src/main.cpp     RMP_GAME(MainMenuScene); -- the first scene, and nothing else
    src/scenes/      the game: a scene per screen, starting with the main menu
    include/         your headers, when the game has some
    resources/       the art, sounds, fonts and levels the game loads
    branding/icon.png  the app icon on Android, iOS and Windows
    {TOML}  the name, the app ids, the platforms

`src/main.cpp` names the first scene with `RMP_GAME`, and the game is in
`src/scenes/` -- an object of your own goes in a folder of its own beside it.
Every .cpp under `src/` is built. The rest is `include/` for your headers,
`resources/`, `branding/icon.png` and `{TOML}`. `src/rmp/` and
`include/rmp/` are the framework: you include its headers and never edit it.
"""


def setting(key: str):
    """A matcher for `key = ...` at the start of a line."""
    return lambda line: line.split("=", 1)[0].strip() == key and "=" in line


def setting_in(section: str, key: str):
    """A matcher for `key = ...` inside `[section]` only, for a key that more
    than one table has -- `enabled` is in [targets], [upx] and [android.admob].
    edit_lines() asks it about every line in order, so it can follow the
    headers as it goes."""
    state = {"section": ""}

    def match(line: str) -> bool:
        stripped = line.strip()
        if stripped.startswith("[") and "]" in stripped:
            state["section"] = stripped[1:stripped.index("]")].strip()
            return False
        return state["section"] == section and setting(key)(line)
    return match


def assign_raw(value: str):
    """Replace an unquoted value -- `true`, a number -- keeping any comment."""
    def replace(line: str) -> str:
        head, rest = line.split("=", 1)
        comment = rest[rest.index("#"):] if "#" in rest else ""
        end = "\n" if line.endswith("\n") else ""
        if comment:
            return f"{head}= {value}  {comment.rstrip()}{end}"
        return f"{head}= {value}{end}"
    return replace


def assign(value: str):
    """Replace the value of `key = "..."`, keeping the alignment and any comment."""
    def replace(line: str) -> str:
        head, rest = line.split("=", 1)
        quote_end = rest.index('"', rest.index('"') + 1)
        return f'{head}= "{value}"{rest[quote_end + 1:]}'
    return replace


def cmd_new(_ctx, args):
    if len(args) != 1:
        raise Usage("rmp new DIR, for example: rmp new my_game")
    framework = Path(__file__).resolve().parents[1]
    if mode_of(framework) != "framework":
        raise Refused(f"this rmp belongs to the game in {framework}; `rmp new` needs the "
                      "framework's rmp on PATH")
    if args == ["--list"]:
        # What a new game gets, without making one.
        for path in new_game_paths(tracked(framework)):
            print(path)
        return OK
    if shutil.which("git") is None:
        raise Refused("rmp new needs git")
    top = subprocess.run(["git", "-C", str(framework), "rev-parse", "--show-toplevel"],
                         capture_output=True, text=True)
    if top.returncode != 0 or Path(top.stdout.strip()).resolve() != framework:
        raise Refused(f"rmp new needs a git clone of the framework, and {framework} is not one")

    target = (Path.cwd() / args[0]).resolve()
    name = target.name
    configure = load_configure(framework)
    if not configure.NAME_RE.match(name) or name in configure.RESERVED_NAMES or \
            name.lower() in configure.WINDOWS_DEVICES:
        suggestion = "".join(c if c.isalnum() or c in "_-" else "_" for c in name)
        if not suggestion[:1].isalpha():
            suggestion = "game_" + suggestion
        raise Usage(f"{name!r} cannot be a game's name -- it becomes the executable on five "
                    f"operating systems. Letters, digits, _ and -, starting with a letter; "
                    f"for instance {suggestion!r}.")
    if target == framework or framework in target.parents:
        raise Refused(f"{target} is inside the framework; make the game somewhere else")
    if target.exists() and not target.is_dir():
        raise Refused(f"{target} exists and is not a folder")
    if target.is_dir() and any(target.iterdir()):
        raise Refused(f"{target} is not empty")

    # Android ids take `_` and not `-`; Apple bundle ids take `-` and not `_`.
    app_id = "com.example." + name.lower().replace("-", "_")
    bundle_id = "com.example." + name.lower().replace("_", "-")
    assert configure.APPID_RE.match(app_id) and configure.BUNDLE_RE.match(bundle_id)

    entries = tracked(framework)
    for _mode, _sha, path in entries:
        if classify(path) is None:
            raise Refused(f"{path} is not in rmp new's manifest. This is a bug in the framework.")

    created = not target.exists()
    target.mkdir(parents=True, exist_ok=True)
    try:
        make_game(framework, target, name, app_id, bundle_id, entries)
    except BaseException:
        if created:
            remove_tree(target)
        else:
            for child in target.iterdir():
                remove_tree(child)
        raise

    pin = next(sha for mode, sha, path in entries if path == GITLINKS[0])
    count = sum(1 for _ in target.rglob("*") if _.is_file() and ".git" not in _.parts)
    print(f"created {target.name}/ from raylib_multiplatform "
          f"({count} files; raylib-iOS pinned at {pin[:8]})")
    print()
    print(f"  [project] name     {name:12} the executable, save folder and store names")
    print(f"  android id         {app_id}")
    print(f"  ios bundle id      {bundle_id}")
    print()
    print("  Those ids are placeholders, and a tag build refuses them. Set real ones in")
    print(f"  {TOML} before your first release: a Google Play id is")
    print("  permanent.")
    print()
    print("next:")
    print(f"  cd {args[0]}")
    print("  rmp run")
    print('  git commit -m "New game"')
    print("  git submodule update --init      only to build for iOS")
    print()
    print("The game has no licence of its own; choosing one is yours. The framework's")
    print("MIT notice ships with every build, in LICENSES.txt.")
    return OK


def new_game_paths(entries) -> list[str]:
    """Every path a new game's index holds: the manifest's, renamed where it
    says, plus the README rmp new writes and the submodule."""
    want = {RENAME.get(p, p) for m, s, p in entries if classify(p)[0] in ("include", "rename")}
    want |= {"README.md", GITLINKS[0]}
    return sorted(want)


def make_game(framework: Path, target: Path, name: str, app_id: str, bundle_id: str,
              entries) -> None:
    executable = []
    for mode, _sha, path in entries:
        kind, _pattern = classify(path)
        if kind not in ("include", "rename"):
            continue
        dest = target / (RENAME[path] if kind == "rename" else path)
        source = framework / path
        if not source.is_file():
            raise Refused(f"{path} is tracked and missing from the framework's checkout:\n"
                          f"  git -C {framework} checkout -- {path}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, dest)
        if mode == "100755":
            executable.append(dest.relative_to(target).as_posix())

    edit_lines(target / TOML, [
        (setting("name"), assign(name)),
        (setting("title"), assign(name)),
        (setting("application_id"), assign(app_id)),
        (setting("bundle_id"), assign(bundle_id)),
        # Obfuscation, as the .toml says -- and one shared password across
        # every game made from this framework would be less than that.
        (setting("rres_password"), assign(secrets_token())),
        # Ads are opt-in. The framework keeps them on so its CI builds the ads
        # path; a game starts without the SDK, AD_ID and INTERNET, and turns
        # them on when it has an ad to show.
        (setting_in("android.admob", "enabled"), assign_raw("false")),
        # ES 2.0, which every Android device has. The framework compiles the
        # ES 3.0 path in its own CI; a game asks for it when it needs it.
        (setting_in("android", "gl_version"), assign("ES20")),
    ])
    edit_lines(target / "THIRD_PARTY_LICENSES.md", [
        (lambda line: line.startswith("doctest "), lambda line: None),
        (lambda line: line.startswith("raylib_multiplatform "),
         lambda line: line.replace("LICENSE                     ",
                                   "thirdparty/raylib_multiplatform", 1)),
    ])
    drop_bullet(target / "THIRD_PARTY_LICENSES.md", "- **doctest**")
    edit_lines(target / "thirdparty" / "FROZEN_VERSIONS.md", [
        (lambda line: line.startswith("sha256_doctest "), lambda line: None),
        (lambda line: line.startswith("| doctest |"), lambda line: None),
    ])
    (target / "README.md").write_text(game_readme(name), encoding="utf-8")
    # The submodule's folder, empty, as a clone without --recursive has it:
    # without it, `git add -A` would stage the gitlink's deletion.
    (target / GITLINKS[0]).mkdir(parents=True, exist_ok=True)

    env = dict(os.environ, GIT_TERMINAL_PROMPT="0")

    def git(*argv):
        got = subprocess.run(["git", "-C", str(target), *argv], capture_output=True,
                             text=True, env=env)
        if got.returncode != 0:
            raise Refused(f"git {' '.join(argv)} failed: {got.stderr.strip()}")
        return got.stdout

    git("init", "-q", "-b", "main")
    git("add", "-A")
    pin = next(sha for mode, sha, path in entries if path == GITLINKS[0])
    git("update-index", "--add", "--cacheinfo", f"160000,{pin},{GITLINKS[0]}")
    # Windows has no exec bit to read, so the index is told: rmp, gradlew and
    # the scripts are executable in the game's history too.
    for rel in executable:
        git("update-index", "--chmod=+x", rel)

    # Nothing but what the manifest says, and everything it says.
    want = set(new_game_paths(entries))
    have = set(git("ls-files", "-z").split("\0")) - {""}
    if have != want:
        missing, extra = sorted(want - have), sorted(have - want)
        raise Refused(f"the new game's index is not the manifest: missing {missing[:5]}, "
                      f"extra {extra[:5]}. This is a bug in the framework.")

    # The game's own tools say it is valid, or there is no game.
    for check in (["tools/configure.py", "--check"], ["tools/license_db.py", "--check"]):
        got = subprocess.run([sys.executable, *check], cwd=str(target), capture_output=True,
                             text=True)
        if got.returncode != 0:
            raise Refused(f"the new game failed {' '.join(check)}:\n{got.stdout}{got.stderr}")


def secrets_token() -> str:
    import secrets
    return secrets.token_urlsafe(18)


def cmd_help(ctx_or_none, args):
    mode = ctx_or_none.mode if ctx_or_none else "game"
    if args == ["--json"]:
        # Every command and every test stage, framework-only ones marked: what
        # the docs site checks every rmp command line it shows against.
        print(json.dumps({
            "commands": [{"name": name, "usage": c.usage, "summary": c.summary,
                          "sentence": c.sentence, "framework_only": c.framework_only,
                          "examples": [{"line": line, "note": note} for line, note in c.examples]}
                         for name, c in COMMANDS.items()],
            "stages": [{"name": s.name, "summary": s.summary, "scope": s.scope,
                        "in_all": s.in_all} for s in STAGES],
        }, indent=1))
        return OK
    if not args:
        print(usage_text(mode))
        return OK
    if len(args) > 1 or args[0] not in COMMANDS:
        raise Usage(f"no command called {' '.join(args)!r}; `rmp help` lists them")
    print(page(args[0], mode))
    return OK


# ---------------------------------------------------------------------------
# Help, as data. Every command has a page; tests/rmp_test.py checks that every
# example line on it parses as a real command line.
# ---------------------------------------------------------------------------

class Command:
    def __init__(self, usage, summary, sentence, examples, handler,
                 framework_only=False, needs_project=True):
        self.usage = usage
        self.summary = summary
        self.sentence = sentence
        self.examples = examples
        self.handler = handler
        self.framework_only = framework_only
        self.needs_project = needs_project


COMMANDS = {
    "run": Command(
        "run", "build the game (debug) and play it",
        "Build the game in debug, then play it; only what changed is compiled again.",
        [("rmp run", "build, then start the game"),
         ("rmp build release", "the optimised build players get"),
         ("rmp clean", "if a build goes strange, start from nothing")],
        cmd_run),
    "build": Command(
        "build [release]", "compile the game: debug, or release",
        "Compile the game without running it: debug by default, or release "
        "(optimised, reading ./resources/: the one next to the executable when "
        "there is one, as in a shipped package, and the working directory's "
        "otherwise -- so run build/<name> from the project folder).",
        [("rmp build", "writes build/<name>"),
         ("rmp build release", "the release build, as CI ships it")],
        cmd_build),
    "web": Command(
        "web", "build for the browser (needs the emsdk)",
        "Build build/web/<name>.html with Emscripten; activate the emsdk first, "
        "so that EMSDK is set.",
        [("rmp web", "writes build/web/<name>.html"),
         ("python3 -m http.server -d build/web", "then open localhost:8000/<name>.html")],
        cmd_web),
    "android": Command(
        "android", "build the Android APK (needs the SDK and NDK)",
        "Build a debug APK with Gradle; it needs the Android SDK and NDK.",
        [("rmp android", "prints the APK it wrote"),
         ("adb install -r <apk>", "puts it on a connected phone")],
        cmd_android),
    "test": Command(
        "test [stage]", "check the project; `rmp help test` names the stages",
        "Check the project. With no stage, every one of them; name a stage to "
        "run only that one.",
        [("rmp test", "everything"),
         ("rmp test smoke", "build, boot and see it draw"),
         ("rmp test config", "the .toml, the pinned versions, the licences")],
        cmd_test),
    "fmt": Command(
        "fmt [check]", "format your C and C++ with clang-format",
        "Format your C and C++ with clang-format -- the version pinned in "
        "thirdparty/FROZEN_VERSIONS.md.",
        [("rmp fmt", "rewrite what needs it"),
         ("rmp fmt check", "only report what it would change")],
        cmd_fmt),
    "pack": Command(
        "pack", "bundle resources/ into resources.rres, as a release does",
        "Bundle resources/ into resources/resources.rres, encrypted, the way a "
        "release ships it.",
        [("rmp pack", "the game reads the pack from now on"),
         ("rmp unpack", "back to the loose files while you add art")],
        cmd_pack),
    "unpack": Command(
        "unpack", "delete the pack; the game reads resources/ again",
        "Delete resources/resources.rres, so that the game reads the loose files "
        "in resources/ again.",
        [("rmp unpack", "loose files again"),
         ("rmp pack", "bundle them again")],
        cmd_unpack),
    "clean": Command(
        "clean", "delete build/ and everything generated from the .toml",
        "Delete build/ and every file generated from raylib_multiplatform.toml; "
        "the next build makes them again.",
        [("rmp clean", "everything, gone"),
         ("rmp run", "rebuild from nothing, and play")],
        cmd_clean),
    "push": Command(
        "push [force]", "git push, unless it would cancel a running CI",
        "Run git push -- refused while a CI run is in flight, because the push "
        "would cancel it; with [ci] on_push = false a push starts no run, and "
        "nothing is checked. The first push of a branch sets its upstream on origin.",
        [("rmp push", "push, or say which run it would kill"),
         ("rmp push force", "push anyway, and cancel the run")],
        cmd_push),
    "deploy": Command(
        "deploy VERSION", "tag a release and push it; CI builds and publishes it",
        "Release a version: check it the way CI will, tag it, push the tag. CI "
        "builds every target and attaches them to the release.",
        [("rmp deploy 1.2.0", "tags v1.2.0 (the v is added)"),
         ("rmp deploy 1.2.0-rc1", "a pre-release, marked as one"),
         ("rmp push", "first, if your last commit is not on GitHub yet")],
        cmd_deploy),
    "new": Command(
        "new DIR", "make a new game in DIR",
        "Make a new game in DIR: a copy of what a game needs from this framework, "
        "named after the folder, ready to build and to ship.",
        [("rmp new my_game", "makes my_game/, here"),
         ("rmp new ~/games/space_rocks", "anywhere; the name is the folder's"),
         ("rmp new --list", "what a new game gets, without making one")],
        cmd_new, needs_project=False),
    "help": Command(
        "help [command]", "this list, or one command in detail",
        "Show the commands, or one command in detail.",
        [("rmp help", "every command"),
         ("rmp help deploy", "one of them"),
         ("rmp help --json", "every command and test stage, for a tool to read")],
        cmd_help, needs_project=False),
    "lint": Command(
        "lint [fix]", "clang-tidy over src/, tests/ and examples/",
        "Run clang-tidy over src/, tests/ and examples/; every warning is an error.",
        [("rmp lint", "report"),
         ("rmp lint fix", "apply what clang-tidy is sure of, then format")],
        cmd_lint, framework_only=True),
    "example": Command(
        "example [name]", "build and run one example; no name lists them",
        "Build and run one example from examples/, by name or by path.",
        [("rmp example", "list them"),
         ("rmp example 01_pong", "a bare name, when it is unique"),
         ("rmp example games/01_pong", "or the path under examples/")],
        cmd_example, framework_only=True),
}


def usage_text(mode: str) -> str:
    lines = ["usage: rmp <command> [args]", ""]
    width = max(len(c.usage) for c in COMMANDS.values()) + 2
    for c in COMMANDS.values():
        if not c.framework_only:
            lines.append(f"  {c.usage:{width}} {c.summary}")
    if mode == "framework":
        lines += ["", "framework only:"]
        for c in COMMANDS.values():
            if c.framework_only:
                lines.append(f"  {c.usage:{width}} {c.summary}")
    lines += ["", "rmp help <command> explains one, with examples."]
    return "\n".join(lines)


def page(name: str, mode: str) -> str:
    c = COMMANDS[name]
    lines = [f"rmp {c.usage}", *textwrap.wrap(c.sentence, 78), ""]
    width = max(len(cmdline) for cmdline, _ in c.examples) + 2
    for cmdline, note in c.examples:
        lines.append(f"  {cmdline:{width}} {note}")
    if name == "test":
        lines += ["", "stages (all of them, in this order, unless one is named):"]
        for s in stages_for(mode):
            if s.in_all:
                lines.append(f"  {s.name:15} {s.summary}")
        rest = [s for s in stages_for(mode) if not s.in_all]
        if rest:
            lines += ["", "only by name:"]
            for s in rest:
                lines.append(f"  {s.name:15} {s.summary}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

HELP_FLAGS = ("-h", "--help")


def delegate(root: Path, argv: list[str]) -> int | None:
    """Run the project's own tools/rmp.py when it is not this file."""
    theirs = root / "tools" / "rmp.py"
    if os.environ.get("RMP_DELEGATED") or os.environ.get("RMP_NO_DELEGATE"):
        return None
    if not theirs.is_file() or theirs.resolve() == Path(__file__).resolve():
        return None
    env = dict(os.environ, RMP_DELEGATED="1")
    return subprocess.run([sys.executable, str(theirs), *argv], env=env).returncode


def main(argv: list[str], cwd: Path | None = None) -> int:
    cwd = cwd or Path.cwd()
    if argv[:1] == ["--mode"]:
        root = find_root(cwd)
        if root is None:
            print(f"rmp: not inside a project (no {TOML} here or above {cwd})", file=sys.stderr)
            return USAGE
        print(mode_of(root))
        return OK

    root = find_root(cwd)
    command = argv[0] if argv else "help"
    if root is not None and command != "new":
        delegated = delegate(root, argv)
        if delegated is not None:
            return delegated
    ctx = Ctx(root) if root is not None else None
    mode = ctx.mode if ctx else "game"

    if command in HELP_FLAGS:
        command, argv = "help", ["help"]
    rest = argv[1:]
    if command not in COMMANDS:
        guess = difflib.get_close_matches(command, list(COMMANDS), n=1)
        hint = f" (did you mean {guess[0]}?)" if guess else ""
        print(f"rmp: unknown command {command!r}{hint}. Run rmp help.", file=sys.stderr)
        return USAGE
    spec = COMMANDS[command]
    if command != "help" and any(a in HELP_FLAGS for a in rest):
        print(page(command, mode))
        return OK
    if spec.framework_only and mode != "framework":
        where = f" ({root})" if root else ""
        print(f"rmp: {command} is a framework command; this is a game{where}.", file=sys.stderr)
        return USAGE
    if spec.needs_project and ctx is None:
        print(f"rmp: not inside a project (no {TOML} here or above {cwd}). "
              "`rmp new DIR` makes one.", file=sys.stderr)
        return USAGE
    try:
        return spec.handler(ctx, rest)
    except Usage as e:
        print(f"rmp {command}: {e}", file=sys.stderr)
        return USAGE
    except Refused as e:
        print(f"FAIL: {e}")
        return FAILED
    except KeyboardInterrupt:
        return INTERRUPTED


if __name__ == "__main__":
    # Line by line, so what rmp says and what the tools it runs say come out in
    # the order they happened.
    sys.stdout.reconfigure(line_buffering=True)
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(INTERRUPTED)
    except BrokenPipeError:
        # `rmp new --list | head`: the reader left; there is nobody to tell.
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())
        sys.exit(OK)
