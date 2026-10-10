"""Tests for tools/binary_check.py: the readers that judge what ships.

Run with:  python3 -m unittest discover -s tests -p binary_check_test.py

The binaries are built here, byte by byte, rather than committed: a PE with an
import table, a delay-load table and an icon group, and a fat Mach-O with a
minimum OS per slice. The layouts were checked against what real tools write
-- the Windows x64 .exe of CI run 37229289825 (MinGW, libstdc++-6.dll), the
arm64 one of 37231088726 (MSVC /MD, VCRUNTIME140.dll), its macOS universal
binary (minos 26.0 in both slices), and executables linked by zig with an .rc
whose group is NAMED, which is what `IDI_ICON1 ICON "app_icon.ico"` makes --
reading numeric ids only, the first version of the reader missed it.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import struct
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

spec = importlib.util.spec_from_file_location("binary_check", REPO / "tools" / "binary_check.py")
bc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bc)

# What the two real Windows builds imported, in the order they imported it.
UCRT = [f"api-ms-win-crt-{n}-l1-1-0.dll" for n in (
    "convert", "environment", "filesystem", "heap", "locale", "math", "private",
    "runtime", "stdio", "string", "time", "utility")]
MINGW_DYNAMIC = ["libgcc_s_seh-1.dll", "GDI32.dll", "KERNEL32.dll", *UCRT, "SHELL32.dll",
                 "USER32.dll", "WINMM.dll", "libstdc++-6.dll"]
MINGW_STATIC = [d for d in MINGW_DYNAMIC if not d.startswith("lib")]
MSVC_MD = ["WINMM.dll", "KERNEL32.dll", "USER32.dll", "GDI32.dll", "SHELL32.dll",
           "MSVCP140.dll", "VCRUNTIME140.dll", *UCRT]


# ---------------------------------------------------------------------------
# A PE, built
# ---------------------------------------------------------------------------

SECTION_VA = 0x1000
SECTION_RAW = 0x200


def resources(groups, icon_ids, va):
    """A resource section at `va`: RT_ICON holding `icon_ids`, and
    RT_GROUP_ICON holding one group per (name, [(size, icon id), ...]) of
    `groups`, the name an int id or a string.

    Three levels of directory -- type, name, language -- and the language
    entry points at a data entry, which holds the RVA of the bytes. Named
    entries come before numbered ones in a directory, and a name is stored
    elsewhere in the section as a length and UTF-16."""
    blobs = []
    for name, sizes in groups:
        blob = struct.pack("<HHH", 0, 1, len(sizes))
        for size, icon in sizes:
            blob += struct.pack("<BBBBHHIH", size % 256, size % 256, 0, 0, 1, 32, 40, icon)
        blobs.append((name, blob))
    types = [(bc.RT_ICON, [(i, b"\x89PNG fake icon data") for i in icon_ids]),
             (bc.RT_GROUP_ICON, blobs)]

    out = bytearray()
    names = []      # (where the name field is, the name)
    leaves = []     # (where the language entry is, the bytes)

    def directory(entries):
        """Writes one directory with its entries zeroed; returns its offset and
        each entry's offset, in the order written."""
        ordered = ([e for e in entries if isinstance(e[0], str)]
                   + [e for e in entries if not isinstance(e[0], str)])
        named = sum(isinstance(k, str) for k, _ in ordered)
        at = len(out)
        out.extend(struct.pack("<IIHHHH", 0, 0, 0, 0, named, len(ordered) - named))
        slots = []
        for key, payload in ordered:
            slots.append((len(out), key, payload))
            out.extend(b"\0" * 8)
        return at, slots

    def link(slot, key, child):
        if isinstance(key, str):
            names.append((slot, key))
        else:
            struct.pack_into("<I", out, slot, key)
        struct.pack_into("<I", out, slot + 4, child | 0x80000000)

    _root, type_slots = directory(types)
    for slot, kind, entries in type_slots:
        at, name_slots = directory(entries)
        link(slot, kind, at)
        for name_slot, name, data in name_slots:
            at, lang_slots = directory([(1033, data)])
            link(name_slot, name, at)
            leaves.append((lang_slots[0][0], data))
    data_entries = []
    for slot, data in leaves:
        struct.pack_into("<II", out, slot, 1033, len(out))   # a leaf: no high bit
        data_entries.append((len(out), data))
        out.extend(b"\0" * 16)
    for slot, name in names:
        struct.pack_into("<I", out, slot, len(out) | 0x80000000)
        out.extend(struct.pack("<H", len(name)) + name.encode("utf-16-le"))
    for entry, data in data_entries:
        while len(out) % 4:
            out.append(0)
        struct.pack_into("<II", out, entry, va + len(out), len(data))
        out.extend(data)
    return bytes(out)


def pe(imports=(), delay=(), groups=(), icon_ids=(), machine=0x8664, pe32=False,
       old_delay=False):
    """A Windows executable: the import tables in .rdata, the resources in a
    .rsrc after it, as a linker lays them out."""
    image_base = 0x400000 if pe32 else 0x140000000
    body = bytearray()
    imp_at = 0
    body.extend(b"\0" * (20 * (len(imports) + 1)))
    delay_at = len(body)
    body.extend(b"\0" * (32 * (len(delay) + 1)))
    # Every DLL's thunk list is the same empty one: a terminator, no symbols.
    thunks = SECTION_VA + len(body)
    body.extend(b"\0" * 8)
    for i, name in enumerate(imports):
        rva = SECTION_VA + len(body)
        body.extend(name.encode() + b"\0")
        struct.pack_into("<IIIII", body, imp_at + 20 * i, thunks, 0, 0, rva, thunks)
    for i, name in enumerate(delay):
        rva = SECTION_VA + len(body)
        body.extend(name.encode() + b"\0")
        attributes, field = (0, rva + image_base) if old_delay else (1, rva)
        tables = (thunks + image_base) if old_delay else thunks
        struct.pack_into("<IIIIII", body, delay_at + 32 * i, attributes, field, tables,
                         tables, tables, 0)
    while len(body) % 0x200:
        body.append(0)
    rsrc_va = SECTION_VA + 0x1000 * (1 + len(body) // 0x1000)
    rsrc = resources(groups, icon_ids, rsrc_va) if groups or icon_ids else b""

    dirs = [(0, 0)] * 16
    if imports:
        dirs[1] = (SECTION_VA + imp_at, 20 * (len(imports) + 1))
    if rsrc:
        dirs[2] = (rsrc_va, len(rsrc))
    if delay:
        dirs[13] = (SECTION_VA + delay_at, 32 * (len(delay) + 1))

    if pe32:
        opt = struct.pack("<HBBIIIIIIII", 0x10B, 14, 0, 0, 0, 0, 0, 0, 0, image_base, 0x1000)
    else:
        opt = struct.pack("<HBBIIIIIQI", 0x20B, 14, 0, 0, 0, 0, 0, 0, image_base, 0x1000)
    opt += struct.pack("<IHHHHHHIII", 0x200, 6, 0, 0, 0, 6, 0, 0, 0x3000, SECTION_RAW)
    opt += b"\0" * ((92 if pe32 else 108) - len(opt))
    opt += struct.pack("<I", 16)
    opt += b"".join(struct.pack("<II", *d) for d in dirs)
    sections = [struct.pack("<8sIIIIIIHHI", b".rdata", len(body), SECTION_VA, len(body),
                            SECTION_RAW, 0, 0, 0, 0, 0x40000040)]
    if rsrc:
        sections.append(struct.pack("<8sIIIIIIHHI", b".rsrc", len(rsrc), rsrc_va, len(rsrc),
                                    SECTION_RAW + len(body), 0, 0, 0, 0, 0x40000040))
    coff = struct.pack("<HHIIIHH", machine, len(sections), 0, 0, 0, len(opt), 0x22)
    head = bytearray(b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x40))
    head += b"PE\0\0" + coff + opt + b"".join(sections)
    head += b"\0" * (SECTION_RAW - len(head))
    return bytes(head + body + rsrc)


# ---------------------------------------------------------------------------
# A Mach-O, built
# ---------------------------------------------------------------------------

X86_64, ARM64 = 0x01000007, 0x0100000C


def packed(version):
    major, minor, patch = (list(map(int, version.split("."))) + [0, 0])[:3]
    return (major << 16) | (minor << 8) | patch


def build_version(minos, platform=1):
    return (bc.LC_BUILD_VERSION, struct.pack("<IIII", platform, packed(minos),
                                              packed("26.5"), 0))


def version_min(minos):
    return (bc.LC_VERSION_MIN_MACOSX, struct.pack("<II", packed(minos), packed("26.5")))


def thin(cpu, commands, wide=True):
    """One Mach-O: a header and its load commands, a segment first so the
    version command is not the first thing the reader meets."""
    commands = [(0x19, b"__PAGEZERO".ljust(16, b"\0") + b"\0" * 48)] + list(commands)
    body = b"".join(struct.pack("<II", cmd, 8 + len(p)) + p for cmd, p in commands)
    if wide:
        head = struct.pack("<IIIIIIII", 0xFEEDFACF, cpu, 0, 2, len(commands), len(body), 0, 0)
    else:
        head = struct.pack("<IIIIIII", 0xFEEDFACE, cpu, 0, 2, len(commands), len(body), 0)
    return head + body


def fat(*slices, wide=False):
    """A universal binary: the fat header, big-endian, then each slice at a
    4 KB boundary, which is where lipo puts them."""
    entry = 32 if wide else 20
    out = bytearray(struct.pack(">II", 0xCAFEBABF if wide else 0xCAFEBABE, len(slices)))
    out.extend(b"\0" * (entry * len(slices)))
    for i, (cpu, data) in enumerate(slices):
        while len(out) % 4096:
            out.append(0)
        if wide:
            struct.pack_into(">iiQQII", out, 8 + entry * i, cpu, 0, len(out), len(data), 12, 0)
        else:
            struct.pack_into(">iiIII", out, 8 + entry * i, cpu, 0, len(out), len(data), 12)
        out.extend(data)
    return bytes(out)


# ---------------------------------------------------------------------------

class Files:
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def file(self, data, name="game"):
        path = Path(self.tmp.name) / name
        path.write_bytes(data)
        return str(path)

    def run_main(self, argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = bc.main(argv)
        return code, out.getvalue()


class WindowsTest(Files, unittest.TestCase):

    def windows(self, *exes, icon=False):
        return self.run_main(["windows", *exes] + (["--require-icon"] if icon else []))

    def test_the_mingw_build_ci_shipped_is_refused_and_says_why(self):
        """Seen red: the import table of run 37229289825's ray_test.exe."""
        code, out = self.windows(self.file(pe(MINGW_DYNAMIC), "ray_test.exe"))
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL  libstdc++-6.dll: MinGW's C++ runtime: link it with -static", out)
        self.assertIn("FAIL  libgcc_s_seh-1.dll", out)
        self.assertEqual(out.count("FAIL  "), 2, out)

    def test_the_same_build_linked_statically_passes(self):
        code, out = self.windows(self.file(pe(MINGW_STATIC)))
        self.assertEqual(code, 0, out)
        self.assertIn("ok    KERNEL32.dll", out)
        self.assertIn("ok    api-ms-win-crt-heap-l1-1-0.dll", out)
        self.assertIn("PASS: 1 executable(s)", out)

    def test_msvc_with_the_dynamic_runtime_is_refused(self):
        """Seen red: run 37231088726's arm64 .exe, built /MD because the
        static-CRT setting sat before project(), where MSVC is not yet set."""
        code, out = self.windows(self.file(pe(MSVC_MD, machine=0xAA64)))
        self.assertEqual(code, 1, out)
        self.assertIn("(arm64,", out)
        self.assertIn("FAIL  VCRUNTIME140.dll", out)
        self.assertIn("FAIL  MSVCP140.dll", out)

    def test_an_unknown_dll_fails_even_when_it_is_named_nowhere(self):
        code, out = self.windows(self.file(pe(["KERNEL32.dll", "SDL2.dll"])))
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL  SDL2.dll: not a part of Windows; the zip does not carry it", out)

    def test_a_delay_loaded_dll_is_still_a_dll_the_game_needs(self):
        # The old format, which stored VAs, is VC6's: 32-bit only.
        for old in (False, True):
            with self.subTest(old_format=old):
                code, out = self.windows(self.file(pe(["KERNEL32.dll"],
                                                      delay=["libwinpthread-1.dll"],
                                                      old_delay=old, pe32=old,
                                                      machine=0x14C if old else 0x8664)))
                self.assertEqual(code, 1, out)
                self.assertIn("FAIL  libwinpthread-1.dll (delay-loaded)", out)
        code, out = self.windows(self.file(pe(["KERNEL32.dll"], delay=["user32.dll"])))
        self.assertEqual(code, 0, out)
        self.assertIn("ok    user32.dll (delay-loaded)", out)

    def test_names_compare_without_case(self):
        code, out = self.windows(self.file(pe(["kernel32.DLL", "API-MS-WIN-CRT-HEAP-L1-1-0.DLL",
                                                 "LIBSTDC++-6.DLL"])))
        self.assertEqual(code, 1, out)
        self.assertIn("ok    kernel32.DLL", out)
        self.assertIn("ok    API-MS-WIN-CRT-HEAP-L1-1-0.DLL", out)
        self.assertIn("FAIL  LIBSTDC++-6.DLL: MinGW's C++ runtime", out)

    def test_an_executable_that_imports_nothing_is_a_misread_not_a_pass(self):
        code, out = self.windows(self.file(pe([])))
        self.assertEqual(code, 1, out)
        self.assertIn("no imports at all", out)

    def test_a_32_bit_executable_is_read_too(self):
        code, out = self.windows(self.file(pe(["KERNEL32.dll", "libgcc_s_dw2-1.dll"],
                                              machine=0x14C, pe32=True)))
        self.assertEqual(code, 1, out)
        self.assertIn("(x86, 2 DLLs)", out)
        self.assertIn("FAIL  libgcc_s_dw2-1.dll", out)

    def test_every_executable_is_judged(self):
        good, bad = self.file(pe(MINGW_STATIC), "a.exe"), self.file(pe(MINGW_DYNAMIC), "b.exe")
        code, out = self.windows(good, bad)
        self.assertEqual(code, 1, out)
        self.assertIn("a.exe", out)
        self.assertIn("b.exe", out)

    def test_the_icon(self):
        sizes = [(16, 1), (32, 2), (48, 3), (256, 4)]
        cases = [
            # (groups, icon ids, passes --require-icon, says)
            ((("IDI_ICON1", sizes),), (1, 2, 3, 4), True, "ok    icon: 16 32 48 256"),
            (((1, sizes),), (1, 2, 3, 4), True, "ok    icon: 16 32 48 256"),
            ((), (), False, "FAIL  no icon group"),
            # A group whose icons are not in the file shows nothing.
            ((("IDI_ICON1", sizes),), (), False, "FAIL  no icon group"),
            ((("IDI_ICON1", sizes),), (9,), False, "FAIL  no icon group"),
            ((("IDI_ICON1", sizes),), (1, 2), True, "ok    icon: 16 32"),
        ]
        for groups, ids, passes, says in cases:
            with self.subTest(groups=groups, ids=ids):
                exe = self.file(pe(MINGW_STATIC, groups=groups, icon_ids=ids))
                code, out = self.windows(exe, icon=True)
                self.assertEqual(code, 0 if passes else 1, out)
                self.assertIn(says, out)
        code, out = self.windows(self.file(pe(MINGW_STATIC)))
        self.assertEqual(code, 0, "without --require-icon a missing icon is reported, "
                                  "not failed: " + out)
        self.assertIn("--    no icon", out)

    def test_what_is_not_a_windows_executable_is_said_and_exits_2(self):
        good = pe(MINGW_STATIC)
        cases = {
            "an ELF": b"\x7fELF" + b"\0" * 200,
            "empty": b"",
            "MZ and nothing else": b"MZ",
            "no PE signature": b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x40) + b"XX\0\0",
            "cut after the headers": good[:SECTION_RAW + 4],
        }
        for what, data in cases.items():
            with self.subTest(what=what):
                code, out = self.windows(self.file(data))
                self.assertEqual(code, 2, out)
                self.assertTrue(out.startswith("FAIL: "), out)
        code, out = self.windows(str(Path(self.tmp.name) / "missing.exe"))
        self.assertEqual(code, 2, out)

    def test_the_lists_do_not_contradict_each_other(self):
        for dll in bc.WHY_NOT:
            with self.subTest(dll=dll):
                self.assertIsNone(bc.why_allowed(dll))
        for dll, why in bc.ALLOWED.items():
            with self.subTest(dll=dll):
                self.assertEqual(dll, dll.lower())
                self.assertTrue(dll.endswith(".dll") and why)


class MacosTest(Files, unittest.TestCase):

    def macos(self, data, min_os):
        return self.run_main(["macos", self.file(data), "--min-os", min_os])

    def test_the_universal_binary_ci_shipped_is_refused(self):
        """Seen red: run 37231088726's macOS binary, minos 26.0 in both
        slices, against the target the build now declares."""
        shipped = fat((X86_64, thin(X86_64, [build_version("26.0")])),
                      (ARM64, thin(ARM64, [build_version("26.0")])))
        code, out = self.macos(shipped, "10.15")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL  x86_64: needs macOS 26.0 (LC_BUILD_VERSION); the target is 10.15", out)
        self.assertIn("FAIL  arm64: needs macOS 26.0 (LC_BUILD_VERSION); the target is 11.0", out)

    def test_a_build_with_the_target_set_passes(self):
        """What the toolchain makes of 10.15: x86_64 at 10.15, arm64 raised
        to 11.0, the first macOS on Apple silicon."""
        built = fat((X86_64, thin(X86_64, [build_version("10.15")])),
                    (ARM64, thin(ARM64, [build_version("11.0")])))
        code, out = self.macos(built, "10.15")
        self.assertEqual(code, 0, out)
        self.assertIn("ok    x86_64: macOS 10.15", out)
        self.assertIn("ok    arm64: macOS 11.0", out)

    def test_the_arm64_floor_is_eleven_and_no_higher(self):
        built = fat((X86_64, thin(X86_64, [build_version("10.15")])),
                    (ARM64, thin(ARM64, [build_version("11.1")])))
        code, out = self.macos(built, "10.15")
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL  arm64: needs macOS 11.1", out)
        code, out = self.macos(fat((X86_64, thin(X86_64, [build_version("11.0")]))), "10.15")
        self.assertEqual(code, 1, "x86_64 has no floor of 11: " + out)

    def test_a_slice_lower_than_the_target_is_fine(self):
        code, out = self.macos(thin(X86_64, [build_version("10.13")]), "10.15")
        self.assertEqual(code, 0, out)

    def test_a_patch_level_counts(self):
        code, out = self.macos(thin(X86_64, [build_version("10.15.1")]), "10.15")
        self.assertEqual(code, 1, out)
        self.assertIn("needs macOS 10.15.1", out)
        code, out = self.macos(thin(X86_64, [build_version("10.15.1")]), "10.15.1")
        self.assertEqual(code, 0, out)

    def test_every_shape_of_binary(self):
        shapes = {
            "thin 64-bit": thin(ARM64, [build_version("11.0")]),
            "thin 32-bit": thin(7, [version_min("10.13")], wide=False),
            "fat with 64-bit offsets": fat((X86_64, thin(X86_64, [build_version("10.15")])),
                                           (ARM64, thin(ARM64, [build_version("11.0")])),
                                           wide=True),
            "the older version command": fat((X86_64, thin(X86_64, [version_min("10.15")]))),
        }
        for what, data in shapes.items():
            with self.subTest(what=what):
                code, out = self.macos(data, "10.15")
                self.assertEqual(code, 0, out)

    def test_a_slice_that_says_nothing_or_says_another_platform_fails(self):
        cases = {
            "no version command": (thin(X86_64, []), "no LC_BUILD_VERSION"),
            "built for iOS": (thin(ARM64, [build_version("11.0", platform=2)]),
                              "for platform 2, not macOS"),
        }
        for what, (data, says) in cases.items():
            with self.subTest(what=what):
                code, out = self.macos(data, "10.15")
                self.assertEqual(code, 1, out)
                self.assertIn(says, out)

    def test_an_unreadable_target_or_binary_exits_2(self):
        ok = thin(X86_64, [build_version("10.15")])
        for min_os in ("", "abc", "10.x", "10.15.1.2", " "):
            with self.subTest(min_os=min_os):
                code, out = self.macos(ok, min_os)
                self.assertEqual(code, 2, out)
        cases = {
            "an ELF": b"\x7fELF" + b"\0" * 60,
            "empty": b"",
            "a fat header cut short": struct.pack(">II", 0xCAFEBABE, 2) + b"\0" * 10,
            "a fat header with no slices": struct.pack(">II", 0xCAFEBABE, 0),
            "a slice cut short": ok[:40],
        }
        for what, data in cases.items():
            with self.subTest(what=what):
                code, out = self.macos(data, "10.15")
                self.assertEqual(code, 2, out)
                self.assertTrue(out.startswith("FAIL: "), out)



# ---------------------------------------------------------------------------
# An ELF, built
# ---------------------------------------------------------------------------

def elf(etype=bc.ET_DYN, interp=True, relro=True, stack_flags=6, dynamic=None,
        wide=True, big=False):
    """An ELF executable's header, program headers and dynamic section --
    what binary_check.py elf reads, and nothing it does not. stack_flags is
    PT_GNU_STACK's p_flags (6 = RW, 7 = RWX), None for no such segment;
    dynamic is {tag: value}, BIND_NOW and nothing else by default."""
    o = ">" if big else "<"
    dynamic = {bc.DT_FLAGS: bc.DF_BIND_NOW} if dynamic is None else dynamic
    entries = list(dynamic.items()) + [(bc.DT_NULL, 0)]
    dyn = b"".join(struct.pack(o + ("qQ" if wide else "iI"), tag, value)
                   for tag, value in entries)
    segments = []   # (type, flags, offset, size)
    if interp:
        segments.append((bc.PT_INTERP, 4, 0, 28))
    segments.append((bc.PT_DYNAMIC, 6, 0, len(dyn)))   # offset patched below
    if relro:
        segments.append((bc.PT_GNU_RELRO, 4, 0, 0x100))
    if stack_flags is not None:
        segments.append((bc.PT_GNU_STACK, stack_flags, 0, 0))
    ehsize, phentsize = (64, 56) if wide else (52, 32)
    dyn_at = ehsize + phentsize * len(segments)
    ident = b"\x7fELF" + bytes([2 if wide else 1, 2 if big else 1, 1]) + b"\0" * 9
    if wide:
        header = ident + struct.pack(o + "HHIQQQIHHHHHH", etype, 0x3E, 1, 0, ehsize, 0, 0,
                                     ehsize, phentsize, len(segments), 64, 0, 0)
    else:
        header = ident + struct.pack(o + "HHIIIIIHHHHHH", etype, 3, 1, 0, ehsize, 0, 0,
                                     ehsize, phentsize, len(segments), 40, 0, 0)
    table = b""
    for kind, flags, offset, size in segments:
        if kind == bc.PT_DYNAMIC:
            offset = dyn_at
        if wide:
            table += struct.pack(o + "IIQQQQQQ", kind, flags, offset, 0, 0, size, size, 8)
        else:
            table += struct.pack(o + "IIIIIIII", kind, offset, 0, 0, size, size, flags, 4)
    return header + table + dyn


# What linux_build.sh's zig wrote for linux-x64-glibc before the release
# hardening: RELRO, BIND_NOW and a RW stack by lld's default, and ET_EXEC.
ZIG_RELEASE = dict(etype=bc.ET_EXEC, dynamic={bc.DT_FLAGS: bc.DF_BIND_NOW,
                                              bc.DT_FLAGS_1: bc.DF_1_NOW})


class ElfTest(Files, unittest.TestCase):

    def check(self, *datas, pie=True):
        return self.run_main(["elf", *(["--pie"] if pie else []),
                              *(self.file(d, f"game{i}") for i, d in enumerate(datas))])

    def test_the_zig_release_is_not_pie_and_only_pie_asks_for_it(self):
        """The linux-x64-glibc binary tools/linux_build.sh makes: ET_EXEC, and
        RELRO, BIND_NOW and a RW stack by lld's own default. -pie costs 5.5%
        there and is off, so without --pie that is said and passes."""
        code, out = self.check(elf(**ZIG_RELEASE))
        self.assertEqual(code, 1, out)
        self.assertIn("FAIL  ET_EXEC, not position-independent", out)
        self.assertEqual(out.count("FAIL  "), 1, out)
        self.assertIn("ok    RELRO", out)
        self.assertIn("ok    BIND_NOW", out)
        code, out = self.check(elf(**ZIG_RELEASE), pie=False)
        self.assertEqual(code, 0, out)
        self.assertIn("--    ET_EXEC, not position-independent", out)
        self.assertIn("(not required: no --pie)", out)
        self.assertIn("PASS: RELRO, BIND_NOW and a non-executable stack", out)

    def test_a_hardened_executable_passes(self):
        code, out = self.check(elf())
        self.assertEqual(code, 0, out)
        self.assertIn("PASS: PIE, RELRO, BIND_NOW and a non-executable stack", out)
        code, out = self.check(elf(), pie=False)
        self.assertEqual(code, 0, out)
        self.assertIn("ok    position-independent", out)

    def test_without_pie_the_rest_is_still_required(self):
        for data, says in ((elf(etype=bc.ET_EXEC, relro=False), "no PT_GNU_RELRO segment"),
                           (elf(etype=bc.ET_EXEC, dynamic={}), "no BIND_NOW"),
                           (elf(etype=bc.ET_EXEC, stack_flags=7), "an EXECUTABLE stack")):
            with self.subTest(says=says):
                code, out = self.check(data, pie=False)
                self.assertEqual(code, 1, out)
                self.assertIn("FAIL  " + says, out)

    def test_each_missing_property_is_named(self):
        for what, data, says in (
                ("no relro", elf(relro=False), "no PT_GNU_RELRO segment"),
                ("lazy binding", elf(dynamic={}), "no BIND_NOW"),
                ("an executable stack", elf(stack_flags=7), "an EXECUTABLE stack"),
                ("no stack segment", elf(stack_flags=None), "no PT_GNU_STACK segment"),
                ("a shared library", elf(interp=False), "a shared library"),
        ):
            with self.subTest(what=what):
                code, out = self.check(data)
                self.assertEqual(code, 1, out)
                self.assertIn(says, out)
                self.assertEqual(out.count("FAIL  "), 1, out)

    def test_every_spelling_of_bind_now(self):
        for dynamic in ({bc.DT_BIND_NOW: 0}, {bc.DT_FLAGS: bc.DF_BIND_NOW},
                        {bc.DT_FLAGS_1: bc.DF_1_NOW}):
            with self.subTest(dynamic=dynamic):
                code, out = self.check(elf(dynamic=dynamic))
                self.assertEqual(code, 0, out)

    def test_a_static_pie_is_pie(self):
        code, out = self.check(elf(interp=False, dynamic={bc.DT_FLAGS_1: bc.DF_1_PIE | bc.DF_1_NOW}))
        self.assertEqual(code, 0, out)

    def test_32_bit_and_big_endian(self):
        for wide, big in ((False, False), (True, True), (False, True)):
            with self.subTest(wide=wide, big=big):
                code, out = self.check(elf(wide=wide, big=big))
                self.assertEqual(code, 0, out)
                code, out = self.check(elf(wide=wide, big=big, **ZIG_RELEASE))
                self.assertEqual(code, 1, out)

    def test_the_worst_of_several_decides(self):
        code, out = self.check(elf(), elf(**ZIG_RELEASE))
        self.assertEqual(code, 1, out)
        self.assertEqual(out.count("== "), 2)

    def test_what_is_not_an_elf_program_exits_2(self):
        whole = elf()
        cases = {"a PE": b"MZ" + b"\0" * 100, "empty": b"", "cut short": whole[:30],
                 # e_phnum, at 56 in a 64-bit header, set to 0
                 "an object file": whole[:56] + struct.pack("<H", 0) + whole[58:],
                 "program headers cut short": whole[:80]}
        for what, data in cases.items():
            with self.subTest(what=what):
                code, out = self.check(data)
                self.assertEqual(code, 2, out)
                self.assertTrue(out.startswith("FAIL: "), out)

if __name__ == "__main__":
    unittest.main()
