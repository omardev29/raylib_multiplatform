"""tools/binary_check.py's readers, held to their promise for ANY bytes.

What CI hands binary_check.py is whatever the linker wrote -- or a download
cut short, a file UPX already packed, a binary of another kind under the
right name. The promise (its docstring: exit 0, 1 or 2, and 2 for "not a
binary this reads") is that every one of those is a verdict: the readers
raise NotReadable and nothing else, and each check returns 0, 1 or 2 and
says why. A traceback in a release job is a red that names nobody.

The inputs are the binaries tests/binary_check_test.py builds the way real
linkers lay them out -- a PE with imports, delay-loads and an icon group, thin
and fat Mach-O, ELF of both widths and byte orders -- each then cut, poked and
extended at random: a reader meets every offset it follows pointing somewhere
else, and every table ending early.

Every property was seen red; the commit that added this file says how, and
each counterexample is a named case in binary_check_test.py.
"""

from __future__ import annotations

import contextlib
import io
import re
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hypothesis_profiles  # noqa: E402,F401  (first: the profile, or the skip)
from hypothesis import given, strategies as st  # noqa: E402

# The builders, and the module they built against: tools/binary_check.py
# loaded once, as `tools.binary_check` -- the name mutmut records hits under.
import binary_check_test as built  # noqa: E402

bc = built.bc

PES = [
    built.pe(built.MINGW_STATIC),
    built.pe(built.MINGW_DYNAMIC, delay=["xinput1_4.dll"]),
    built.pe(["KERNEL32.dll"], delay=["WINMM.dll"], old_delay=True, pe32=True),
    built.pe(["KERNEL32.dll"], groups=[("IDI_ICON1", [(16, 1), (256, 2)])], icon_ids=[1, 2]),
    built.pe(["KERNEL32.dll"], groups=[(7, [(32, 3)])], icon_ids=[3], pe32=True,
             machine=0x14C),
]
MACHOS = [
    built.thin(built.ARM64, [built.build_version("11.0")]),
    built.thin(7, [built.version_min("10.13")], wide=False),
    built.fat((built.X86_64, built.thin(built.X86_64, [built.build_version("10.15")])),
              (built.ARM64, built.thin(built.ARM64, [built.build_version("11.0")]))),
    built.fat((built.X86_64, built.thin(built.X86_64, [built.version_min("10.15")])),
              wide=True),
    # Read to its last load command, where nothing says which macOS it needs.
    built.thin(built.X86_64, []),
    built.thin(built.ARM64, [built.build_version("11.0", platform=2)]),
]
ELFS = [
    built.elf(),
    built.elf(**built.ZIG_RELEASE),
    built.elf(**built.MUSL_RELEASE),
    built.elf(**built.MUSL_HEADLESS),
    built.elf(wide=False, big=True, needed=["libc.so", "libc.so.6"]),
    built.elf(machine=0xB7, interp_path="/lib/ld-musl-aarch64.so.1", needed=["libc.so"]),
]

# The numbers a reader is most likely to trust: zero, the top of a field, one
# past the end of the file, a sign bit.
EXTREMES = st.sampled_from([0, 1, 7, 8, 0x40, 0x7F, 0x80, 0xFF, 0x7FFF, 0xFFFF, 0x7FFFFFFF,
                            0x80000000, 0xFFFFFFFF, 0x10000, 0x1000]) | st.integers(0, 0xFFFFFFFF)

# Offsets near the start, where every header and table this reads begins, as
# often as anywhere.
AT = st.integers(0, 0x240) | st.integers(0, 1 << 20)

OPS = st.one_of(
    st.tuples(st.just("cut"), AT),
    st.tuples(st.just("byte"), AT, st.integers(0, 255)),
    st.tuples(st.just("u32"), AT, EXTREMES, st.sampled_from("<>")),
    st.tuples(st.just("u64"), AT, EXTREMES | st.just(1 << 63),
              st.sampled_from("<>")),
    st.tuples(st.just("insert"), AT, st.binary(max_size=16)),
)


def mutate(data: bytes, ops) -> bytes:
    out = bytearray(data)
    for op in ops:
        if not out and op[0] != "insert":
            continue
        at = op[1] % (len(out) + (1 if op[0] in ("cut", "insert") else 0))
        if op[0] == "cut":
            del out[at:]
        elif op[0] == "byte":
            out[at] = op[2]
        elif op[0] == "u32":
            packed = struct.pack(op[3] + "I", op[2])
            out[at:at + 4] = packed[:len(out) - at] if at + 4 > len(out) else packed
        elif op[0] == "u64":
            packed = struct.pack(op[3] + "Q", op[2])
            out[at:at + 8] = packed[:len(out) - at] if at + 8 > len(out) else packed
        else:
            out[at:at] = op[2]
    return bytes(out)


def damaged(seeds):
    """A binary cut short -- the commonest damage there is, a download that
    stopped -- or cut, poked and extended at random."""
    return st.one_of(
        st.builds(lambda data, n: data[:n], st.sampled_from(seeds), AT),
        st.builds(mutate, st.sampled_from(seeds), st.lists(OPS, max_size=6)))


# Anything at all, behind the first bytes each reader decides on.
MAGICS = st.sampled_from([b"", b"MZ", b"\x7fELF", b"\x7fELF\x02\x01", b"\x7fELF\x01\x02",
                          b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe",
                          b"\xca\xfe\xba\xbf"])
NOISE = st.builds(lambda head, tail: head + tail, MAGICS, st.binary(max_size=300))


def read_pe(data: bytes):
    pe = bc.Pe(data)
    return pe.machine, pe.imports(), pe.icon_groups()


def read_elf(data: bytes):
    elf = bc.Elf(data)
    return bc.elf_findings(elf), bc.musl_findings(elf), elf.interpreter(), elf.needed()


class Run:
    """A check run on bytes in a file, as CI runs it: its exit code and what
    it printed."""

    def __init__(self, data: bytes):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = str(Path(self.tmp.name) / "game")
        Path(self.path).write_bytes(data)

    def __call__(self, check, *args):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = check(*args)
        return code, out.getvalue()

    def close(self):
        self.tmp.cleanup()


def verdict(check, data: bytes, *args):
    """(exit code, what it printed with the file's path as `game`)."""
    run = Run(data)
    try:
        code, out = run(check, run.path if check is bc.check_macos else [run.path], *args)
        return code, out.replace(run.path, "game")
    finally:
        run.close()


def holds_its_exit_promise(case, code, out, path_said=True):
    """0 is a PASS, 1 a FAIL with what failed, 2 a FAIL: <file>: <why>."""
    case.assertIn(code, (0, 1, 2), out)
    if code == 0:
        case.assertIn("PASS: ", out)
    elif code == 1:
        case.assertIn("  FAIL  ", out)
    elif path_said:
        case.assertRegex(out, r"(^|\n)FAIL: game: ", out)


class ReadersSayNotReadableTest(unittest.TestCase):
    """Reading anything raises NotReadable or gives an answer: never
    struct.error, IndexError, ValueError or anything else a reader did not
    mean to raise."""

    @given(st.one_of(damaged(PES), NOISE))
    def test_a_pe(self, data):
        try:
            read_pe(data)
        except bc.NotReadable:
            pass

    @given(st.one_of(damaged(MACHOS), NOISE))
    def test_a_mach_o(self, data):
        try:
            bc.slices(data)
        except bc.NotReadable:
            pass

    @given(st.one_of(damaged(ELFS), NOISE))
    def test_an_elf(self, data):
        try:
            read_elf(data)
        except bc.NotReadable:
            pass


class ChecksAlwaysGiveAVerdictTest(unittest.TestCase):
    """What CI runs -- the check, on a file -- exits 0, 1 or 2 for any bytes,
    and says which."""

    @given(st.one_of(damaged(PES), NOISE), st.booleans())
    def test_windows(self, data, icon):
        holds_its_exit_promise(self, *verdict(bc.check_windows, data, icon))

    @given(st.one_of(damaged(MACHOS), NOISE),
           st.sampled_from(["10.15", "11.0", "26.0", "10.15.1"]))
    def test_macos(self, data, min_os):
        holds_its_exit_promise(self, *verdict(bc.check_macos, data, min_os))

    @given(st.one_of(damaged(ELFS), NOISE), st.booleans(), st.booleans())
    def test_elf(self, data, pie, musl):
        holds_its_exit_promise(self, *verdict(bc.check_elf, data, pie, musl))


class EveryCutIsAVerdictTest(unittest.TestCase):
    """The commonest damage there is, a download that stopped, at EVERY
    length of every binary built here -- not the cuts a search happens to
    draw. The ci profile's 200 draws, a quarter of them cuts and those spread
    over a megabyte, reached a 64-bit ELF cut inside its dynamic section
    without --musl -- the traceback 075803f fixed -- in none: with the fix
    taken back out, ChecksAlwaysGiveAVerdictTest stayed green."""

    def every_cut(self, seeds, check, *flags):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "game"
            for s, seed in enumerate(seeds):
                for n in range(len(seed) + 1):
                    path.write_bytes(seed[:n])
                    for args in flags:
                        target = str(path) if check is bc.check_macos else [str(path)]
                        out = io.StringIO()
                        with contextlib.redirect_stdout(out):
                            code = check(target, *args)
                        said = out.getvalue().replace(str(path), "game")
                        with self.subTest(seed=s, cut=n, args=args):
                            holds_its_exit_promise(self, code, said)

    def test_windows(self):
        self.every_cut(PES, bc.check_windows, (False,), (True,))

    def test_macos(self):
        self.every_cut(MACHOS, bc.check_macos, ("10.15",))

    def test_elf(self):
        self.every_cut(ELFS, bc.check_elf, (True, False), (True, True), (False, False))


# What can follow a binary: anything, zeros, and bytes shaped like the records
# a reader walks -- a Mach-O load command (a version one among them), an ELF
# dynamic entry -- so that a reader that walked on would find something.
TAIL = st.one_of(
    st.binary(min_size=1, max_size=64),
    st.sampled_from([b"\0" * 8, b"\0" * 16, b"\xff" * 16]),
    st.builds(lambda cmd, size, rest: struct.pack("<II", cmd, size) + rest,
              st.sampled_from([bc.LC_BUILD_VERSION, bc.LC_VERSION_MIN_MACOSX, 0x19, 0]),
              st.integers(0, 64), st.binary(max_size=24)),
    st.builds(lambda tag, value: struct.pack("<qQ", tag, value),
              st.sampled_from([bc.DT_NEEDED, bc.DT_FLAGS, bc.DT_FLAGS_1, bc.DT_BIND_NOW]),
              st.integers(0, 0xFFFFFFFF)),
)


class TrailingBytesChangeNothingTest(unittest.TestCase):
    """A reader follows offsets and sizes, and never reads to the end of the
    file: what a linker wrote reads the same with anything after it -- a PE
    overlay, an installer's payload, padding. A verdict that changed with
    bytes nobody points at would be reading them."""

    @given(st.sampled_from(PES), TAIL, st.booleans())
    def test_windows(self, data, tail, icon):
        self.assertEqual(verdict(bc.check_windows, data, icon),
                         verdict(bc.check_windows, data + tail, icon))

    @given(st.sampled_from(MACHOS), TAIL)
    def test_macos(self, data, tail):
        self.assertEqual(verdict(bc.check_macos, data, "10.15"),
                         verdict(bc.check_macos, data + tail, "10.15"))

    @given(st.sampled_from(ELFS), TAIL, st.booleans())
    def test_elf(self, data, tail, musl):
        self.assertEqual(verdict(bc.check_elf, data, True, musl),
                         verdict(bc.check_elf, data + tail, True, musl))


# A version as a person or a script writes one wrong: what int() takes and a
# version does not (a sign, an underscore, digits of another script), what
# isdigit() takes and int() does not (a superscript), one part too many.
NEAR_VERSIONS = st.sampled_from([
    "10.15", " 10.15 ", "+10.15", "10.+15", "-1", "1_0.15", "10..15", "10.15.", ".10",
    "10.15.1.2", "", " ", "\u0661\u0660.\u0661\u0665", "10.\u0661\u0665", "\uff11\uff10",
    "10\u00b2", "\u00b2", "10.15\n", "10,15"])


class MinOsTest(unittest.TestCase):
    """--min-os is read or refused, never half-read: a version comes back as
    the numbers written, and anything else is a ValueError, which the check
    turns into exit 2. A version is one to three groups of ASCII digits:
    the same word tools/header_budget.txt holds a count to, and P8's for a
    tag -- int() reads Arabic-Indic and full-width digits, and a version a
    person cannot read as written is no version."""

    @given(st.lists(st.integers(0, 10**6), min_size=1, max_size=3))
    def test_a_version_reads_as_written(self, parts):
        text = ".".join(map(str, parts))
        self.assertEqual(bc.parse_version(text), tuple(parts + [0] * (3 - len(parts))))

    @given(NEAR_VERSIONS | st.text(alphabet="0123456789.-+_ \t\u0661\u00b2", max_size=12)
           | st.text(max_size=12))
    def test_anything_else_is_a_value_error(self, text):
        written = re.fullmatch(r"[0-9]+(\.[0-9]+){0,2}", text.strip())
        try:
            got = bc.parse_version(text)
        except ValueError:
            self.assertIsNone(written, f"{text!r} is a version and was refused")
            return
        self.assertIsNotNone(written, f"{text!r} is no version and read as {got}")
        parts = [int(p) for p in text.strip().split(".")]
        self.assertEqual(got, tuple(parts + [0] * (3 - len(parts))))


if __name__ == "__main__":
    unittest.main()
