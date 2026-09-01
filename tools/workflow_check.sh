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
# Usage: tools/workflow_check.sh          (from the repo root)

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - <<'PY'
import pathlib, sys

try:
    import yaml
except ImportError:
    print("  skip  PyYAML not installed")
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

if fails:
    print()
    print(f"FALLA: {fails} problem(s). These are startup failures: the run appears with")
    print("       zero jobs and no logs, so there is nothing to read afterwards.")
    sys.exit(1)
print(f"  ok    {checked} reusable-workflow call(s), permissions and inputs agree")
PY
