#!/usr/bin/env python3
"""Say what is newer than our pins. Advisory only — it never fails a build.

Separate from the canary's pass/fail on purpose. The canary answers "does the
floating world still build?", which is a yes/no. This answers "what exists?",
which is a decision for a human: a new NDK is information, not an instruction.

Standard library only, and every lookup is wrapped — an upstream being down
must not turn a weekly report into a red job.
"""

from __future__ import annotations

import json
import re
import sys
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TIMEOUT = 20


def frozen() -> dict[str, str]:
    md = REPO / "thirdparty" / "FROZEN_VERSIONS.md"
    pins, inside = {}, False
    for line in md.read_text(encoding="utf-8").splitlines():
        if line.strip() == "```versions":
            inside = True
            continue
        if inside and line.strip() == "```":
            break
        if inside and line.strip() and not line.strip().startswith("#"):
            parts = line.split(None, 1)
            if len(parts) == 2:
                pins[parts[0]] = parts[1].strip()
    return pins


def fetch(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "raylib-multiplatform-canary"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return r.read().decode("utf-8", errors="replace")


def latest_github_release(repo: str) -> str:
    return json.loads(fetch(f"https://api.github.com/repos/{repo}/releases/latest"))["tag_name"]


def latest_maven(group_path: str, artifact: str) -> str:
    xml = fetch(f"https://dl.google.com/dl/android/maven2/{group_path}/{artifact}/maven-metadata.xml")
    # Stable only: Google publishes alphas into the same channel, and <release>
    # cheerfully points at one.
    stable = [v for v in re.findall(r"<version>([^<]+)</version>", xml)
              if re.fullmatch(r"\d+\.\d+\.\d+", v)]
    return max(stable, key=lambda v: [int(x) for x in v.split(".")]) if stable else "?"


def latest_android_sdk(prefix: str) -> str:
    xml = fetch("https://dl.google.com/android/repository/repository2-3.xml")
    found = sorted(set(re.findall(rf"{prefix};([0-9][0-9.]*)", xml)),
                   key=lambda v: [int(x) for x in v.split(".")])
    # NDK marks pre-releases with a .0. patch line; .1/.2 are the stable ones.
    if prefix == "ndk":
        found = [v for v in found if not re.match(r"^\d+\.0\.", v)]
    return found[-1] if found else "?"


def latest_gradle() -> str:
    return json.loads(fetch("https://services.gradle.org/versions/current"))["version"]


CHECKS: list[tuple[str, str, callable]] = [
    ("android_ndk",   "Android NDK",   lambda: latest_android_sdk("ndk")),
    ("agp",           "AGP",           lambda: latest_maven("com/android/tools/build", "gradle")),
    ("gradle",        "Gradle",        latest_gradle),
    ("mesa",          "mesa-dist-win", lambda: latest_github_release("pal1000/mesa-dist-win")),
    ("ninja_mac",     "Ninja",         lambda: latest_github_release("ninja-build/ninja").lstrip("v")),
    ("xcodegen",      "XcodeGen",      lambda: latest_github_release("yonaskolb/XcodeGen")),
]


def main() -> int:
    pins = frozen()
    rows = []
    for key, label, fn in CHECKS:
        try:
            newest = fn()
        except Exception as exc:                      # noqa: BLE001 - advisory only
            newest = f"(lookup failed: {type(exc).__name__})"
        cur = pins.get(key, "—")
        moved = newest not in ("?", cur) and not newest.startswith("(")
        rows.append((label, key, cur, newest, moved))

    print("## Upstream versions")
    print()
    print("Advisory only. A newer version is information, not an instruction — bumping one is a "
          "deliberate act, and `tools/versions_check.sh` will hold you to updating every place "
          "it appears.")
    print()
    print("| what | pin | ours | newest upstream |")
    print("|---|---|---|---|")
    for label, key, cur, newest, moved in rows:
        mark = " ⬆️" if moved else ""
        print(f"| {label} | `{key}` | `{cur}` | `{newest}`{mark} |")
    print()
    behind = [r[0] for r in rows if r[4]]
    print(f"**{len(behind)} behind upstream**" + (f": {', '.join(behind)}" if behind else "."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
