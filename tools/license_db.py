#!/usr/bin/env python3
"""The licence guard: every vendored component has a licence we have decided
we can live with, and where we altered one, we have said so.

    python3 tools/license_db.py --check      (what tools/license_check.sh runs)

Three things are compared and every disagreement fails:

  1. THE TREE. thirdparty/ walked to the depths where components live: the
     top-level directories, the single files under thirdparty/rres/external/,
     and everything raylib bundles under thirdparty/raylib/src/external/.
     Those last ~35 are statically linked into every binary and used to be
     invisible to the notice generator, which only ever looked one level down.
  2. THE RECORD. The ```components block in THIRD_PARTY_LICENSES.md: one row
     per component with its licence, which alternative we ELECT when there is
     a choice, whether we modified it, which release families link it, and
     where the licence text is. The block is the single source the shipped
     LICENSES.txt is generated from, so a row that is wrong ships wrong, and a
     component with no row does not ship at all -- which is why (1) and (2)
     have to agree in both directions.
  3. THE TEXT. For every row the licence text is found on disk -- a LICENSE
     file, or a block in the first 200 / last 120 lines of the file, which is
     where a licence always is -- and fingerprinted. The fingerprint has to
     contain every family the row claims, so a row cannot say MIT about a file
     that says GPL.

And then the rules. Copyleft (AGPL, SSPL, GPL, LGPL, MPL, EUPL) fails; LGPL
too, because dynamic linking is not available to us on iOS or in a static musl
build, so LGPL is GPL here. Non-commercial fails. UNKNOWN fails. A component
under a licence with a mark-your-changes clause -- zlib clause 2, Apache 4(b)
-- is either recorded unmodified and pinned by sha256 in FROZEN_VERSIONS.md,
recomputed here, or marked modified WITH a PATCHES.md next to it. MIT and BSD
have no such clause; we record modifications anyway, because a reader cannot
tell which family imposes what and one rule is cheaper than a table.

Standard library only, no network, no compiler; reads at most 320 lines of any
source file, so the whole thing is well under the 200 ms a `just test` gate is
allowed. Not a licence scanner: it does not look for stripped headers inside a
component, and it does not parse SPDX expressions in full. It answers one
question the same way every other check here does, by failing the build.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Where the licence text of a file is: the first 200 lines or the last 120.
HEAD_LINES = 200
TAIL_LINES = 120

LICENCE_FILES = ("LICENSE", "LICENSE.md", "LICENSE.txt", "LICENCE", "LICENCE.md",
                 "COPYING", "UNLICENSE")

# Fingerprints, matched against whitespace-collapsed lower-case text. Every one
# that matches is collected; the ORDER only matters for the two pairs where one
# phrase contains the other (LGPL before GPL, BSD-3 before BSD-2).
FINGERPRINTS = [
    ("AGPL", r"affero general public license"),
    ("SSPL", r"server side public license"),
    ("LGPL", r"lesser general public license|library general public license"),
    ("GPL", r"gnu general public license"),
    ("MPL", r"mozilla public license"),
    ("EUPL", r"european union public licence"),
    ("CC-BY-NC", r"creative commons[^.]{0,80}non-?commercial|attribution-noncommercial"),
    ("CC0", r"creative commons zero|cc0 1\.0|cc0-1\.0|dedicate any and all copyright interest"),
    ("Unlicense", r"free and unencumbered software released into the public domain|www\.unlicense\.org"),
    ("Apache-2.0", r"apache license,? version 2\.0|apache-2\.0"),
    ("zlib", r"altered source versions must be plainly marked"),
    ("BSD-3", r"neither the name"),
    ("BSD-2", r"redistributions in binary form"),
    ("MIT-0", r"mit no attribution|mit-0"),
    ("MIT", r"permission is hereby granted, free of charge|spdx-license-identifier: mit\b"),
    ("WTFPL", r"do what the fuck you want|wtfpl"),
    # ISC and the Kevlin Henney / X11-style notices: "permission to use, copy,
    # modify, and distribute this software ... is hereby granted".
    ("permissive", r"permission to use, copy, modify,? and(?:/or)? distribute this software"),
    ("public-domain", r"public domain"),
]

# What we refuse, and why the message says what it says.
REFUSED = {
    "AGPL": "copyleft", "SSPL": "copyleft", "GPL": "copyleft", "MPL": "copyleft",
    "EUPL": "copyleft",
    "LGPL": "copyleft in practice: it allows dynamic linking, and this project has "
            "none on iOS or in a static musl build, so it binds like the GPL",
    "CC-BY-NC": "non-commercial: it forbids selling the game",
    "UNKNOWN": "no recognisable licence",
}

# Families with a "mark your changes" clause: zlib clause 2, Apache 4(b).
MARK_CHANGES = {"zlib", "Apache-2.0"}

# Families under which a missing notice text is not a problem, because nothing
# is owed: a public-domain dedication needs no notice to travel with the code.
NO_NOTICE_OWED = {"CC0", "Unlicense", "WTFPL", "public-domain", "MIT-0"}

# Release families, as tools/configure.py's TARGETS spells them, and the file
# whose packaging step has to name LICENSES.txt for that family. `desktop` in a
# row's `linked` column means linux + windows + macos + bsd.
FAMILY_FILES = {
    "linux": ".github/workflows/_linux.yml",
    "windows": ".github/workflows/_windows.yml",
    "apple": ".github/workflows/_apple.yml",
    "bsd": ".github/workflows/_bsd.yml",
    "web": ".github/workflows/_web.yml",
    "android": "raymob/app/build.gradle",
    "ios": "tools/configure.py",
}
LINKED_VALUES = {"all", "none", "desktop", "web", "android", "ios"}
MODIFIED_VALUES = {"no", "yes", "subset"}

COLUMNS = ("name", "path", "licences", "elect", "modified", "linked", "evidence")


class Row(dict):
    """One line of the ```components block."""

    @property
    def families(self) -> list[str]:
        return self["licences"].split("|")

    @property
    def elected(self) -> str:
        return self["elect"] if self["elect"] != "-" else self["licences"]

    @property
    def linked(self) -> set[str]:
        return set(self["linked"].split("|"))


# ---------------------------------------------------------------------------
# The record
# ---------------------------------------------------------------------------

def parse_block(text: str) -> list[Row]:
    """The ```components block, as rows. Raises ValueError on a malformed row,
    naming the line, because the block is edited by hand."""
    rows: list[Row] = []
    inside = False
    for number, line in enumerate(text.splitlines(), 1):
        if line.strip() == "```components":
            inside = True
            continue
        if inside and line.strip() == "```":
            inside = False
            continue
        if not inside or not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split()
        if len(fields) != len(COLUMNS):
            raise ValueError(
                f"THIRD_PARTY_LICENSES.md:{number}: a components row has {len(fields)} "
                f"columns, not {len(COLUMNS)} ({' '.join(COLUMNS)})")
        row = Row(zip(COLUMNS, fields))
        if row["elect"] != "-" and row["elect"] not in row.families:
            raise ValueError(
                f"THIRD_PARTY_LICENSES.md:{number}: {row['name']} elects "
                f"{row['elect']!r}, which is not one of its licences {row['licences']!r}")
        if row["elect"] == "-" and len(row.families) != 1:
            raise ValueError(
                f"THIRD_PARTY_LICENSES.md:{number}: {row['name']} offers a choice "
                f"({row['licences']}) and the elect column has to say which one we take")
        if row["modified"] not in MODIFIED_VALUES:
            raise ValueError(
                f"THIRD_PARTY_LICENSES.md:{number}: modified = {row['modified']!r}; "
                f"use one of {', '.join(sorted(MODIFIED_VALUES))}")
        if not row.linked <= LINKED_VALUES:
            raise ValueError(
                f"THIRD_PARTY_LICENSES.md:{number}: linked = {row['linked']!r}; "
                f"use {', '.join(sorted(LINKED_VALUES))}, joined with |")
        rows.append(row)
    if not rows:
        raise ValueError("THIRD_PARTY_LICENSES.md has no ```components block")
    return rows


def load_rows(repo: Path = REPO) -> list[Row]:
    return parse_block((repo / "THIRD_PARTY_LICENSES.md").read_text(encoding="utf-8"))


def frozen_pins(repo: Path = REPO) -> dict[str, str]:
    """The ```versions block of FROZEN_VERSIONS.md, as key -> value."""
    pins: dict[str, str] = {}
    inside = False
    for line in (repo / "thirdparty" / "FROZEN_VERSIONS.md").read_text(encoding="utf-8").splitlines():
        if line.strip() == "```versions":
            inside = True
            continue
        if inside and line.strip() == "```":
            break
        if inside and line.strip() and not line.lstrip().startswith("#"):
            key, _, value = line.partition(" ")
            pins[key.strip()] = value.strip()
    return pins


# ---------------------------------------------------------------------------
# The tree
# ---------------------------------------------------------------------------

def submodule_paths(repo: Path) -> set[str]:
    modules = repo / ".gitmodules"
    if not modules.is_file():
        return set()
    return set(re.findall(r"^\s*path\s*=\s*(\S+)", modules.read_text(encoding="utf-8"), re.M))


def _single_file_components(directory: Path) -> list[Path]:
    """The .c/.h files of a directory grouped by stem: aes.c and aes.h are one
    component, and it is the .c that names it when there is one."""
    by_stem: dict[str, list[Path]] = {}
    for p in sorted(directory.iterdir()):
        if p.is_file() and p.suffix in (".c", ".h", ".cpp", ".hpp"):
            by_stem.setdefault(p.stem, []).append(p)
    out = []
    for stem, files in sorted(by_stem.items()):
        sources = [f for f in files if f.suffix in (".c", ".cpp")]
        out.append(sources[0] if sources else files[0])
    return out


def inventory(root: Path, repo: Path = REPO) -> list[Path]:
    """Every component under `root` (a thirdparty/ directory), as paths
    relative to `repo`. Depth is deliberate: top-level entries, plus the two
    places where one component bundles others of its own."""
    found: list[Path] = []
    if not root.is_dir():
        return found
    for entry in sorted(root.iterdir()):
        if entry.is_file():
            if entry.suffix in (".c", ".h", ".cpp", ".hpp"):
                found.append(entry)
            continue  # FROZEN_VERSIONS.md and friends are not components
        if entry.is_dir():
            found.append(entry)
            external = entry / "external"
            if external.is_dir():  # rres/external/
                found += _single_file_components(external)
            bundled = entry / "src" / "external"
            if bundled.is_dir():  # raylib/src/external/
                for sub in sorted(bundled.iterdir()):
                    if sub.is_dir():
                        found.append(sub)
                found += _single_file_components(bundled)
    return [p.relative_to(repo) for p in found]


# ---------------------------------------------------------------------------
# The text
# ---------------------------------------------------------------------------

def _region(path: Path) -> list[tuple[int, str]]:
    """(line number, line) for the head and tail of a file -- where a licence
    is -- without reading a 4 MB header in full."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if len(lines) <= HEAD_LINES + TAIL_LINES:
        return list(enumerate(lines, 1))
    head = list(enumerate(lines[:HEAD_LINES], 1))
    tail = list(enumerate(lines[-TAIL_LINES:], len(lines) - TAIL_LINES + 1))
    return head + tail


def classify(text: str) -> list[str]:
    """Every family whose fingerprint appears in `text`, most specific first."""
    flat = re.sub(r"\s+", " ", text.lower())
    found: list[str] = []
    for family, pattern in FINGERPRINTS:
        if not re.search(pattern, flat):
            continue
        if family == "GPL" and ("LGPL" in found or "AGPL" in found):
            continue  # the phrase is inside the longer one
        if family == "BSD-2" and "BSD-3" in found:
            continue
        found.append(family)
    return found


def licence_file_for(component: Path, evidence: str) -> Path | None:
    """The LICENSE file a row's evidence points at, or None."""
    if component.is_file() and evidence == "file":
        return component  # a bare notice file, e.g. ios/ANGLE-LICENSE.txt
    base = component if component.is_dir() else component.parent
    if evidence.startswith("file:"):
        named = base / evidence[len("file:"):]
        return named if named.is_file() else None
    for candidate in LICENCE_FILES:
        if (base / candidate).is_file() and component.is_dir():
            return base / candidate
    return None


def _comment_lines(region: list[tuple[int, str]]) -> list[bool]:
    """Which lines of a head/tail region are comment or blank, tracking block
    comments across lines. The region is two contiguous runs (head, tail);
    state restarts where the numbering jumps."""
    flags = []
    in_block = False
    previous = None
    for number, line in region:
        if previous is not None and number != previous + 1:
            in_block = False  # the jump from head to tail
        previous = number
        s = line.strip()
        was_in_block = in_block
        if "/*" in s and "*/" not in s.split("/*", 1)[1]:
            in_block = True
        if "*/" in s:
            in_block = False
        commentish = (was_in_block or in_block or s == "" or s.startswith("//")
                      or s.startswith("/*") or s.startswith("*"))
        flags.append(commentish)
    return flags


def notice_text(component: Path, evidence: str, families: list[str]) -> str | None:
    """The licence text to ship for a component: the LICENSE file, or the
    comment block in the source that carries it. None when there is none."""
    if evidence == "file" or evidence.startswith("file:"):
        found = licence_file_for(component, evidence)
        return found.read_text(encoding="utf-8", errors="replace").rstrip() if found else None
    if evidence != "header":
        return None
    source = component
    if component.is_dir():
        headers = sorted(component.glob("*.h"))
        if not headers:
            return None
        source = headers[0]
    region = _region(source)
    flags = _comment_lines(region)
    patterns = [p for f, p in FINGERPRINTS if f in families]
    hits = []
    for index, (_, line) in enumerate(region):
        flat = re.sub(r"\s+", " ", line.lower())
        if any(re.search(p, flat) for p in patterns):
            hits.append(index)
    if not hits:
        return None

    # Every hit expands to its enclosing comment block -- up and down while the
    # lines are still comment, and no further than a screenful either way --
    # and the LONGEST block wins: a header that says "see the end of the file
    # for the licence" matches at the top with one line, and the licence
    # itself matches at the bottom with thirty.
    def block(hit: int) -> tuple[int, int]:
        lo = hit
        while lo > 0 and hit - lo < 120 and flags[lo - 1] \
                and region[lo - 1][0] == region[lo][0] - 1:
            lo -= 1
        hi = hit
        while hi + 1 < len(region) and hi - hit < 120 and flags[hi + 1] \
                and region[hi + 1][0] == region[hi][0] + 1:
            hi += 1
        return lo, hi

    # Scored by how many of the claimed families the block actually contains,
    # then by length: dr_wav's 120-line documentation header mentions "public
    # domain or MIT-0" once and the real dedication, with both texts, is at
    # the bottom of the file.
    def score(b: tuple[int, int]) -> tuple[int, int]:
        text = "\n".join(line for _, line in region[b[0]:b[1] + 1])
        seen = classify(text)
        return (sum(1 for f in families if f in seen), b[1] - b[0])

    lo, hi = max({block(h) for h in hits}, key=score)
    out = []
    for _, line in region[lo:hi + 1]:
        s = line.strip()
        s = re.sub(r"^(/\*+|\*+/|\*|//+|#)\s?", "", s)
        s = re.sub(r"\*+/$", "", s).rstrip()
        out.append(s)
    while out and not out[0]:
        out.pop(0)
    while out and not out[-1]:
        out.pop()
    return "\n".join(out) if out else None


def single_source(component: Path) -> Path | None:
    """The one source file of a single-header component, or None when the
    component is a file itself (returned as is) or a directory with several."""
    if component.is_file():
        return component
    sources = [p for p in component.iterdir()
               if p.is_file() and p.suffix in (".c", ".h", ".cpp", ".hpp")]
    return sources[0] if len(sources) == 1 else None


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


# ---------------------------------------------------------------------------
# The rules
# ---------------------------------------------------------------------------

def check(rows: list[Row], pins: dict[str, str], repo: Path = REPO,
          root: Path | None = None, families: set[str] | None = None) -> tuple[list[str], list[str]]:
    """Every rule, against a tree. Returns (ok lines, failures). `root` is the
    directory whose entries are the top-level components (thirdparty/, or a
    fixture standing in for it)."""
    root = root or repo / "thirdparty"
    top = root.relative_to(repo)
    families = FAMILY_FILES.keys() if families is None else families
    ok: list[str] = []
    fails: list[str] = []
    submodules = submodule_paths(repo)

    on_disk = inventory(root, repo)
    by_path = {str(p): p for p in on_disk}
    rows_by_path = {r["path"]: r for r in rows}
    names = [r["name"] for r in rows]
    for name in set(names):
        if names.count(name) > 1:
            fails.append(f"{name}: appears {names.count(name)} times in the components block")

    # (1) symmetry: every component on disk has a row, every row is on disk.
    for path in on_disk:
        if str(path) not in rows_by_path:
            fails.append(f"{path}: on disk, but not in the components block of "
                         "THIRD_PARTY_LICENSES.md. Add a row (licence, whether we modified "
                         "it, which families link it), or take the dependency out.")
    for row in rows:
        path = repo / row["path"]
        if row["path"] in by_path:
            continue
        if path.is_file() and row["evidence"] == "file":
            continue  # a bare notice file for something that ships prebuilt (ANGLE)
        fails.append(f"{row['path']}: in the components block, but not on disk. "
                     "Remove the row, or fix the path.")

    # (2)-(5) per row.
    for row in rows:
        path = repo / row["path"]
        label = row["name"]
        elected = row.elected

        if elected in REFUSED:
            fails.append(f"{label}: {elected} -- {REFUSED[elected]}. Take the dependency "
                         "out. If you believe the classification is wrong, fix the row and "
                         "the fingerprint; a licence you had to argue about is a licence "
                         "to walk away from.")
            continue

        is_submodule = row["path"] in submodules
        if is_submodule and path.is_dir() and not any(path.iterdir()):
            ok.append(f"{label:24} submodule not checked out; nothing to read")
            continue

        # The text on disk, and what it says.
        evidence = row["evidence"]
        if evidence.startswith("part-of:"):
            parent = evidence[len("part-of:"):]
            owner = next((r for r in rows if r["name"] == parent), None)
            if owner is None:
                fails.append(f"{label}: evidence names {parent!r}, which is not a row")
            elif owner.elected != elected:
                fails.append(f"{label}: is part of {parent} ({owner.elected}) but claims "
                             f"{elected}")
            else:
                ok.append(f"{label:24} {elected:12} part of {parent}")
        elif evidence.startswith("upstream:"):
            if elected not in NO_NOTICE_OWED:
                fails.append(f"{label}: no licence text on disk, only a URL, and {elected} "
                             "requires the notice to travel with the code. Vendor the "
                             "notice (file:<name>) or take the dependency out.")
            else:
                ok.append(f"{label:24} {elected:12} no notice owed; upstream recorded")
        elif evidence == "file" or evidence.startswith("file:") or evidence == "header":
            if not path.exists():
                fails.append(f"{label}: {row['path']} does not exist")
                continue
            if evidence == "header":
                source = path if path.is_file() else next(iter(sorted(path.glob("*.h"))), None)
                text = "\n".join(l for _, l in _region(source)) if source else ""
            else:
                found = licence_file_for(path, evidence)
                text = found.read_text(encoding="utf-8", errors="replace") if found else ""
            if not text:
                fails.append(f"{label}: no licence text found ({evidence}) at {row['path']}. "
                             "Every component needs one: a LICENSE file, or a block in the "
                             f"first {HEAD_LINES} / last {TAIL_LINES} lines of the source.")
                continue
            seen = classify(text)
            missing = [f for f in row.families if f not in seen]
            if missing:
                fails.append(f"{label}: the row says {row['licences']} but the text at "
                             f"{row['path']} reads as {','.join(seen) or 'UNKNOWN'}; "
                             f"{','.join(missing)} not found in it")
                continue
            refused_seen = [f for f in seen if f in REFUSED and f not in row.families]
            if refused_seen and elected not in NO_NOTICE_OWED and len(seen) == 1:
                fails.append(f"{label}: the text at {row['path']} reads as "
                             f"{','.join(refused_seen)}")
                continue
            ok.append(f"{label:24} {elected:12} {evidence}")
        else:
            fails.append(f"{label}: evidence = {evidence!r}; use file, file:<name>, header, "
                         "part-of:<name> or upstream:<url>")
            continue

        # Modifications. Top level only: what raylib bundles is raylib's, and
        # raylib's PATCHES.md is where "nothing under src/external is touched"
        # is stated.
        top_level = Path(row["path"]).parent == top
        if row["modified"] in ("yes", "subset"):
            if not top_level:
                fails.append(f"{label}: a bundled component cannot be marked modified; "
                             "mark the component that bundles it")
                continue
            patches = path / "PATCHES.md" if path.is_dir() else path.parent / "PATCHES.md"
            note = patches.read_text(encoding="utf-8") if patches.is_file() else ""
            if "MODIFIED" not in note:
                fails.append(f"{label}: marked modified, but {patches.relative_to(repo)} is "
                             "missing or does not say MODIFIED. The zlib licence's clause 2 "
                             "requires altered source to be plainly marked; that file is "
                             "the mark.")
        elif top_level and single_source(path) is not None:
            # An unmodified single-header component is pinned by its sha256, so
            # "unmodified" is a fact the guard recomputes and not a claim.
            key = "sha256_" + re.sub(r"[^a-z0-9]+", "_", label.lower())
            want = pins.get(key)
            have = sha256_of(single_source(path))
            if want is None:
                fails.append(f"{label}: unmodified single-file component with no pin. Add "
                             f"`{key} {have}` to the versions block of FROZEN_VERSIONS.md, "
                             "or mark it modified with a PATCHES.md.")
            elif want != have:
                fails.append(f"{label}: sha256 {have[:16]}... does not match the pin "
                             f"{want[:16]}... in FROZEN_VERSIONS.md. Either the file changed "
                             "-- then mark it modified and write PATCHES.md -- or the pin is "
                             "stale.")
        elif elected in MARK_CHANGES and top_level and is_submodule:
            pass  # pinned by the submodule commit itself

    # (6) packaging: every family that ships names LICENSES.txt.
    for family in sorted(families):
        rel = FAMILY_FILES.get(family)
        if rel is None:
            fails.append(f"family {family!r} has no packaging file in FAMILY_FILES")
            continue
        text = (repo / rel).read_text(encoding="utf-8", errors="replace") if (repo / rel).is_file() else ""
        if "LICENSES.txt" not in text:
            fails.append(f"{rel}: packages the {family} family and never mentions "
                         "LICENSES.txt, so that family ships no third-party notice")
        else:
            ok.append(f"{'packaging':24} {family:12} {rel}")
    return ok, fails


def rows_for(rows: list[Row], family: str) -> list[Row]:
    """The rows that reach a release family's binary. `desktop` covers linux,
    windows, macos and the BSDs."""
    wanted = {"all", family}
    if family in ("linux", "windows", "apple", "bsd", "macos"):
        wanted.add("desktop")
    return [r for r in rows if r.linked & wanted]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="run every rule and report")
    ap.add_argument("--inventory", action="store_true", help="list the components found")
    args = ap.parse_args(argv)
    if args.inventory:
        for p in inventory(REPO / "thirdparty"):
            print(p)
        return 0
    try:
        rows = load_rows()
    except ValueError as e:
        print(f"FAIL: {e}")
        return 1
    ok, fails = check(rows, frozen_pins())
    for line in ok:
        print(f"  ok    {line}")
    for line in fails:
        print(f"  FAIL  {line}")
    if fails:
        print(f"FAIL: {len(fails)} licence problem(s) above; {len(ok)} components ok")
        return 1
    print(f"PASS: {len(ok)} components, every licence known, every alteration marked")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
