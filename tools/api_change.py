#!/usr/bin/env python3
"""The API-change gate: what a pull request changes of the framework's API.

The API is what every game is written against, and changing it is Omar's
decision, not the author's (CLAUDE.md, "How an agent works here"). Two things
are the API here:

  - a public header: any change to a file under include/, a comment included
    -- a header's comment is the promise a game reads;
  - a key of raylib_multiplatform.toml: a key of DEFAULTS in
    tools/configure.py, or its default (read from the source, never run), and
    a key of the framework's own raylib_multiplatform.toml -- its keys, not
    its values, which are the demo game's own settings. [ios.settings] is the
    game's own table, passed to Xcode as written: its keys are not ours.

    python3 tools/api_change.py                     this branch, against origin/main
    python3 tools/api_change.py --base A --head B   any two commits
    python3 tools/api_change.py --base HEAD^1 --head HEAD --pr N
                                                    the `api` job, on a merge commit

Compared from the merge base of the two, as a pull request is: what main did
after the branch began is not this change.

With --pr -- the `api` job of .github/workflows/ci.yml, on a pull request --
a change labels the pull request api-change and fails unless it also carries
api-approved, added by the repository's owner: the issue's events say who
added it, and anybody else's is refused. Without --pr it only says what it
found, and passes.

Exit status: 0 nothing to approve, or approved; 1 an API change waiting for
approval; 2 a question it could not answer (a git or gh call failed, DEFAULTS
is not a literal, the .toml does not parse) -- never a pass by default.

Standard library and git; --pr needs the gh CLI, GH_TOKEN, GITHUB_REPOSITORY
and GITHUB_REPOSITORY_OWNER, as the job gives them.
"""

from __future__ import annotations

import argparse
import ast
import json
import os
import subprocess
import sys
import tomllib

CHANGE_LABEL = "api-change"
APPROVED_LABEL = "api-approved"

PUBLIC = "include/"
CONFIGURE = "tools/configure.py"
TOML = "raylib_multiplatform.toml"
# configure.py's deep_merge() passes these tables through as the game wrote
# them, so a key in one is the game's, not the framework's.
FREE_FORM = ("ios.settings",)

STATUS = {"A": "added", "C": "copied", "D": "deleted", "M": "modified", "R": "renamed",
          "T": "changed type"}


class Unsure(Exception):
    """A question the gate could not answer. Never read as a pass."""


def git(*argv: str, cwd: str | None = None) -> str:
    got = subprocess.run(["git", *argv], cwd=cwd, capture_output=True, text=True)
    if got.returncode != 0:
        raise Unsure(f"git {' '.join(argv)}: {got.stderr.strip() or 'failed'}")
    return got.stdout


def show(rev: str, path: str, cwd: str | None = None) -> str | None:
    """A file's text at a commit, or None where the commit has no such file.
    The commit itself was verified first, so a failure here is the path."""
    got = subprocess.run(["git", "show", f"{rev}:{path}"], cwd=cwd, capture_output=True,
                         text=True)
    return got.stdout if got.returncode == 0 else None


def defaults_of(source: str | None, where: str) -> dict:
    """DEFAULTS, as configure.py's source assigns it: the literal, never the
    module run -- the head of a pull request is not code to execute here."""
    if source is None:
        return {}
    try:
        tree = ast.parse(source)
    except SyntaxError as e:
        raise Unsure(f"{where} does not parse: {e}") from None
    for node in tree.body:
        if isinstance(node, ast.AnnAssign):
            target = node.target
        elif isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
        else:
            continue
        if not (isinstance(target, ast.Name) and target.id == "DEFAULTS") or node.value is None:
            continue
        try:
            value = ast.literal_eval(node.value)
        except ValueError:
            raise Unsure(f"DEFAULTS in {where} is not a literal, so its keys cannot be read "
                         "without running it") from None
        if not isinstance(value, dict):
            raise Unsure(f"DEFAULTS in {where} is not a table")
        return value
    raise Unsure(f"{where} assigns no DEFAULTS: the keys of the .toml are not where this "
                 "gate reads them")


def leaves(table: dict, prefix: str = "") -> dict:
    """{"window.fps": 0, ...}: every key that holds a value, by its path. An
    empty table and a free-form one are values themselves."""
    out = {}
    for key, value in table.items():
        path = f"{prefix}.{key}" if prefix else str(key)
        if isinstance(value, dict) and value and path not in FREE_FORM:
            out.update(leaves(value, path))
        else:
            out[path] = value
    return out


def toml_keys(source: str | None, where: str) -> set[str]:
    if source is None:
        return set()
    try:
        return set(leaves(tomllib.loads(source)))
    except tomllib.TOMLDecodeError as e:
        raise Unsure(f"{where} is not valid TOML: {e}") from None


def spelled(value) -> str:
    """A default as the .toml writes it: true, "x", [1, 2]."""
    return json.dumps(value)


def changes(base: str, head: str, cwd: str | None = None) -> list[str]:
    """What `head` changes of the API since its merge base with `base`, one
    line each; [] when nothing."""
    for rev in (base, head):
        got = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{rev}^{{commit}}"],
                             cwd=cwd, capture_output=True, text=True)
        if got.returncode != 0:
            raise Unsure(f"{rev} is not a commit of this repository (fetched deep enough?)")
    since = git("merge-base", base, head, cwd=cwd).strip()
    out = []
    for line in git("diff", "--name-status", "-M", since, head, "--", PUBLIC,
                    cwd=cwd).splitlines():
        status, *paths = line.split("\t")
        what = STATUS.get(status[:1], status)
        if len(paths) == 2:
            out.append(f"{paths[1]}: {what} from {paths[0]}")
        else:
            out.append(f"{paths[0]}: {what}")

    at = f"{CONFIGURE} at {since[:12]}", f"{CONFIGURE} at {head}"
    old = leaves(defaults_of(show(since, CONFIGURE, cwd), at[0]))
    new = leaves(defaults_of(show(head, CONFIGURE, cwd), at[1]))
    where = f"(DEFAULTS in {CONFIGURE})"
    for key in sorted(old.keys() | new.keys()):
        if key not in old:
            out.append(f"[{key}]: a new key, default {spelled(new[key])} {where}")
        elif key not in new:
            out.append(f"[{key}]: no longer a key {where}")
        elif spelled(old[key]) != spelled(new[key]):
            out.append(f"[{key}]: its default was {spelled(old[key])} and is "
                       f"{spelled(new[key])} {where}")

    before = toml_keys(show(since, TOML, cwd), f"{TOML} at {since[:12]}")
    after = toml_keys(show(head, TOML, cwd), f"{TOML} at {head}")
    out += [f"[{key}]: a new key in {TOML}" for key in sorted(after - before)]
    out += [f"[{key}]: no longer in {TOML}" for key in sorted(before - after)]
    return out


# ---------------------------------------------------------------------------
# The pull request: its labels, and who added them
# ---------------------------------------------------------------------------

def gh(*argv: str, stdin: str | None = None) -> str:
    got = subprocess.run(["gh", *argv], input=stdin, capture_output=True, text=True)
    if got.returncode != 0:
        raise Unsure(f"gh {' '.join(argv[:4])}: "
                     f"{got.stderr.strip() or got.stdout.strip() or 'failed'}")
    return got.stdout


def documents(text: str) -> list:
    """Every item of every page `gh api --paginate` printed: one JSON array a
    page, written one after the other."""
    decoder = json.JSONDecoder()
    out: list = []
    i, text = 0, text.strip()
    while i < len(text):
        page, i = decoder.raw_decode(text, i)
        if not isinstance(page, list):
            raise Unsure(f"gh api answered something that is not a list: {text[:200]}")
        out.extend(page)
        while i < len(text) and text[i].isspace():
            i += 1
    return out


def approval(pr: str, repo: str, owner: str) -> tuple[bool, str]:
    """Label the pull request api-change, and say whether the owner approved
    it: (approved, why)."""
    issue = f"repos/{repo}/issues/{pr}"
    labels = [label.get("name") for label in documents(gh("api", f"{issue}/labels?per_page=100"))]
    if CHANGE_LABEL not in labels:
        gh("api", "--method", "POST", f"{issue}/labels", "--input", "-",
           stdin=json.dumps({"labels": [CHANGE_LABEL]}))
    if APPROVED_LABEL not in labels:
        return False, f"it does not carry {APPROVED_LABEL}"
    events = documents(gh("api", "--paginate", f"{issue}/events?per_page=100"))
    added = [e for e in events if e.get("event") == "labeled"
             and (e.get("label") or {}).get("name") == APPROVED_LABEL]
    if not added:
        raise Unsure(f"the pull request carries {APPROVED_LABEL} and no event of it says "
                     "who added it")
    who = str((added[-1].get("actor") or {}).get("login") or "somebody GitHub no longer names")
    if who.casefold() != owner.casefold():
        return False, (f"{APPROVED_LABEL} was added by {who}, and only {owner}, the "
                       "repository's owner, approves an API change")
    return True, f"{APPROVED_LABEL} was added by {who}"


def report(found: list[str], verdict: str, summary: str | None) -> None:
    lines = ["This changes the framework's API:", *(f"  {line}" for line in found), "",
             verdict]
    print("\n".join(lines))
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write("### The API-change gate\n\n" + "\n".join(
                [f"- `{line}`" for line in found] + ["", verdict.replace("\n", " "), ""]))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--base", default="origin/main",
                        help="the branch the change goes into (default: origin/main)")
    parser.add_argument("--head", default="HEAD", help="the change (default: HEAD)")
    parser.add_argument("--pr", help="the pull request's number: label it, and fail "
                                     "without the owner's approval")
    args = parser.parse_args(argv)
    try:
        found = changes(args.base, args.head)
        if not found:
            print("PASS: no public header and no key of the .toml changed.")
            return 0
        if args.pr is None:
            report(found, "A pull request of this is labelled api-change, and its `api` "
                          f"check fails until Omar adds {APPROVED_LABEL}.", None)
            return 0
        repo = os.environ.get("GITHUB_REPOSITORY", "")
        owner = os.environ.get("GITHUB_REPOSITORY_OWNER", "")
        if not repo or not owner:
            raise Unsure("--pr needs GITHUB_REPOSITORY and GITHUB_REPOSITORY_OWNER, as the "
                         "`api` job sets them")
        approved, why = approval(args.pr, repo, owner)
    except Unsure as e:
        print(f"FAIL: the API-change gate could not tell: {e}")
        return 2
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if approved:
        report(found, f"PASS: approved -- {why}.", summary)
        return 0
    report(found, f"FAIL: an API change waits for Omar, and {why}.\n"
                  f"He reads it and adds the label {APPROVED_LABEL} -- only {owner} may; "
                  "the author never does --\nthen re-runs this job: a label starts no run "
                  "of its own.", summary)
    print(f"::error title=API change::{APPROVED_LABEL} is missing: this changes a public "
          "header or a key of the .toml, and waits for Omar")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
