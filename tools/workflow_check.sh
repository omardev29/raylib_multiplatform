#!/usr/bin/env bash
# Things about the workflows that only GitHub finds out, and finds out late.
#
# actionlint reads one file at a time. It cannot see that a reusable workflow
# asks for a permission its caller does not grant, and GitHub answers that with
# a startup_failure: the run appears with ZERO jobs, no logs, and the message
# "Error calling workflow". Nothing local reproduces it and the matrix is gone.
#
# It cost exactly one round: _itch.yml moved into the build image for butler,
# which needs `packages: read`, and the `itch` job in ci.yml did not grant it.
#
# Usage: tools/workflow_check.sh [root]   (from the repo root)
#
# `root` is a directory holding a .github/workflows/ to check instead of this
# one. It exists so tests/configure_test.py can point this at a fixture with a
# caller that forgets `secrets: inherit` and WATCH IT GO RED -- a checker
# nobody has seen fail is a hope with a name, and this one had a branch that
# could not fail at all (see the PyYAML note below).

set -uo pipefail
cd "${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

python3 - <<'PY'
import os, pathlib, re, sys

# A gate that cannot fail is not a gate. This check ran green in CI for months
# having done NOTHING: the build image had no PyYAML, so it took the skip
# branch below and printed a line that reads like a pass. Inside the image --
# which is exactly where it matters, because the failure it catches is a
# startup_failure with zero jobs and no logs -- a missing PyYAML is now the
# error, not an excuse. The image ships python3-yaml.
#
# On a laptop without it, it stays a skip: this has to be installable-free for
# somebody who has just cloned the repository.
IN_IMAGE = pathlib.Path("/etc/raylib-build-image.json").is_file()

try:
    import yaml
except ImportError:
    if IN_IMAGE:
        print("FALLA: PyYAML is missing INSIDE the build image, so this check cannot run.")
        print("       It is the only thing that catches a reusable workflow asking for a")
        print("       permission its caller does not grant -- which GitHub answers with a")
        print("       run that has zero jobs and no logs. Add python3-yaml to the image.")
        sys.exit(1)
    print("  skip  PyYAML not installed (install it, or run this inside the build image)")
    sys.exit(0)

# contents and metadata are granted by default, so a caller that says nothing
# still has them. Everything else has to be asked for explicitly, and that is
# the whole failure mode: the callee asks, the caller never granted, GitHub
# refuses to start the run.
DEFAULTED = {"contents", "metadata"}

fails = 0
checked = 0
for caller_path in sorted(pathlib.Path(".github/workflows").glob("*.yml")):
    caller = yaml.safe_load(caller_path.read_text(encoding="utf-8")) or {}
    for job_name, job in (caller.get("jobs") or {}).items():
        uses = job.get("uses")
        if not uses or not uses.startswith("./"):
            continue
        callee_path = pathlib.Path(uses[2:])
        if not callee_path.is_file():
            print(f"  FAIL  {caller_path.name}: {job_name} calls {uses}, which does not exist")
            fails += 1
            continue
        callee = yaml.safe_load(callee_path.read_text(encoding="utf-8")) or {}

        wanted = set((callee.get("permissions") or {}).keys())
        for sub in (callee.get("jobs") or {}).values():
            wanted |= set((sub.get("permissions") or {}).keys())
        granted = set((job.get("permissions") or {}).keys())
        missing = (wanted - granted) - DEFAULTED
        checked += 1
        if missing:
            print(f"  FAIL  {caller_path.name}: job '{job_name}' calls {callee_path.name}, "
                  f"which asks for {sorted(missing)} and is not granted it")
            print(f"        add to that job:  permissions:\n"
                  f"                            "
                  + "\n                            ".join(f"{k}: read" for k in sorted(granted | missing)))
            fails += 1

        # And the inputs, which actionlint does check but only sometimes.
        declared = set(((callee.get("on") or callee.get(True) or {})
                        .get("workflow_call", {}).get("inputs") or {}))
        required = {k for k, v in (((callee.get("on") or callee.get(True) or {})
                    .get("workflow_call", {}).get("inputs") or {}).items())
                    if v and v.get("required")}
        passed = set((job.get("with") or {}).keys())
        unknown = passed - declared
        absent = required - passed
        if unknown:
            print(f"  FAIL  {caller_path.name}: job '{job_name}' passes {sorted(unknown)} "
                  f"to {callee_path.name}, which does not declare it")
            fails += 1
        if absent:
            print(f"  FAIL  {caller_path.name}: job '{job_name}' does not pass "
                  f"{sorted(absent)}, which {callee_path.name} requires")
            fails += 1

        # And the secrets, which is failure mode #2 in ci.yml's own header:
        # "Called workflows get no secrets unless you say `secrets: inherit`.
        # Miss it and the signing / butler / GCP steps all quietly take their
        # 'not configured, skipping' branch and the pipeline looks green."
        # Nothing checked it. A silent skip is the worst outcome available --
        # it is not even a red run to go and read.
        callee_text = callee_path.read_text(encoding="utf-8")
        used = sorted({m for m in re.findall(r"secrets\.([A-Za-z_][A-Za-z0-9_]*)", callee_text)
                       if m != "GITHUB_TOKEN"})  # always present, never inherited
        declared_secrets = set(((callee.get("on") or callee.get(True) or {})
                                .get("workflow_call", {}).get("secrets") or {}))
        needed = sorted(set(used) | declared_secrets)
        if needed:
            given = job.get("secrets")
            if given == "inherit":
                pass
            elif isinstance(given, dict):
                still = [s for s in needed if s not in given]
                if still:
                    print(f"  FAIL  {caller_path.name}: job '{job_name}' passes an explicit "
                          f"secrets map to {callee_path.name}, which also reads {still}")
                    fails += 1
            else:
                print(f"  FAIL  {caller_path.name}: job '{job_name}' calls {callee_path.name}, "
                      f"which reads {needed}, without `secrets: inherit`")
                print("        Those steps will silently take their "
                      "'not configured, skipping' branch and the run will be green.")
                fails += 1

if fails:
    print()
    print(f"FALLA: {fails} problem(s). These are startup failures: the run appears with")
    print("       zero jobs and no logs, so there is nothing to read afterwards.")
    sys.exit(1)
print(f"  ok    {checked} reusable-workflow call(s): permissions, inputs and secrets agree")
PY
