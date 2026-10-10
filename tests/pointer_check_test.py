"""Tests for tools/pointer_check.py: no raw pointer and no C string a game can name.

Run with:  python3 -m unittest discover -s tests -p pointer_check_test.py

The gate reads clang's AST, so every test here needs clang++. Without one they
skip, unless RMP_REQUIRE_CLANG=1 -- which the CI lint job sets, because the
image carries clang and a gate that skipped there would be a green that proved
nothing.

The framework's own headers are the gate's job, `rmp test pointers` runs it
first; these are the gate's tests, against fixtures that hold still.

The red fixture, tests/fixtures/pointer_check/bad, holds one of every shape the
gate refuses, and the test asks for exactly those findings and no others: a
gate that also flagged an exempt operator-> would be as wrong as one that
missed a field, and both would still exit 1.
"""

from __future__ import annotations

import functools
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOL = REPO / "tools" / "pointer_check.py"
FIXTURES = REPO / "tests" / "fixtures" / "pointer_check"
FINDING = re.compile(r"^FAIL: \S+:(\d+): (\S+) -- (returns|parameter \S+|field|variable|alias) ")

# Every finding the red fixture must produce, as (name, role).
BAD_PUBLIC = {
    ("rmp::take", "parameter thing"),
    ("rmp::find_thing", "returns"),
    ("rmp::named", "parameter name"),
    ("rmp::Holder::held", "field"),
    ("rmp::Tag::name", "field"),
    ("rmp::ThingPtr", "alias"),
    ("rmp::hidden", "returns"),
    ("rmp::call", "parameter fn"),
    ("rmp::global_thing", "variable"),
    ("rmp::Box::get", "returns"),
    ("rmp::touch", "parameter friendly"),
    ("rmp::Arrow::operator->", "returns"),
    ("rmp::Outside::later", "returns"),
}
BAD_UNLISTED = {
    ("rmp::Box::_held", "field"),
    ("rmp::detail::unlisted", "parameter thing"),
    ("rmp::Private::_secret", "field"),
}


def clang():
    return os.environ.get("RMP_CLANGXX") or shutil.which("clang++")


def run(*args, env=None):
    return subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True,
                          env=env if env is not None else os.environ.copy())


def fixture(name, ratchet=None):
    root = FIXTURES / name
    return run("--include", str(root), "--ratchet", str(ratchet or root / "ratchet.txt"))


@functools.cache
def red():
    """The red fixture, read once: every red test asks about the same run."""
    return fixture("bad")


def findings(out):
    got = set()
    for line in out.splitlines():
        m = FINDING.match(line)
        if m:
            got.add((m.group(2), m.group(3)))
    return got


class NeedsClang(unittest.TestCase):
    def setUp(self):
        if clang() is None:
            if os.environ.get("RMP_REQUIRE_CLANG") == "1":
                self.fail("RMP_REQUIRE_CLANG=1 and there is no clang++ on PATH")
            self.skipTest("no clang++: the gate cannot read an AST here")


class RedFixtureTest(NeedsClang):
    def test_every_shape_is_found_and_nothing_else(self):
        got = red()
        self.assertEqual(got.returncode, 1, got.stdout)
        self.assertEqual(findings(got.stdout), BAD_PUBLIC | BAD_UNLISTED, got.stdout)

    def test_a_detail_or_private_pointer_names_the_ratchet(self):
        got = red()
        for name, _role in BAD_UNLISTED:
            with self.subTest(name=name):
                at = got.stdout.index(name)
                self.assertIn("not in ratchet.txt", got.stdout[at:at + 300])

    def test_a_dead_line_in_the_ratchet_fails(self):
        got = red()
        self.assertIn("`probe.h rmp::detail::gone` -- it matches nothing any more",
                      got.stdout)

    def test_a_public_pointer_listed_in_the_ratchet_is_still_refused(self):
        got = red()
        self.assertIn("`probe.h rmp::take` -- it is public", got.stdout)
        self.assertIn(("rmp::take", "parameter thing"), findings(got.stdout))

    def test_a_line_without_a_reason_fails(self):
        got = red()
        self.assertIn("the reason is not optional: `probe.h  rmp::detail::noreason`",
                      got.stdout)


class GreenFixtureTest(NeedsClang):
    def test_values_references_and_the_exempt_arrows_pass(self):
        got = fixture("good")
        self.assertEqual(got.returncode, 0, got.stdout)
        self.assertIn("1 public header: no raw pointer", got.stdout)
        self.assertIn("(2 ratchet lines", got.stdout)

    def test_the_ratchet_is_what_lets_a_listed_pointer_through(self):
        with tempfile.TemporaryDirectory() as tmp:
            empty = Path(tmp) / "ratchet.txt"
            empty.write_text("# nothing owed\n")
            got = fixture("good", ratchet=empty)
        self.assertEqual(got.returncode, 1, got.stdout)
        self.assertEqual(findings(got.stdout), {("rmp::detail::listed", "parameter thing"),
                                                ("rmp::Private::_listed", "field")})

    def test_a_line_added_for_nothing_fails_the_green_fixture(self):
        with tempfile.TemporaryDirectory() as tmp:
            padded = Path(tmp) / "ratchet.txt"
            padded.write_text((FIXTURES / "good" / "ratchet.txt").read_text() +
                              "probe.h  rmp::detail::extra -- owed nothing\n")
            got = fixture("good", ratchet=padded)
        self.assertEqual(got.returncode, 1, got.stdout)
        self.assertIn("rmp::detail::extra` -- it matches nothing", got.stdout)

    def test_a_line_listed_twice_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            twice = Path(tmp) / "ratchet.txt"
            line = "probe.h  rmp::detail::listed -- again\n"
            twice.write_text((FIXTURES / "good" / "ratchet.txt").read_text() + line)
            got = fixture("good", ratchet=twice)
        self.assertEqual(got.returncode, 1, got.stdout)
        self.assertIn("listed twice", got.stdout)


class WithoutClangTest(unittest.TestCase):
    """No clang is a skip on a laptop and a failure where it was promised."""

    def without_clang(self, require):
        with tempfile.TemporaryDirectory() as tmp:
            env = {k: v for k, v in os.environ.items() if k != "RMP_CLANGXX"}
            env["PATH"] = tmp  # an empty directory: no clang++, nothing else
            env["RMP_REQUIRE_CLANG"] = "1" if require else "0"
            return run(env=env)

    def test_it_skips_and_says_so(self):
        got = self.without_clang(require=False)
        self.assertEqual(got.returncode, 0, got.stdout)
        self.assertIn("skip  no clang++", got.stdout)

    def test_it_fails_when_clang_was_required(self):
        got = self.without_clang(require=True)
        self.assertEqual(got.returncode, 1, got.stdout)
        self.assertIn("RMP_REQUIRE_CLANG=1", got.stdout)


if __name__ == "__main__":
    unittest.main()
