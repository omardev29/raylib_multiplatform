"""Tests for tools/size_check.py: every release held to its size budget.

Run with:  python3 -m unittest discover -s tests -p size_check_test.py

The binaries are made here, as files of a chosen size that begin the way the
real ones do -- an ELF header, a PE's MZ and PE signature, a fat Mach-O, a
wasm module -- and, for a packed one, with "UPX!" where UPX 5.2.0 put it in
linux-x64-glibc at cf7b676 (offset 324). The bundle is a zip laid out the way
AGP lays out an .aab (base/lib/<abi>/lib<name>.so). Each gate is seen red on
the thing it was written for: a byte over the budget, a stale line, a missing
line, a malformed one.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import re
import sys
import tempfile
import unittest
import unittest.mock
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

spec = importlib.util.spec_from_file_location("size_check", REPO / "tools" / "size_check.py")
sc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sc)

ELF = b"\x7fELF\x02\x01\x01" + b"\0" * 9
WASM = b"\0asm\x01\0\0\0"
FAT_MACHO = b"\xca\xfe\xba\xbe\0\0\0\x02"


def pe_head() -> bytes:
    head = bytearray(0x80)
    head[0:2] = b"MZ"
    head[0x3C:0x40] = (0x40).to_bytes(4, "little")
    head[0x40:0x44] = b"PE\0\0"
    return bytes(head)


def upx(head: bytes) -> bytes:
    """`head`, packed: UPX's stub writes UPX! at offset 324 of an ELF."""
    body = bytearray(head.ljust(400, b"\0"))
    body[324:328] = b"UPX!"
    return bytes(body)


BUDGET = """\
# a comment, and a blank line after it

linux-x64-glibc        elf       100000     40000  # measured at cf7b676
windows-x64            pe        200000         -
macos                  macho          ?         -  # not measured yet
web                    wasm       50000         -
android/arm64-v8a      so         30000         -
android/x86_64         so         31000         -
"""


class Case(unittest.TestCase):
    """A temporary folder holding a budget and the files it is measured from."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))
        self.budget = self.tmp / "size_budget.txt"
        self.budget.write_text(BUDGET)

    def file(self, name: str, head: bytes, size: int) -> Path:
        path = self.tmp / name
        path.write_bytes(head.ljust(size, b"\0")[:size])
        self.assertEqual(path.stat().st_size, size)
        return path

    def run_main(self, *argv, mode="framework") -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), \
                unittest.mock.patch.object(sc, "mode_of", return_value=mode), \
                unittest.mock.patch.dict("os.environ", {"GITHUB_SHA": "a" * 40}):
            code = sc.main(["--budget", str(self.budget), *map(str, argv)])
        return code, out.getvalue()

    def check(self, target, unpacked: Path, shipped: Path | None = None, mode="framework"):
        return self.run_main(target, unpacked, *([shipped] if shipped else []), mode=mode)

    def linux(self, unpacked: int, packed: int | None):
        """linux-x64-glibc as built and as shipped: packed when `packed` is given."""
        built = self.file("built", ELF, unpacked)
        if packed is None:
            return built, None
        return built, self.file("shipped", upx(ELF), packed)


class TheBudgetHoldsTest(Case):
    """Over its budget by one byte fails; at it passes. Under the least its
    line allows by one byte fails; at it passes. For both columns."""

    def test_within_passes_and_prints_the_line(self):
        code, out = self.check("linux-x64-glibc", *self.linux(100000, 40000))
        self.assertEqual(code, 0, out)
        self.assertIn("  ok    linux-x64-glibc", out)
        self.assertIn("linux-x64-glibc        elf       100000     40000  # measured at aaaaaaa",
                      out.splitlines())

    def test_one_byte_over_the_budget_fails(self):
        low, high = sc.bounds(100000)
        self.assertEqual((low, high), (98500, 101500))
        code, out = self.check("linux-x64-glibc", *self.linux(high, 40000))
        self.assertEqual(code, 0, out)
        code, out = self.check("linux-x64-glibc", *self.linux(high + 1, 40000))
        self.assertEqual(code, 1, out)
        self.assertIn("unpacked 101501 bytes is over its budget of 101500", out)
        self.assertIn(f"linux-x64-glibc        elf       {high + 1}     40000  # measured at",
                      out)

    def test_one_packed_byte_over_the_budget_fails(self):
        _, high = sc.bounds(40000)
        code, out = self.check("linux-x64-glibc", *self.linux(100000, high))
        self.assertEqual(code, 0, out)
        code, out = self.check("linux-x64-glibc", *self.linux(100000, high + 1))
        self.assertEqual(code, 1, out)
        self.assertIn(f"packed {high + 1} bytes is over its budget of {high}", out)

    def test_a_stale_line_fails(self):
        """A binary much smaller than its line: the line would hide the next
        growth, so it has to come down."""
        low, _ = sc.bounds(100000)
        code, out = self.check("linux-x64-glibc", *self.linux(low, 40000))
        self.assertEqual(code, 0, out)
        code, out = self.check("linux-x64-glibc", *self.linux(low - 1, 40000))
        self.assertEqual(code, 1, out)
        self.assertIn("is under 98500, the least its line allows", out)
        self.assertIn("The line is stale", out)
        plow, _ = sc.bounds(40000)
        code, out = self.check("linux-x64-glibc", *self.linux(100000, plow - 1))
        self.assertEqual(code, 1, out)
        self.assertIn("The line is stale", out)

    def test_the_band_is_the_one_the_header_states(self):
        header = (REPO / "tools" / "size_budget.txt").read_text()
        said = re.search(r"within (\d+(?:\.\d+)?)% of it, either way", " ".join(header.split()))
        self.assertIsNotNone(said, "the header no longer states the rule")
        self.assertEqual(float(said.group(1)) / 100, float(sc.TOLERANCE))
        self.assertIn(f"is {sc.TOLERANCE * 2:.0%}", " ".join(header.split()),
                      "the header's most a binary can drift unseen is twice the band")

    def test_a_target_with_no_line_fails_and_says_what_to_add(self):
        code, out = self.check("linux-x64-musl", *self.linux(100000, 40000))
        self.assertEqual(code, 1, out)
        self.assertIn("linux-x64-musl has no line in", out)
        self.assertIn("linux-x64-musl         elf       100000     40000  # measured at", out)

    def test_a_line_nobody_measured_fails_and_says_what_to_paste(self):
        code, out = self.check("macos", self.file("mac", FAT_MACHO, 1696040))
        self.assertEqual(code, 1, out)
        self.assertIn("macos: unpacked is ? -- nobody has measured it yet", out)
        self.assertIn("macos                  macho    1696040         -  # measured at", out)

    def test_a_single_file_is_both_the_binary_and_what_ships(self):
        code, out = self.check("web", self.file("w.wasm", WASM, 50000))
        self.assertEqual(code, 0, out)
        code, out = self.check("windows-x64", self.file("a.exe", pe_head(), 200000),
                               self.file("b.exe", pe_head(), 200000))
        self.assertEqual(code, 0, out)


class WhatTheFileIsTest(Case):
    """The kind, and whether UPX packed it, are read off the bytes: a check
    that measured the zip, or the packed copy as the unpacked one, would say
    a wrong number with a straight face."""

    def test_kinds(self):
        self.assertEqual(sc.kind_of(ELF), "elf")
        self.assertEqual(sc.kind_of(WASM), "wasm")
        self.assertEqual(sc.kind_of(FAT_MACHO), "macho")
        self.assertEqual(sc.kind_of(b"\xcf\xfa\xed\xfe\x07\0\0\x01"), "macho")
        self.assertEqual(sc.kind_of(pe_head()), "pe")
        self.assertIsNone(sc.kind_of(b"MZ" + b"\0" * 100), "MZ alone is not a PE")
        self.assertIsNone(sc.kind_of(b"PK\x03\x04"), "a zip is not a binary")

    def test_upx(self):
        self.assertTrue(sc.upx_packed(upx(ELF)))
        self.assertFalse(sc.upx_packed(ELF.ljust(4096, b"\0")))
        pe = bytearray(pe_head().ljust(0x200, b"\0"))
        pe[0x178:0x17C], pe[0x1A0:0x1A4] = b"UPX0", b"UPX1"
        self.assertTrue(sc.upx_packed(bytes(pe)))

    def test_the_line_says_another_kind(self):
        code, out = self.check("windows-x64", self.file("x", ELF, 200000))
        self.assertEqual(code, 1, out)
        self.assertIn("windows-x64: its line says pe and the file is elf", out)

    def test_not_a_binary(self):
        code, out = self.check("web", self.file("web-build.zip", b"PK\x03\x04", 50000))
        self.assertEqual(code, 1, out)
        self.assertIn("is not an executable this knows", out)

    def test_the_unpacked_size_is_read_before_upx(self):
        packed = self.file("p", upx(ELF), 40000)
        code, out = self.check("linux-x64-glibc", packed, packed)
        self.assertEqual(code, 1, out)
        self.assertIn("is packed by UPX already", out)

    def test_a_shipped_file_that_is_neither(self):
        built = self.file("b", ELF, 100000)
        code, out = self.check("windows-x64", self.file("a.exe", pe_head(), 200000),
                               self.file("c.exe", pe_head(), 199000))
        self.assertEqual(code, 1, out)
        self.assertIn("neither the binary nor a packed copy of it", out)
        code, out = self.check("linux-x64-glibc", built, self.tmp / "nothing")
        self.assertEqual(code, 1, out)
        self.assertIn("does not exist", out)

    def test_packed_where_the_line_says_no_upx_and_the_other_way(self):
        code, out = self.check("windows-x64", self.file("a.exe", pe_head(), 200000),
                               self.file("p.exe", upx(pe_head()), 80000))
        self.assertEqual(code, 1, out)
        self.assertIn("packed by UPX, and its line says UPX does not apply", out)
        code, out = self.check("linux-x64-glibc", *self.linux(100000, None))
        self.assertEqual(code, 1, out)
        self.assertIn("its line has a packed size and the file that ships is not packed", out)


class TheFileIsReadStrictlyTest(unittest.TestCase):
    """A malformed line is an error that names its line, never a line that
    is skipped -- a skipped line is a target with no budget, passing."""

    def bad(self, line: str, said: str):
        with self.subTest(line=line):
            with self.assertRaises(sc.BudgetError) as e:
                sc.parse("# header\n" + line + "\n", "budget")
            self.assertIn("budget:2: ", str(e.exception))
            self.assertIn(said, str(e.exception))

    def test_malformed_lines(self):
        self.bad("linux-x64-glibc elf 100000", "3 fields where there are four")
        self.bad("linux-x64-glibc elf 100000 - extra", "5 fields")
        self.bad("linux-x64-glibc exe 100000 -", "kind 'exe' is not one of")
        self.bad("linux-x64-glibc elf 0 -", "unpacked = '0'")
        self.bad("linux-x64-glibc elf -5 -", "unpacked = '-5'")
        self.bad("linux-x64-glibc elf 1.2M -", "unpacked = '1.2M'")
        self.bad("linux-x64-glibc elf 0100 -", "unpacked = '0100'")
        self.bad("linux-x64-glibc elf - -", "unpacked cannot be -")
        self.bad("linux-x64-glibc elf 100 big", "packed = 'big'")
        self.bad("Linux-X64 elf 100 -", "is not a target name")
        self.bad("android/arm64/v8a so 100 -", "is not a target name")
        with self.assertRaises(sc.BudgetError) as e:
            sc.parse("web wasm 1 -\nweb wasm 2 -\n", "b")
        self.assertIn("b:2: web has a line already, at line 1", str(e.exception))

    def test_a_malformed_file_fails_the_job_that_reads_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            budget = Path(tmp) / "b.txt"
            budget.write_text("web wasm 50000\n")
            wasm = Path(tmp) / "w.wasm"
            wasm.write_bytes(WASM.ljust(50000, b"\0"))
            out = io.StringIO()
            with contextlib.redirect_stdout(out), \
                    unittest.mock.patch.object(sc, "mode_of", return_value="framework"):
                code = sc.main(["--budget", str(budget), "web", str(wasm)])
        self.assertEqual(code, 1, out.getvalue())
        self.assertIn("3 fields where there are four", out.getvalue())

    def test_what_parses(self):
        lines = sc.parse(BUDGET)
        self.assertEqual(list(lines), ["linux-x64-glibc", "windows-x64", "macos", "web",
                                       "android/arm64-v8a", "android/x86_64"])
        self.assertEqual(lines["linux-x64-glibc"],
                         sc.Line(3, "linux-x64-glibc", "elf", 100000, 40000, "measured at cf7b676"))
        self.assertEqual(lines["macos"].unpacked, None)
        self.assertEqual(lines["windows-x64"].packed, sc.NOT_PACKED)

    def test_the_printed_line_reads_back(self):
        for m in (sc.Measured("linux-x64-glibc", "elf", 1234656, 365836),
                  sc.Measured("android/armeabi-v7a", "so", 1727196, "-"),
                  sc.Measured("linux-arm64-glibc-drm", "elf", 1, "-")):
            with self.subTest(key=m.key):
                line = sc.render(m, "cf7b676")
                back = sc.parse(line)[m.key]
                self.assertEqual((back.kind, back.unpacked, back.packed),
                                 (m.kind, m.unpacked, m.packed))
                self.assertEqual(sc.measured_in("2026-10-11T00:00:00Z " + line),
                                 ({m.key: m}, "cf7b676"))
        here = sc.render(sc.Measured("web", "wasm", 5, "-"), "")
        self.assertTrue(here.endswith("  # measured here"), here)
        self.assertEqual(sc.measured_in(here)[1], "")


class UsageTest(unittest.TestCase):
    def test_what_it_does_not_take(self):
        for argv in ([], ["--check-file", "extra"], ["--update"], ["web"],
                     ["web", "a", "b", "c"], ["--bogus", "a"], ["android", "a", "b"]):
            with self.subTest(argv=argv):
                with self.assertRaises(sc.Usage):
                    sc.main(argv)


class NoBudgetTest(Case):
    """The framework without its file fails; a game without one prints its
    lines and passes, because `rmp new` gives it none."""

    def test_the_framework_without_its_file_fails(self):
        self.budget.unlink()
        code, out = self.check("web", self.file("w.wasm", WASM, 50000))
        self.assertEqual(code, 1, out)
        self.assertIn("is missing, and the framework's every release is held to it", out)
        code, out = self.run_main("--check-file")
        self.assertEqual(code, 1, out)

    def test_a_game_without_one_prints_its_lines_and_passes(self):
        self.budget.unlink()
        code, out = self.check("web", self.file("w.wasm", WASM, 50000), mode="game")
        self.assertEqual(code, 0, out)
        self.assertIn("web                    wasm       50000         -  # measured at", out)
        self.assertIn("skip  this game has no", out)
        code, out = self.run_main("--check-file", mode="game")
        self.assertEqual(code, 0, out)
        self.assertIn("skip", out)

    def test_a_game_with_one_is_held_to_it(self):
        code, out = self.check("web", self.file("w.wasm", WASM, 60000), mode="game")
        self.assertEqual(code, 1, out)
        self.assertIn("over its budget", out)

    def test_the_rule_for_which_is_which_is_rmps(self):
        self.assertEqual(sc.mode_of(), "framework")
        rmp_py = (REPO / "tools" / "size_check.py").read_text()
        self.assertIn("rmp.mode_of(REPO)", rmp_py)


class AndroidTest(Case):
    """The .so files of a bundle, per ABI, as they sit in it."""

    def bundle(self, libs: dict[str, bytes], name="app-release.aab", prefix="base/") -> Path:
        path = self.tmp / name
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.writestr("base/manifest/AndroidManifest.xml", b"x")
            for entry, data in libs.items():
                zf.writestr(prefix + entry, data)
        return path

    def so(self, size: int) -> bytes:
        return ELF.ljust(size, b"\0")

    def test_each_abi_is_held_to_its_line(self):
        aab = self.bundle({"lib/arm64-v8a/libgame.so": self.so(30000),
                           "lib/x86_64/libgame.so": self.so(31000)})
        code, out = self.run_main("android", aab)
        self.assertEqual(code, 0, out)
        self.assertIn("android/arm64-v8a      so         30000         -  # measured at", out)
        aab = self.bundle({"lib/arm64-v8a/libgame.so": self.so(30451),
                           "lib/x86_64/libgame.so": self.so(31000)})
        code, out = self.run_main("android", aab)
        self.assertEqual(code, 1, out)
        self.assertIn("android/arm64-v8a: unpacked 30451 bytes is over its budget of 30450", out)

    def test_an_apk_reads_the_same(self):
        apk = self.bundle({"lib/arm64-v8a/libgame.so": self.so(30000),
                           "lib/x86_64/libgame.so": self.so(31000)}, "a.apk", prefix="")
        self.assertEqual(self.run_main("android", apk)[0], 0)

    def test_every_library_of_an_abi_counts(self):
        aab = self.bundle({"lib/arm64-v8a/libgame.so": self.so(20000),
                           "lib/arm64-v8a/libc++_shared.so": self.so(10000),
                           "lib/x86_64/libgame.so": self.so(31000)})
        code, out = self.run_main("android", aab)
        self.assertEqual(code, 0, out)
        self.assertIn("android/arm64-v8a      so         30000", out)

    def test_an_abi_with_no_line_and_a_line_with_no_abi(self):
        aab = self.bundle({"lib/arm64-v8a/libgame.so": self.so(30000),
                           "lib/x86/libgame.so": self.so(29000)})
        code, out = self.run_main("android", aab)
        self.assertEqual(code, 1, out)
        self.assertIn("android/x86 has no line", out)
        self.assertIn("android/x86_64: the bundle carries no x86_64 library", out)

    def test_what_is_not_a_library(self):
        aab = self.bundle({"lib/arm64-v8a/libgame.so": b"not an elf".ljust(30000, b"\0"),
                           "lib/x86_64/libgame.so": self.so(31000)})
        code, out = self.run_main("android", aab)
        self.assertEqual(code, 1, out)
        self.assertIn("is not an ELF library", out)
        code, out = self.run_main("android", self.bundle({}))
        self.assertEqual(code, 1, out)
        self.assertIn("carries no native library at all", out)
        code, out = self.run_main("android", self.file("x.aab", b"nope", 10))
        self.assertEqual(code, 1, out)
        self.assertIn("is not a zip", out)


class TheFileOnItsOwnTest(unittest.TestCase):
    """--check-file: one line for each binary an enabled target ships, no name
    that is not one, the kind of its family, and the packed column exactly
    where [upx] packs."""

    KNOWN = {"linux-x64-glibc": "linux", "windows-x64": "windows", "macos": "apple",
             "ios": "apple", "web": "web", "android": "android", "freebsd-x64": "bsd"}

    def problems(self, text: str, enabled=None, packed=("linux-x64-glibc",)):
        enabled = list(enabled if enabled is not None else
                       ["linux-x64-glibc", "windows-x64", "macos", "ios", "web", "android"])
        return sc.check_file(sc.parse(text), self.KNOWN, enabled, list(packed))

    GOOD = BUDGET + "android/armeabi-v7a so 1 -\nandroid/x86 so 1 -\n"

    def test_the_good_one(self):
        problems, notes = self.problems(self.GOOD)
        self.assertEqual(problems, [])
        self.assertEqual(notes, ["macos"])

    def test_a_missing_line(self):
        problems, _ = self.problems(self.GOOD.replace("web ", "# web "))
        self.assertEqual(problems, ["web has no line, and it ships. Its release job prints "
                                    "the line to add; ? holds the place until it runs"])
        problems, _ = self.problems(self.GOOD.replace("android/x86 so 1 -\n", ""))
        self.assertIn("android/x86 has no line, and it ships. Its release job prints the "
                      "line to add; ? holds the place until it runs", problems)

    def test_a_disabled_target_needs_no_line_and_ios_never_has_one(self):
        problems, _ = self.problems("web wasm 1 -\n", enabled=["web", "ios"])
        self.assertEqual(problems, [])
        problems, _ = self.problems("web wasm 1 -\nios macho 1 -\n", enabled=["web", "ios"])
        self.assertEqual(len(problems), 1)
        self.assertIn("ios is not a binary any target ships -- CI builds it for the simulator",
                      problems[0])

    def test_names_and_kinds(self):
        problems, _ = self.problems("playstation elf 1 -\n", enabled=[])
        self.assertEqual(problems, ["line 1: playstation is not a binary any target ships"])
        problems, _ = self.problems("android/mips so 1 -\n", enabled=[])
        self.assertEqual(problems, ["line 1: android/mips is not a binary any target ships"])
        problems, _ = self.problems("freebsd-x64 macho 1 -\n", enabled=[])
        self.assertEqual(problems, ["line 1: freebsd-x64 is elf, and its line says macho"])

    def test_the_packed_column_follows_upx(self):
        problems, _ = self.problems("linux-x64-glibc elf 1 -\n", enabled=[])
        self.assertIn("[upx] packs linux-x64-glibc, and its line has no packed size", problems[0])
        problems, _ = self.problems("linux-x64-glibc elf 1 ?\n", enabled=[])
        self.assertEqual(problems, [])
        problems, _ = self.problems("windows-x64 pe 1 5\n", enabled=[])
        self.assertIn("windows-x64 is not in [upx] enabled, and its line has a packed size",
                      problems[0])

    def test_the_real_file_covers_the_real_project(self):
        lines = sc.load(REPO / sc.BUDGET)
        known, enabled, packed = sc.project()
        problems, notes = sc.check_file(lines, known, enabled, packed)
        self.assertEqual(problems, [])
        self.assertEqual(sorted(sc.required_keys(list(known))), sorted(lines),
                         "the framework builds every target, so it has every line")

    def test_the_real_file_is_laid_out_the_way_update_writes_it(self):
        """--update rewrites a line with render(); a line laid out any other
        way would turn every update into a diff of whitespace."""
        lines = sc.load(REPO / sc.BUDGET)
        raw = (REPO / sc.BUDGET).read_text().splitlines()
        for key, line in lines.items():
            with self.subTest(key=key):
                m = sc.Measured(key, line.kind, 0, "-")
                want = sc.render(m, "x").split("#")[0]
                got = raw[line.number - 1].split("#")[0]
                self.assertEqual(len(got), len(want), f"{key} is not in the columns")
                self.assertEqual(got[:29], want[:29])

    def test_the_cli_on_the_framework(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = sc.main(["--check-file"])
        self.assertEqual(code, 0, out.getvalue())
        self.assertIn("one for each binary that ships", out.getvalue())


class UpdateTest(unittest.TestCase):
    """One CI run's log is one commit: --update reads the measured lines out of
    `gh run view --log`, prefixes and all."""

    LOG = ("x64\tWithin its size budget\t2026-10-11T10:00:00.1Z "
           "linux-x64-glibc        elf       100900     40100  # measured at 1234567\n"
           "macos\tWithin its size budget\t2026-10-11T10:00:01.1Z   ok    nothing to see\n"
           "macos\tWithin its size budget\t2026-10-11T10:00:02.1Z "
           "macos                  macho    1730000         -  # measured at 1234567\n"
           "musl\tWithin its size budget\t2026-10-11T10:00:03.1Z "
           "linux-x64-musl         elf      1232872    365196  # measured at 1234567\n")

    def update(self, log: str) -> tuple[int, str, str]:
        with tempfile.TemporaryDirectory() as tmp:
            budget = Path(tmp) / "b.txt"
            budget.write_text(BUDGET)
            logfile = Path(tmp) / "run.log"
            logfile.write_text(log)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = sc.main(["--budget", str(budget), "--update", str(logfile)])
            return code, out.getvalue(), budget.read_text()

    def test_it_rewrites_what_the_log_measured(self):
        code, out, text = self.update(self.LOG)
        self.assertEqual(code, 0, out)
        lines = sc.parse(text)
        self.assertEqual((lines["linux-x64-glibc"].unpacked, lines["linux-x64-glibc"].packed),
                         (100900, 40100))
        self.assertEqual(lines["macos"].unpacked, 1730000)
        self.assertEqual(lines["linux-x64-musl"].packed, 365196, "a new target is added")
        self.assertEqual(lines["web"].unpacked, 50000, "what the log did not measure stays")
        self.assertIn("linux-x64-musl (new)", out)
        self.assertIn("the log measured nothing for: windows-x64, web", out)
        self.assertTrue(text.startswith("# a comment"), "the header stays")

    def test_it_refuses_two_commits_and_two_answers(self):
        code, out, text = self.update(self.LOG + self.LOG.replace("1234567", "7654321"))
        self.assertEqual(code, 1, out)
        self.assertIn("measurements of 2 commits", out)
        self.assertEqual(text, BUDGET)
        code, out, text = self.update(self.LOG + self.LOG.replace("100900", "100901"))
        self.assertEqual(code, 1, out)
        self.assertIn("measured linux-x64-glibc twice, differently", out)
        self.assertEqual(text, BUDGET)

    def test_a_log_with_nothing_measured(self):
        code, out, text = self.update("x64\tBuild\t2026 nothing here\n")
        self.assertEqual(code, 1, out)
        self.assertEqual(text, BUDGET)


if __name__ == "__main__":
    sys.exit(unittest.main())
