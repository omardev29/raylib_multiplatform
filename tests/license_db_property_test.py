"""tools/license_db.py held to its promises for ANY record and ANY tree.

THIRD_PARTY_LICENSES.md is edited by hand, and thirdparty/ changes under it,
so the guard has to answer whatever it is given with what its rules say: a
row it cannot read is a ValueError naming the line (main() prints it as a
FAIL), and every other disagreement is a failure line -- never a traceback
(R7 in the phase 16 design).

  parse_block()   raises only ValueError, and reads back a block written from
                  rows exactly
  classify()      blind to case and to how the text is broken into lines;
                  adding text never loses a family it found (but the two a
                  longer phrase replaces: GPL by LGPL/AGPL, BSD-2 by BSD-3)
  pin_of()        blind to CRLF, and moved by any byte or any file name
  check()         total over generated trees and records, and each rule
                  fails exactly the rows it is about -- the packaging one too
  notice_text()   the licence block of a file, its comment marks gone and
                  nothing but it; a LICENSE file as it is
  the readers     _region, _comment_lines, frozen_pins, submodule_paths,
                  licence_file_for, pinned_sources, inventory, rows_for and
                  main()'s report, each against what it promises

Every property was seen red: the commit that added this file says how.
"""

from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hypothesis_profiles  # noqa: E402,F401  (first: the profile, or the skip)
from hypothesis import example, given, strategies as st  # noqa: E402

REPO = hypothesis_profiles.REPO


def load(name: str):
    """tools/<name>.py by its path, as `tools.<name>`: the name mutmut gives it."""
    spec = importlib.util.spec_from_file_location(f"tools.{name}", REPO / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


ldb = load("license_db")

FAMILIES = [family for family, _ in ldb.FINGERPRINTS]
# A sentence that carries each family's fingerprint, from the licences themselves.
PHRASES = {
    "AGPL": "GNU Affero General Public License", "SSPL": "Server Side Public License",
    "LGPL": "GNU Lesser General Public License", "GPL": "GNU General Public License",
    "MPL": "Mozilla Public License", "EUPL": "European Union Public Licence",
    "CC-BY-NC": "Attribution-NonCommercial", "CC0": "CC0 1.0 Universal",
    "Unlicense": "This is free and unencumbered software released into the public domain",
    "Apache-2.0": "Apache License, Version 2.0",
    "zlib": "Altered source versions must be plainly marked as such",
    "BSD-3": "Neither the name of the copyright holder",
    "BSD-2": "Redistributions in binary form must reproduce the above copyright notice",
    "MIT-0": "MIT No Attribution",
    "MIT": "Permission is hereby granted, free of charge, to any person",
    "WTFPL": "DO WHAT THE FUCK YOU WANT TO PUBLIC LICENSE",
    "permissive": "Permission to use, copy, modify, and distribute this software",
    "public-domain": "dedicated to the public domain",
}

TOKEN = st.text(alphabet=st.characters(categories=["L", "N", "P", "S"], exclude_characters="|"),
                min_size=1, max_size=8)
# A row's first word: one that starts with `#` makes the line a comment.
NAME = TOKEN.filter(lambda s: not s.startswith("#"))
WORD = st.one_of(st.sampled_from(sorted(PHRASES.values()) + ["license", "licence", "GNU",
                                                             "public", "the", "\n", "  "]),
                 st.text(max_size=12))
LICENCE_TEXT = st.lists(WORD, max_size=12).map(" ".join)


def row_strategy(names, paths):
    licences = st.lists(st.sampled_from(FAMILIES + ["UNKNOWN"]), min_size=1, max_size=2,
                        unique=True)
    return st.builds(
        lambda name, path, lic, elect, modified, linked, evidence: ldb.Row(
            name=name, path=path, licences="|".join(lic),
            elect=elect(lic), modified=modified,
            linked="|".join(linked), evidence=evidence),
        names, paths, licences,
        st.sampled_from([lambda lic: "-", lambda lic: lic[0], lambda lic: lic[-1]]),
        st.sampled_from(sorted(ldb.MODIFIED_VALUES)),
        st.lists(st.sampled_from(sorted(ldb.LINKED_VALUES)), min_size=1, max_size=2, unique=True),
        st.sampled_from(["file", "file:LICENSE", "file:NOPE", "header", "part-of:dep",
                         "part-of:nobody", "upstream:https://example.org", "weird"]))


def block_of(rows) -> str:
    lines = ["# THIRD_PARTY_LICENSES.md", "", "```components",
             "# " + " ".join(ldb.COLUMNS)]
    lines += [" ".join(row[c] for c in ldb.COLUMNS) for row in rows]
    return "\n".join(lines + ["```", ""])


@st.composite
def near_rows(draw) -> str:
    """A row with one column that is not what the block allows."""
    row = draw(row_strategy(NAME, TOKEN))
    column = draw(st.sampled_from(["elect", "modified", "linked", "licences"]))
    row[column] = draw(st.one_of(TOKEN, st.sampled_from(["-", "maybe", "all|web|x", "MIT|zlib"])))
    return " ".join(row[c] for c in ldb.COLUMNS)


BLOCK_LINE = st.one_of(
    st.sampled_from(["```components", "```", "# a comment", "", "   ", "```versions"]),
    st.lists(TOKEN, min_size=1, max_size=9).map(" ".join),
    row_strategy(TOKEN, TOKEN).map(lambda row: " ".join(row[c] for c in ldb.COLUMNS)),
    near_rows(), st.text(max_size=30))
ROW_LINE = row_strategy(NAME, TOKEN).filter(
    lambda row: row["elect"] != "-" or len(row.families) == 1).map(
    lambda row: " ".join(row[c] for c in ldb.COLUMNS))
# Most of them inside the fence, where a row is read; some anywhere; and rows
# that are all fine but one, whose one wrong column is the only thing to see.
BLOCK = st.one_of(st.lists(BLOCK_LINE, max_size=10).map(
                      lambda lines: "\n".join(["```components", *lines, "```"])),
                  st.lists(BLOCK_LINE, max_size=12).map("\n".join),
                  st.tuples(st.lists(ROW_LINE, max_size=3), near_rows(),
                            st.lists(ROW_LINE, max_size=3)).map(
                      lambda parts: "\n".join(["```components", *parts[0], parts[1], *parts[2],
                                               "```"])))


class ParseBlockTest(unittest.TestCase):

    @given(BLOCK)
    def test_any_text_is_rows_or_a_value_error(self, text):
        try:
            rows = ldb.parse_block(text)
        except ValueError as refused:
            self.assertRegex(str(refused), r"THIRD_PARTY_LICENSES\.md")
            return
        self.assertTrue(rows)
        for row in rows:
            self.assertEqual(list(row), list(ldb.COLUMNS))
            self.assertTrue(row["elect"] == "-" and len(row.families) == 1
                            or row["elect"] in row.families, row)
            self.assertIn(row["modified"], ldb.MODIFIED_VALUES)
            self.assertLessEqual(row.linked, ldb.LINKED_VALUES)

    @given(st.lists(row_strategy(NAME, TOKEN), min_size=1, max_size=6))
    def test_a_written_block_reads_back(self, rows):
        rows = [row for row in rows if row["elect"] != "-" or len(row.families) == 1]
        if not rows:
            return
        self.assertEqual(ldb.parse_block(block_of(rows)), rows)


class ClassifyTest(unittest.TestCase):

    @given(LICENCE_TEXT)
    def test_families_are_known_once_each(self, text):
        found = ldb.classify(text)
        self.assertEqual(len(found), len(set(found)))
        self.assertLessEqual(set(found), set(FAMILIES))
        self.assertFalse("GPL" in found and ({"LGPL", "AGPL"} & set(found)))
        self.assertFalse("BSD-2" in found and "BSD-3" in found)
        self.assertEqual(found, [f for f in FAMILIES if f in found], "most specific first")

    @given(LICENCE_TEXT, st.sampled_from([" \n", "\n\n", "\t", "   \r\n  "]))
    def test_case_and_line_breaks_change_nothing(self, text, gap):
        flipped = "".join(c.swapcase() if c.isascii() else c for c in text)
        self.assertEqual(ldb.classify(flipped), ldb.classify(text))
        self.assertEqual(ldb.classify(text.replace(" ", gap)), ldb.classify(text))

    @given(LICENCE_TEXT, LICENCE_TEXT)
    def test_more_text_never_loses_a_family(self, first, more):
        before, after = set(ldb.classify(first)), set(ldb.classify(first + "\n" + more))
        self.assertLessEqual(before - {"GPL", "BSD-2"}, after)

    @given(st.sampled_from(sorted(PHRASES)), LICENCE_TEXT, LICENCE_TEXT)
    def test_a_fingerprint_is_found_anywhere(self, family, before, after):
        found = ldb.classify(f"{before}\n{PHRASES[family]}\n{after}")
        replaced_by = {"GPL": {"LGPL", "AGPL"}, "BSD-2": {"BSD-3"}}.get(family, set())
        self.assertTrue(family in found or replaced_by & set(found), found)


class PinTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls._tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def files(self, contents: dict[str, bytes], where: str) -> list[Path]:
        base = self.dir / where
        base.mkdir(parents=True, exist_ok=True)
        for old in base.iterdir():
            old.unlink()
        for name, data in contents.items():
            (base / name).write_bytes(data)
        return sorted(base / name for name in contents)

    NAMES = st.text(alphabet="abcdefgh_", min_size=1, max_size=6).map(lambda s: s + ".c")

    @given(st.binary(max_size=200))
    def test_one_file_is_its_sha256_with_lf_lines(self, data):
        (path,) = self.files({"a.c": data}, "one")
        lf = data.replace(b"\r\n", b"\n")
        self.assertEqual(ldb.pin_of([path]), hashlib.sha256(lf).hexdigest())
        (crlf,) = self.files({"a.c": lf.replace(b"\n", b"\r\n")}, "one")
        self.assertEqual(ldb.pin_of([crlf]), hashlib.sha256(lf).hexdigest())

    @given(st.dictionaries(NAMES, st.binary(max_size=40), min_size=2, max_size=4), st.data())
    def test_any_byte_or_name_moves_a_pin(self, contents, data):
        base = self.dir / "many"
        pin = ldb.pin_of(self.files(contents, "many"), base)
        self.assertEqual(ldb.pin_of(self.files(contents, "many"), base), pin, "deterministic")
        name = data.draw(st.sampled_from(sorted(contents)))
        changed = dict(contents, **{name: contents[name] + b"x"})
        self.assertNotEqual(ldb.pin_of(self.files(changed, "many"), base), pin)
        renamed = {("z" + k if k == name else k): v for k, v in contents.items()}
        self.assertNotEqual(ldb.pin_of(self.files(renamed, "many"), base), pin)


# ---------------------------------------------------------------------------
# A tree, and a record of it
# ---------------------------------------------------------------------------

MIT = PHRASES["MIT"] + " of this software, to deal in the Software without restriction."
ZLIB = "This software is provided 'as-is'. " + PHRASES["zlib"] + "."
GPL = "This program is free software under the " + PHRASES["GPL"] + "."


@st.composite
def trees(draw):
    """thirdparty/ with up to four components -- a directory with a LICENSE, a
    single header with its licence at the top, a directory that says it is
    MODIFIED -- each under a licence drawn from MIT, zlib and GPL."""
    components = {}
    for name in draw(st.lists(st.sampled_from(["dep", "single.h", "mod", "other"]),
                              unique=True, max_size=4)):
        components[name] = draw(st.sampled_from([MIT, ZLIB, GPL]))
    return components


def build(root: Path, components: dict) -> None:
    for name, text in components.items():
        if name.endswith(".h"):
            (root / name).write_text(f"/* {text} */\nint x;\n")
            continue
        (root / name).mkdir()
        (root / name / "LICENSE").write_text(text + "\n")
        (root / name / "code.c").write_text("int y;\n")
        # A file in a folder of its own: a pin names each file by its path.
        (root / name / "sub").mkdir()
        (root / name / "sub" / "inner.h").write_text("int z;\n")
        if name == "mod":
            (root / name / "PATCHES.md").write_text("MODIFIED: everything\n")


class CheckTest(unittest.TestCase):
    """check() over generated trees and records."""

    def check(self, components, rows, pins, families=frozenset()):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            root = repo / "thirdparty"
            root.mkdir()
            build(root, components)
            return ldb.check(rows, pins, repo=repo, root=root, families=set(families))

    PATHS = st.sampled_from(["thirdparty/dep", "thirdparty/single.h", "thirdparty/mod",
                             "thirdparty/other", "thirdparty/gone", "thirdparty/dep/code.c",
                             "elsewhere", "/abs/olute", "thirdparty/../x"])

    @example(components={"mod": MIT}, rows=[ldb.Row(
        name="mod", path="/abs/olute", licences="MIT", elect="-", modified="yes", linked="all",
        evidence="file")], pins={})
    @given(trees(), st.lists(row_strategy(st.sampled_from(["dep", "single", "mod", "other"]),
                                          PATHS), max_size=5),
           st.dictionaries(st.sampled_from(["sha256_dep", "sha256_single", "sha256_mod",
                                            "sha256_gone"]), st.text(max_size=8), max_size=3))
    def test_every_rule_fails_the_rows_it_is_about_and_nothing_raises(self, components, rows, pins):
        ok, fails = self.check(components, rows, pins)
        self.assertTrue(all(isinstance(line, str) for line in ok + fails))
        names = [row["name"] for row in rows]
        for name in set(names):
            if names.count(name) > 1:
                self.assertIn(f"{name}: appears {names.count(name)} times in the components block",
                              fails)
        written = {row["path"] for row in rows}
        for component in components:
            if f"thirdparty/{component}" not in written:
                self.assertTrue(any(line.startswith(f"thirdparty/{component}: on disk")
                                    for line in fails), fails)
        for row in rows:
            if row.elected in ldb.REFUSED:
                self.assertTrue(any(line.startswith(f"{row['name']}: {row.elected} -- ")
                                    for line in fails), fails)
        for key in pins:
            if key.startswith("sha256_") and key[len("sha256_"):] not in names:
                self.assertTrue(any(line.startswith(f"{key}: a pin") for line in fails), fails)

    @given(st.lists(st.sampled_from(sorted(ldb.FAMILY_FILES) + ["nowhere"]), unique=True,
                    max_size=4), st.data())
    def test_a_family_that_ships_names_licenses_txt(self, families, data):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "thirdparty").mkdir()
            says = {}
            for family in families:
                rel = ldb.FAMILY_FILES.get(family)
                if rel is None:
                    continue
                says[family] = data.draw(st.sampled_from(["names", "silent", "absent"]))
                if says[family] != "absent":
                    (repo / rel).parent.mkdir(parents=True, exist_ok=True)
                    (repo / rel).write_text("copy LICENSES.txt\n" if says[family] == "names"
                                            else "copy nothing\n")
            ok, fails = ldb.check([], {}, repo=repo, root=repo / "thirdparty",
                                  families=set(families))
        expected_ok, expected_fails = [], []
        for family in sorted(families):
            rel = ldb.FAMILY_FILES.get(family)
            if rel is None:
                expected_fails.append(f"family {family!r} has no packaging file in FAMILY_FILES")
            elif says[family] == "names":
                expected_ok.append(f"{'packaging':24} {family:12} {rel}")
            else:
                expected_fails.append(f"{rel}: packages the {family} family and never mentions "
                                      "LICENSES.txt, so that family ships no third-party notice")
        self.assertEqual((ok, fails), (expected_ok, expected_fails))

    @given(trees())
    def test_a_true_record_of_a_tree_passes(self, components):
        """Every component with the row that says what it is, the zlib ones
        pinned: no failure, and a line for each."""
        family = {MIT: "MIT", ZLIB: "zlib", GPL: "GPL"}
        rows, pins = [], {}
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "thirdparty"
            root.mkdir()
            build(root, components)
            for name, text in sorted(components.items()):
                stem = name.removesuffix(".h")
                modified = "yes" if name == "mod" else "no"
                rows.append(ldb.Row(name=stem, path=f"thirdparty/{name}", licences=family[text],
                                    elect="-", modified=modified, linked="all",
                                    evidence="header" if name.endswith(".h") else "file"))
                if modified == "no":
                    path = root / name
                    pins[f"sha256_{stem}"] = ldb.pin_of(ldb.pinned_sources(path),
                                                        path if path.is_dir() else None)
            ok, fails = ldb.check(rows, pins, repo=Path(tmp), root=root, families=set())
        refused = {row["name"] for row in rows if row.elected in ldb.REFUSED}
        self.assertEqual([line for line in fails if line.split(":")[0] not in refused], [])
        self.assertEqual(len(ok), len(rows) - len(refused))


class MainTest(unittest.TestCase):

    @given(BLOCK)
    def test_a_record_it_cannot_read_is_a_fail_line(self, text):
        """`--check` on a record it cannot read: exit 1 and one FAIL line
        naming where, not a traceback."""
        try:
            ldb.parse_block(text)
        except ValueError as refused:
            out = io.StringIO()
            with mock.patch.object(ldb, "load_rows", lambda: ldb.parse_block(text)), \
                    contextlib.redirect_stdout(out):
                self.assertEqual(ldb.main(["--check"]), 1)
            self.assertEqual(out.getvalue(), f"FAIL: {refused}\n")

    @given(st.lists(TOKEN, max_size=4), st.lists(TOKEN, max_size=4))
    def test_the_report_is_every_line_and_the_verdict(self, ok, fails):
        out = io.StringIO()
        with mock.patch.object(ldb, "load_rows", lambda: []), \
                mock.patch.object(ldb, "frozen_pins", lambda: {}), \
                mock.patch.object(ldb, "check", lambda rows, pins: (list(ok), list(fails))), \
                contextlib.redirect_stdout(out):
            code = ldb.main(["--check"])
        last = (f"FAIL: {len(fails)} licence problem(s) above; {len(ok)} components ok" if fails
                else f"PASS: {len(ok)} components, every licence known, every alteration marked")
        self.assertEqual(out.getvalue().splitlines(),
                         [f"  ok    {line}" for line in ok] + [f"  FAIL  {line}" for line in fails]
                         + [last])
        self.assertEqual(code, 1 if fails else 0)

    @given(st.lists(st.text(alphabet="abc/_.", min_size=1, max_size=8), max_size=4))
    def test_the_inventory_is_printed_a_path_a_line(self, paths):
        out = io.StringIO()
        with mock.patch.object(ldb, "inventory", lambda root: [Path(x) for x in paths]), \
                contextlib.redirect_stdout(out):
            self.assertEqual(ldb.main(["--inventory"]), 0)
        self.assertEqual(out.getvalue().splitlines(), [Path(x).as_posix() for x in paths])


class Scratch(unittest.TestCase):
    """A folder of the test's own, emptied before each example."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def fresh(self) -> Path:
        here = Path(self._tmp.name) / "x"
        shutil.rmtree(here, ignore_errors=True)
        here.mkdir()
        return here


class RegionTest(Scratch):

    @given(st.integers(0, 700), st.sampled_from(["\n", "\r\n"]))
    def test_the_head_and_the_tail_numbered_as_in_the_file(self, count, eol):
        lines = [f"line {i}" for i in range(1, count + 1)]
        path = self.fresh() / "f.c"
        path.write_bytes((eol.join(lines) + (eol if lines else "")).encode())
        if count <= ldb.HEAD_LINES + ldb.TAIL_LINES:
            expected = list(enumerate(lines, 1))
        else:
            expected = (list(enumerate(lines[:ldb.HEAD_LINES], 1))
                        + list(enumerate(lines[-ldb.TAIL_LINES:], count - ldb.TAIL_LINES + 1)))
        self.assertEqual(ldb._region(path), expected)


# Pieces of C, each with what _comment_lines() says of its lines.
SEGMENTS = st.sampled_from([
    (["int x;"], [False]), (["// a note"], [True]), (["/* one line */"], [True]),
    (["/* open", "inside it", "* a star", "close */"], [True, True, True, True]),
    ([""], [True]), (["int y; /* open", "inside", "end */ int z;"], [True, True, True]),
    (["   // indented"], [True]), ([" * a star outside any block"], [True])])


class CommentLinesTest(unittest.TestCase):

    @given(st.lists(SEGMENTS, max_size=8))
    def test_a_line_is_comment_when_it_is_inside_or_starts_one(self, segments):
        lines = [line for seg, _ in segments for line in seg]
        expected = [flag for _, flags in segments for flag in flags]
        self.assertEqual(ldb._comment_lines(list(enumerate(lines, 1))), expected)

    @given(st.lists(SEGMENTS, max_size=4), st.lists(SEGMENTS, max_size=4))
    def test_the_tail_starts_outside_any_comment_of_the_head(self, head, tail):
        """An open block at the end of the head does not run into the tail:
        the numbering jumps, and the state starts again."""
        head_lines = [line for seg, _ in head for line in seg] + ["/* left open", "still"]
        tail_lines = [line for seg, _ in tail for line in seg]
        region = list(enumerate(head_lines, 1)) + list(enumerate(tail_lines, 1000))
        alone = ldb._comment_lines(list(enumerate(tail_lines, 1000)))
        self.assertEqual(ldb._comment_lines(region)[len(head_lines):], alone)


BODY = st.lists(st.text(alphabet="abcdefghij ,.()", min_size=1, max_size=30).map(str.strip)
                .filter(bool), max_size=4)


class NoticeTextTest(Scratch):

    @given(st.sampled_from(sorted(PHRASES)), BODY, BODY, st.integers(0, 3), st.integers(0, 3),
           st.sampled_from(["/*\n * {}\n */", "// {}", "/* {} */"]))
    def test_the_licence_block_and_nothing_but_it(self, family, before, after, code_above,
                                                  code_below, style):
        said = ["Copyright (c) 2026 Somebody", *before, PHRASES[family], *after]
        if style.startswith("// "):
            block = "\n".join(f"// {line}" for line in said)
        elif style.startswith("/* {} */"):
            block = "\n".join(f"/* {line} */" for line in said)
        else:
            block = "/*\n" + "\n".join(f" * {line}" for line in said) + "\n */"
        text = "int above;\n" * code_above + block + "\n" + "int below;\n" * code_below
        path = self.fresh() / "f.h"
        path.write_text(text)
        got = ldb.notice_text(path, "header", [family])
        self.assertIsNotNone(got)
        lines = got.split("\n")
        self.assertEqual(lines, said)
        self.assertEqual(ldb.notice_text(path.parent, "header", [family]), got, "a folder: its .h")

    @given(BODY, st.sampled_from(sorted(PHRASES)))
    def test_no_fingerprint_of_its_families_is_none(self, body, family):
        others = [f for f in PHRASES if f != family and family not in ldb.classify(PHRASES[f])]
        path = self.fresh() / "f.h"
        path.write_text("/*\n" + "\n".join(f" * {line}" for line in body) + "\n */\nint x;\n")
        self.assertIsNone(ldb.notice_text(path, "header", [family]))
        self.assertIsNone(ldb.notice_text(path, "upstream:https://example.org", others))
        self.assertIsNone(ldb.notice_text(path, "part-of:dep", [family]))

    @given(st.sampled_from(sorted(PHRASES)), BODY)
    def test_the_block_with_the_licence_beats_a_line_about_it(self, family, more):
        """A header that says where the licence is matches at the top with one
        line; the licence matches at the bottom with all of its lines."""
        said = [PHRASES[family], *more, "THE SOFTWARE IS PROVIDED AS IS."]
        text = (f"// {PHRASES[family]} -- see the end of the file\nint x;\n"
                + "int y;\n" * 5 + "/*\n" + "\n".join(f" * {line}" for line in said) + "\n */\n")
        path = self.fresh() / "f.h"
        path.write_text(text)
        self.assertEqual(ldb.notice_text(path, "header", [family]).split("\n"), said)

    @given(st.text(alphabet="abc \n", max_size=40), st.sampled_from(ldb.LICENCE_FILES))
    def test_a_licence_file_is_read_as_it_is(self, text, name):
        component = self.fresh() / "dep"
        component.mkdir()
        (component / name).write_text("MIT\n" + text)
        self.assertEqual(ldb.notice_text(component, "file", ["MIT"]), ("MIT\n" + text).rstrip())
        (component / "NOTICE.txt").write_text("named " + text)
        self.assertEqual(ldb.notice_text(component, "file:NOTICE.txt", ["MIT"]),
                         ("named " + text).rstrip())
        self.assertIsNone(ldb.notice_text(component, "file:GONE", ["MIT"]))


class ReadersTest(Scratch):

    @given(st.dictionaries(st.text(alphabet="abcdef_0123", min_size=1, max_size=8),
                           st.text(alphabet="abc 0123:/@._-", min_size=1, max_size=16)
                           .map(str.strip).filter(bool), max_size=5),
           st.lists(st.sampled_from(["", "# a comment", "  "]), max_size=3))
    def test_frozen_pins_read_the_versions_block_and_only_it(self, pins, noise):
        lines = ["# FROZEN_VERSIONS", "outside value", "```versions", *noise]
        lines += [f"{key} {value}" for key, value in pins.items()]
        lines += ["```", "after value", "```versions", "second block"]
        repo = self.fresh()
        (repo / "thirdparty").mkdir()
        (repo / "thirdparty" / "FROZEN_VERSIONS.md").write_text("\n".join(lines) + "\n")
        self.assertEqual(ldb.frozen_pins(repo), pins)

    @given(st.lists(st.text(alphabet="abc/_-.", min_size=1, max_size=10), max_size=4))
    def test_the_submodules_are_the_paths_gitmodules_names(self, paths):
        repo = self.fresh()
        self.assertEqual(ldb.submodule_paths(repo), set())
        text = "".join(f'[submodule "s{i}"]\n\tpath = {path}\n\turl = https://x/{i}\n'
                       for i, path in enumerate(paths))
        (repo / ".gitmodules").write_text(text)
        self.assertEqual(ldb.submodule_paths(repo), set(paths))

    @given(st.sets(st.sampled_from(ldb.LICENCE_FILES), max_size=3),
           st.sampled_from(["file", "header", "file:NOTICE"]), st.booleans())
    def test_the_licence_file_is_the_one_the_evidence_names(self, present, evidence, notice):
        component = self.fresh() / "dep"
        component.mkdir()
        for name in present:
            (component / name).write_text("x")
        if notice:
            (component / "NOTICE").write_text("x")
        got = ldb.licence_file_for(component, evidence)
        if evidence == "file:NOTICE":
            self.assertEqual(got, component / "NOTICE" if notice else None)
        else:
            first = next((n for n in ldb.LICENCE_FILES if n in present), None)
            self.assertEqual(got, component / first if first else None)
        single = component / "one.h"
        single.write_text("x")
        self.assertEqual(ldb.licence_file_for(single, "file"), single, "a bare notice file")

    @given(st.lists(st.tuples(st.sampled_from(["", "sub/", "sub/deeper/"]),
                              st.text(alphabet="abc", min_size=1, max_size=3),
                              st.sampled_from([".c", ".h", ".cpp", ".hpp", ".md", ".txt", ""])),
                    max_size=6))
    def test_a_pin_covers_every_source_at_any_depth_in_path_order(self, files):
        component = self.fresh() / "dep"
        component.mkdir()
        written = set()
        for folder, stem, suffix in files:
            path = component / f"{folder}{stem}{suffix}"
            if path.exists() or any(p == path or path in p.parents for p in written):
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("x")
            written.add(path)
        sources = ldb.pinned_sources(component)
        self.assertEqual(set(sources), {p for p in written if p.suffix in (".c", ".h", ".cpp", ".hpp")})
        self.assertEqual(sources, sorted(sources, key=lambda p: p.relative_to(component).as_posix()))
        self.assertEqual(ldb.pinned_sources(component / "gone"), [])

    @given(st.lists(st.sampled_from(sorted(ldb.LINKED_VALUES)), min_size=1, max_size=3, unique=True)
           .map("|".join), st.sampled_from(["linux", "windows", "apple", "bsd", "macos", "web",
                                            "android", "ios"]))
    def test_a_family_ships_what_reaches_it(self, linked, family):
        row = ldb.Row(name="n", path="p", licences="MIT", elect="-", modified="no",
                      linked=linked, evidence="file")
        reaches = ("all" in row.linked or family in row.linked
                   or ("desktop" in row.linked and family in ("linux", "windows", "apple", "bsd",
                                                             "macos")))
        self.assertEqual(ldb.rows_for([row], family), [row] if reaches else [])


@st.composite
def component_trees(draw):
    """thirdparty/ as license_db.inventory() walks it: top-level files and
    folders, a folder's external/ singles (aes.c and aes.h are one), and
    raylib's src/external/ folders and singles. Returns the files to write and
    the components expected, as paths under thirdparty/."""
    files, expected = [], set()
    for name in draw(st.lists(st.sampled_from(["a.c", "b.h", "c.cpp", "notes.md", "d.hpp"]),
                              unique=True, max_size=3)):
        files.append(name)
        if not name.endswith(".md"):
            expected.add(name)
    for folder in draw(st.lists(st.sampled_from(["dep", "lib"]), unique=True, max_size=2)):
        files.append(f"{folder}/README")
        expected.add(folder)
        for where in draw(st.sampled_from([[], ["external"], ["src/external"],
                                           ["external", "src/external"]])):
            stems = draw(st.dictionaries(st.sampled_from(["aes", "md5", "zip"]),
                                         st.sampled_from([(".c",), (".h",), (".c", ".h"),
                                                          (".cpp", ".hpp")]), max_size=2))
            for stem, suffixes in stems.items():
                for suffix in suffixes:
                    files.append(f"{folder}/{where}/{stem}{suffix}")
                expected.add(f"{folder}/{where}/{stem}{suffixes[0]}")
            if where == "src/external" and draw(st.booleans()):
                files.append(f"{folder}/{where}/glfw/glfw3.h")
                expected.add(f"{folder}/{where}/glfw")
    return files, expected


class InventoryTest(Scratch):

    @given(component_trees())
    def test_every_component_once_at_the_depth_it_lives(self, tree):
        files, expected = tree
        repo = self.fresh()
        root = repo / "thirdparty"
        root.mkdir()
        for name in files:
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text("x")
        got = ldb.inventory(root, repo)
        self.assertEqual(len(got), len(set(got)))
        self.assertEqual({p.as_posix() for p in got}, {f"thirdparty/{e}" for e in expected})
        self.assertEqual(ldb.inventory(repo / "gone", repo), [])


if __name__ == "__main__":
    unittest.main()
