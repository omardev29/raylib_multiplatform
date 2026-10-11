#!/usr/bin/env python3
"""What the deep runs found, as one GitHub issue per signature.

.github/workflows/deep.yml fuzzes, runs the properties at random seeds and
mutates every night and every week; a job that fails records what it saw:

    python3 tools/finding.py record fuzz e2e_script build/fuzz/run.log \\
        --repro "bash tools/fuzz.sh replay e2e_script <the crash file>" --out build/findings

and the report job opens the issues:

    python3 tools/finding.py report build/findings

One issue per SIGNATURE, labelled nightly-finding -- what the agents working
on this repository take first. A signature is what makes two failures the
same failure, and nothing that changes between runs of it:

  - a sanitizer report: its kind (heap-buffer-overflow, signed-integer-
    overflow...) and the top three frames that are ours -- function names,
    no addresses, no line numbers, which move with every edit;
  - a fuzz oracle: the promise that did not hold (FUZZ ORACLE FAILED: ...);
  - a property: the test case and the assertion that was falsified;
  - a Python test: its id;
  - a mutation module: the module and whether it fell below its floor.

The same signature found again is a comment on its open issue, never a second
issue. A closed one is not reopened: a new issue says it came back.

Standard library; `report` needs the gh CLI and GH_TOKEN, as the workflow
gives it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

LABEL = "nightly-finding"
KINDS = ("fuzz", "property", "hypothesis", "mutation")

FRAME = re.compile(r"^\s*#\d+\s+0x[0-9a-fA-F]+\s+in\s+(.+?)\s+(?:/|\(|[A-Za-z]:\\)")
# Frames that are the tools', not the code under test.
# Prefixes; main, abort and raise are matched whole -- `main_menu::draw` is ours.
NOT_OURS = ("__asan", "__ubsan", "__sanitizer", "__interceptor", "fuzzer::", "__libc_start",
            "_start", "std::", "__cxx", "LLVMFuzzerTestOneInput", "testing::", "doctest::",
            "rc::", "__GI_")


def without_parameters(name: str) -> str:
    """`ns::f(int, std::vector<int>) const` -> `ns::f`: the last parenthesised
    group at the end and what follows it, and nothing inside the name --
    `(anonymous namespace)::walk` keeps its parentheses."""
    name = re.sub(r"\)\s*(?:const|volatile|&|&&|noexcept|\s)*$", ")", name)
    if not name.endswith(")"):
        return name
    depth = 0
    for i in range(len(name) - 1, -1, -1):
        if name[i] == ")":
            depth += 1
        elif name[i] == "(":
            depth -= 1
            if depth == 0:
                return name[:i].strip()
    return name


def our_frames(log: str, count: int = 3) -> list[str]:
    """The first `count` stack frames of the first report that are not a
    tool's, as bare function names."""
    out = []
    for line in log.splitlines():
        m = FRAME.match(line)
        if not m:
            if out and not line.strip():
                break  # the first report's stack is over
            continue
        name = without_parameters(re.sub(r"\[abi:\w+\]", "", m.group(1)).strip())
        if name.startswith(NOT_OURS) or name in ("main", "abort", "raise"):
            continue
        out.append(name)
        if len(out) == count:
            break
    return out


def basis(kind: str, log: str) -> str:
    """What identifies the failure in `log`, before it is hashed."""
    m = re.search(r"FUZZ ORACLE FAILED: (.+)", log)
    if m:
        return "oracle: " + m.group(1).strip()
    m = re.search(r"(?:ERROR: (?:AddressSanitizer|LeakSanitizer|libFuzzer): ([\w-]+))"
                  r"|(?:SUMMARY: UndefinedBehaviorSanitizer: ([\w-]+))", log)
    if m:
        what = m.group(1) or m.group(2)
        return f"{what} in {' < '.join(our_frames(log)) or 'no frame of ours'}"
    m = re.search(r"runtime error: (.+?)(?: in |$)", log, re.M)
    if m:
        return "runtime error in " + (" < ".join(our_frames(log)) or m.group(1)[:80])
    if kind == "property":
        # doctest prints one block per failing test case, between lines of
        # `=`: the case and what failed in it are read from the same block --
        # the first case of the log with the second case's RC_ASSERT is a
        # failure that never happened.
        for block in re.split(r"^={20,}$", log, flags=re.M):
            case = re.search(r"^TEST CASE:\s+(.+)$", block, re.M)
            if not case:
                continue
            falsified = re.search(r"^(RC_ASSERT.*|RC_.*\(.*\))$", block, re.M)
            check = re.search(r": ERROR: (\w+\(.*\)) is NOT correct!", block)
            what = falsified or check
            return "property: " + case.group(1).strip() + (
                " / " + what.group(1).strip()[:120] if what else "")
    if kind == "hypothesis":
        tests = sorted(set(re.findall(r"^(?:FAIL|ERROR): (\S+ \(\S+\))", log, re.M)))
        if tests:
            return "hypothesis: " + ", ".join(tests)
    if kind == "mutation":
        verdicts = sorted(set(re.findall(r"FAIL\s+(\S+)\s.*?-- (below its floor|the floor is)",
                                         log)))
        if verdicts:
            return "mutation: " + ", ".join(f"{m} {v}" for m, v in verdicts)
    fails = [line.strip() for line in log.splitlines() if line.strip().startswith("FAIL")]
    return fails[0][:160] if fails else "it failed and said nothing a signature can be made of"


def signature(kind: str, target: str, log: str) -> tuple[str, str]:
    what = basis(kind, log)
    digest = hashlib.sha256(f"{kind}|{target}|{what}".encode()).hexdigest()[:12]
    return digest, what


def excerpt(log: str, lines: int = 80) -> str:
    """The report itself, not the libFuzzer progress lines around it."""
    keep = [line for line in log.splitlines()
            if not re.match(r"^#\d+\s+(NEW|REDUCE|pulse|INITED)", line)]
    for i, line in enumerate(keep):
        if re.search(r"ERROR:|runtime error|FUZZ ORACLE|Falsifiable|^FAIL|Traceback", line):
            return "\n".join(keep[max(0, i - 5):i + lines])
    return "\n".join(keep[-lines:])


def record(kind: str, target: str, log_text: str, repro: str, run_url: str) -> dict:
    sig, what = signature(kind, target, log_text)
    title = f"{LABEL}: {kind} {target}: {what[:90]} [{sig}]"
    body = "\n".join([
        f"The deep run found this in **{kind} `{target}`**: {what}",
        "",
        f"Signature `{sig}` -- the same failure found again is a comment here, not a "
        "new issue.",
        "",
        "Reproduce it, in the pinned build image (`tools/dev_shell.sh`):",
        "",
        "```",
        repro,
        "```",
        "",
        f"The run, with its artifacts (the input, the log): {run_url}",
        "",
        "The fix lands with the input in `tests/fuzz/regressions/<target>/`, or the "
        "case as a named doctest/unittest test, seen red first (CLAUDE.md).",
        "",
        "<details><summary>What it said</summary>",
        "",
        "```",
        excerpt(log_text),
        "```",
        "",
        "</details>",
    ])
    return {"signature": sig, "kind": kind, "target": target, "title": title, "body": body}


def gh(*argv: str, stdin: str | None = None) -> str:
    got = subprocess.run(["gh", *argv], input=stdin, capture_output=True, text=True)
    if got.returncode != 0:
        raise SystemExit(f"FAIL: gh {' '.join(argv[:3])}: {got.stderr.strip()}")
    return got.stdout


def report(folder: Path) -> int:
    findings = [json.loads(p.read_text()) for p in sorted(folder.glob("*.json"))]
    if not findings:
        print("nothing found")
        return 0
    gh("label", "create", LABEL, "--color", "B60205", "--force",
       "--description", "Found by the deep runs: take these first")
    for f in findings:
        listed = json.loads(gh("issue", "list", "--label", LABEL, "--state", "open",
                               "--search", f"{f['signature']} in:title", "--json",
                               "number,title", "--limit", "20"))
        mine = [i for i in listed if f"[{f['signature']}]" in i["title"]]
        if mine:
            gh("issue", "comment", str(mine[0]["number"]), "--body-file", "-",
               stdin=f"Found again: {os.environ.get('RUN_URL', 'this run')}")
            print(f"  again  #{mine[0]['number']} {f['title']}")
        else:
            url = gh("issue", "create", "--title", f["title"], "--label", LABEL,
                     "--body-file", "-", stdin=f["body"]).strip()
            print(f"  new    {url} {f['title']}")
    # The jobs that found them are red already; this one did its job.
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    rec = sub.add_parser("record", help="write what a failed job saw, as a finding")
    rec.add_argument("kind", choices=KINDS)
    rec.add_argument("target")
    rec.add_argument("log", type=Path)
    rec.add_argument("--repro", required=True, help="the one command that plays it again")
    rec.add_argument("--out", type=Path, default=Path("build/findings"))
    rep = sub.add_parser("report", help="one issue per signature, from the findings")
    rep.add_argument("folder", type=Path)
    args = parser.parse_args(argv)
    if args.command == "record":
        text = args.log.read_text(errors="replace") if args.log.is_file() else ""
        finding = record(args.kind, args.target, text, args.repro,
                         os.environ.get("RUN_URL", "(no run URL)"))
        args.out.mkdir(parents=True, exist_ok=True)
        path = args.out / f"{args.kind}-{args.target}.json"
        path.write_text(json.dumps(finding, indent=1) + "\n")
        print(f"recorded {path}: {finding['title']}")
        return 0
    return report(args.folder)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
