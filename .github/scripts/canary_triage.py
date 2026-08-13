#!/usr/bin/env python3
"""Turn a failed canary run into something a person or an agent can act on.

The canary's job is not to go red. Anything can go red. Its job is to say WHAT
MOVED — which is why the report leads with the frozen-versus-observed delta and
only then shows the error. A log slice with no version context tells you a build
broke; the delta tells you Xcode went to 27 and took the xcframework script with
it, which is the difference between a ticket and a fix.

Reads the run's own jobs through the API rather than downloading the run log
archive, because that archive is refused while the run is still going
(`gh run view --log` says "run is still in progress") whereas the per-job
endpoint serves a completed job's log immediately. This job runs while its
siblings are finishing, so the per-job endpoint is the only one that works.

Writes .github/canary-report.json and a Markdown summary.

Env:
    GITHUB_TOKEN         needs `actions: read`
    GITHUB_REPOSITORY    owner/repo
    GITHUB_RUN_ID        the canary run to triage
    GITHUB_SERVER_URL    optional, defaults to github.com
"""

from __future__ import annotations

import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
LEDGER = REPO_ROOT / ".github" / "known-breakage.md"
REPORT = REPO_ROOT / ".github" / "canary-report.json"

API = "https://api.github.com"
TOKEN = os.environ.get("GITHUB_TOKEN", "")
REPO = os.environ.get("GITHUB_REPOSITORY", "")
RUN_ID = os.environ.get("GITHUB_RUN_ID", "")
SERVER = os.environ.get("GITHUB_SERVER_URL", "https://github.com")

# A job name looks like "apple / ios" — the part before the slash is the
# reusable workflow's job in ci.yml, which is exactly our family name.
FAMILY_RUNNER = {
    "apple": "macos-26",       # macOS runners have no Docker: must build natively
    "linux": "ubuntu-24.04",
    "web": "ubuntu-24.04",
    "android": "ubuntu-24.04",
    "windows": "windows-2025",
    "bsd": "ubuntu-24.04",     # semi-blind: the agent proposes, the PR validates
}

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
TIMESTAMP = re.compile(r"^\d{4}-\d{2}-\d{2}T[\d:.]+Z\s?")
MAX_LOG_CHARS = 40_000        # a 6 MB Gradle log is read by nobody, human or model


def api(path: str) -> dict | list:
    req = urllib.request.Request(
        f"{API}{path}",
        headers={"Authorization": f"Bearer {TOKEN}",
                 "Accept": "application/vnd.github+json",
                 "User-Agent": "raylib-multiplatform-canary"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    """Stop at the 302 so the redirect can be followed by hand."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # noqa: D102
        raise _Redirect(newurl)


class _Redirect(Exception):
    def __init__(self, url: str):
        super().__init__(url)
        self.url = url


def job_log(job_id: int) -> str:
    """A completed job's log, even while the run is still in progress.

    The redirect has to be followed manually. The API answers 302 to a storage
    URL that authenticates through its own signed query string, and urllib
    helpfully re-sends our `Authorization: Bearer` header to it — which the
    storage backend rejects outright with 401. curl -L drops credentials on a
    cross-host redirect; urllib does not, so we do it ourselves.
    """
    opener = urllib.request.build_opener(_NoRedirect)
    req = urllib.request.Request(
        f"{API}/repos/{REPO}/actions/jobs/{job_id}/logs",
        headers={"Authorization": f"Bearer {TOKEN}",
                 "User-Agent": "raylib-multiplatform-canary"})
    try:
        try:
            with opener.open(req, timeout=120) as r:
                raw = r.read().decode("utf-8", errors="replace")
        except _Redirect as redir:
            # Deliberately no Authorization header here.
            plain = urllib.request.Request(
                redir.url, headers={"User-Agent": "raylib-multiplatform-canary"})
            with urllib.request.urlopen(plain, timeout=120) as r:
                raw = r.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as e:
        return f"<could not fetch log: HTTP {e.code}>"
    except urllib.error.URLError as e:
        return f"<could not fetch log: {e.reason}>"
    return "\n".join(TIMESTAMP.sub("", ANSI.sub("", ln)).rstrip()
                     for ln in raw.splitlines())


def all_jobs() -> list[dict]:
    out, page = [], 1
    while True:   # 14+ legs, and matrix expansion can pass 100
        batch = api(f"/repos/{REPO}/actions/runs/{RUN_ID}/jobs"
                    f"?filter=latest&per_page=100&page={page}")["jobs"]
        out += batch
        if len(batch) < 100:
            return out
        page += 1


def frozen_pins() -> dict[str, str]:
    md = REPO_ROOT / "thirdparty" / "FROZEN_VERSIONS.md"
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


def observed(log: str) -> dict[str, str]:
    """What the floating build actually resolved to.

    Every family echoes `CANARY_OBSERVED key=value` lines. Deliberately a
    contract rather than log-scraping: a regex over `xcodebuild -version` output
    would work today and quietly stop the day Apple reformats it.
    """
    # Anchored to the start of a line on purpose. Actions echoes the script
    # source into the log before running it, so an unanchored match happily
    # picks up `echo "CANARY_OBSERVED xcode=$(xcodebuild ...)"` and reports the
    # version as `$(xcodebuild`. Only the real output starts the line.
    found = dict(re.findall(r"^CANARY_OBSERVED\s+([a-z_]+)=(\S+)\s*$", log, re.M))
    # The container jobs print /etc/raylib-build-image.json, which is a superset.
    m = re.search(r'"android_ndk":\s*"([^"]+)"', log)
    if m:
        found.setdefault("android_ndk", m.group(1))
    return found


def failing_step(job: dict) -> dict | None:
    for st in job.get("steps") or []:
        if st.get("conclusion") in ("failure", "cancelled"):
            return st
    return None


def error_slice(log: str) -> list[str]:
    """The lines worth reading.

    Not a structural parse of the log: `##[group]` framing has changed before
    and will again. Anchor on `##[error]` and keep enough context to see the
    command that produced it, then fall back to the tail.
    """
    lines = log.splitlines()
    idx = [i for i, ln in enumerate(lines) if "##[error]" in ln]
    if idx:
        start = max(0, idx[0] - 60)
        end = min(len(lines), idx[-1] + 10)
        chunk = lines[start:end]
    else:
        chunk = lines[-120:]
    out, total = [], 0
    for ln in chunk:
        if not ln.strip():
            continue
        total += len(ln)
        if total > MAX_LOG_CHARS:
            out.append("... truncated ...")
            break
        out.append(ln)
    return out


def slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:60] or "unknown"


def known_keys() -> set[str]:
    if not LEDGER.exists():
        return set()
    return set(re.findall(r"^-\s+`([^`]+)`", LEDGER.read_text(encoding="utf-8"), re.M))


def _dedupe(rows: list[dict]) -> list[dict]:
    seen, out = set(), []
    for r in rows:
        if r["key"] not in seen:
            seen.add(r["key"])
            out.append(r)
    return out


def main() -> int:
    if not (TOKEN and REPO and RUN_ID):
        print("canary_triage: GITHUB_TOKEN / GITHUB_REPOSITORY / GITHUB_RUN_ID required",
              file=sys.stderr)
        return 2

    pins = frozen_pins()
    jobs = all_jobs()
    broken = [j for j in jobs
              if j.get("conclusion") in ("failure", "cancelled")
              and not j["name"].startswith("triage")]

    families: list[dict] = []
    seen_families: set[str] = set()
    for job in broken:
        family = job["name"].split("/")[0].strip()
        log = job_log(job["id"])
        step = failing_step(job)
        obs = observed(log)

        # A job that dies early — a bad Xcode pin fails at "Select Xcode",
        # before anything gets a chance to echo what it resolved to — would
        # otherwise report no delta at all, which is the one thing the report
        # exists to show. So fall back to what the canary ASKED for, marked as
        # requested rather than observed.
        requested = json.loads(os.environ.get("CANARY_FLOATING", "{}"))
        for k, v in requested.items():
            obs.setdefault(k, v)

        delta = [{"key": k, "frozen": pins[k], "observed": v}
                 for k, v in sorted(obs.items())
                 if k in pins and pins[k] != v]

        # The key is what deduplicates a recurring breakage across weeks. It has
        # to name the cause, not the symptom, or every rerun looks new.
        cause = delta[0] if delta else None
        key = "/".join([
            family,
            f"{cause['key']}-{cause['observed']}" if cause else "no-version-delta",
            slug(step["name"]) if step else slug(job["conclusion"]),
        ])

        entry = {
            "family": family,
            "runner": FAMILY_RUNNER.get(family, "ubuntu-24.04"),
            "job": job["name"],
            "job_url": job.get("html_url"),
            "conclusion": job["conclusion"],
            "step": step["name"] if step else None,
            "version_delta": delta,
            "observed": obs,
            "error_lines": error_slice(log),
            "key": key,
            "known": key in known_keys(),
        }
        families.append(entry)
        seen_families.add(family)

    report = {
        "run_id": RUN_ID,
        "run_url": f"{SERVER}/{REPO}/actions/runs/{RUN_ID}",
        "repository": REPO,
        "failed": bool(families),
        "families": families,
        # Consumed as a matrix by autofix.yml: one agent per broken family, on
        # that family's own runner. Deduplicated by key — `apple / macos` and
        # `apple / ios` failing on the same Xcode bump is ONE problem, and
        # dispatching two agents at it means two PRs racing to make the same
        # change.
        "fix_matrix": _dedupe([{"family": f["family"], "runner": f["runner"], "key": f["key"]}
                               for f in families if not f["known"]]),
    }
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    REPORT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    # Human-readable summary, delta first — that is the finding.
    out = [f"## Canary: {len(families)} broken famil"
           f"{'y' if len(families) == 1 else 'ies'}", ""]
    for f in families:
        out.append(f"### `{f['family']}` — {f['job']}")
        out.append("")
        if f["version_delta"]:
            out.append("| pin | frozen | canary observed |")
            out.append("|---|---|---|")
            for d in f["version_delta"]:
                out.append(f"| `{d['key']}` | `{d['frozen']}` | **`{d['observed']}`** |")
        else:
            out.append("_No version delta: the floating build failed on something other than a "
                       "version bump. Suspect the rolling package mirrors (BSD) or a flake._")
        out.append("")
        out.append(f"Failed at **{f['step'] or f['conclusion']}** · key `{f['key']}`"
                   + ("  ·  **already in known-breakage.md**" if f["known"] else ""))
        out.append("")
        out.append("```")
        out.extend(f["error_lines"][-60:])
        out.append("```")
        out.append("")
    summary = "\n".join(out)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as fh:
            fh.write(summary + "\n")
    print(summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
