#!/usr/bin/env python3
"""What a shipped binary asks of the player's machine, read out of the binary.

A boot test on a CI runner proves the game starts on THAT machine, and a runner
is a developer's machine with everything installed. The Windows x64 .exe
imported libstdc++-6.dll and libgcc_s_seh-1.dll for as long as it was built
with the runner's MinGW, and its boot test passed every time because MinGW's
bin/ was on the runner's PATH; a player double-clicking it got "The code
execution cannot proceed". The macOS universal binary asked for macOS 26 in
both slices, because nothing set a deployment target, and the job that built
it ran on macOS 26. Neither is visible from where the binary is built. Both are
written in the file.

    binary_check.py windows EXE... [--require-icon]
        Every DLL the .exe imports, eagerly or delay-loaded, must be on ALLOWED:
        a part of every Windows 10 and later. Anything else is a file the zip
        would have to carry and does not. --require-icon also wants an icon
        group in the resources, which is what Explorer and the taskbar show.

    binary_check.py macos BINARY --min-os X.Y
        Every slice's minimum macOS (LC_BUILD_VERSION, or the older
        LC_VERSION_MIN_MACOSX) must be at most X.Y -- or 11.0 for arm64, the
        first macOS that ran on Apple silicon, which the toolchain raises any
        lower target to.

    binary_check.py elf BINARY... [--pie]
        The hardening a Linux or BSD release is linked with, read out of the
        program headers and the dynamic section: RELRO and BIND_NOW (the
        relocations are read-only once it starts) and a stack that is not
        executable. With --pie it must also be position-independent, so ASLR
        can move it; without, that is said and not required. CI passes --pie
        whenever the release was configured with RMP_RELEASE_PIE, which is on
        by default (+5.5% on linux-x64-glibc, Omar's decision). A flag a
        toolchain silently ignores is a flag that was never there; this reads
        what the linker wrote.

Standard library only, so it runs on any runner and on a laptop, with no
objdump, otool or lipo of the right flavour to find first.
Exit: 0 = the binary is fine, 1 = it is not, 2 = not a binary this reads, or
a usage error.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Windows: the DLLs every Windows 10 and later has
# ---------------------------------------------------------------------------
#
# Lower case, compared case-insensitively: the import table spells them however
# the import library did ("KERNEL32.dll", "api-ms-win-crt-heap-l1-1-0.dll").
# Each entry is something raylib, its backends, a C or C++ runtime linked
# statically, or the framework can put in the import table -- and that ships
# with Windows itself rather than with a redistributable.
ALLOWED = {
    "kernel32.dll": "the Win32 base: every process imports it",
    "user32.dll": "windows, messages and input: GLFW, RGFW and raylib's Win32 backend",
    "gdi32.dll": "pixel formats and SwapBuffers for the WGL context: GLFW, RGFW, Win32",
    "shell32.dll": "dropped files (DragQueryFileW): GLFW and RGFW",
    "winmm.dll": "timeBeginPeriod for frame timing: raylib links it on every backend",
    "opengl32.dll": "the system's OpenGL: linked by the Win32 and RGFW backends "
                    "(GLFW loads it at run time instead)",
    "advapi32.dll": "registry and security calls a statically linked runtime can make",
    "ole32.dll": "COM, which a game's own code (or a future audio backend) can link",
    "ucrtbase.dll": "the Universal C Runtime, part of Windows since 10",
    "msvcrt.dll": "the C runtime of the older MinGW toolchains, a part of Windows "
                  "since XP and not a redistributable",
}

# API sets: names the loader resolves to a real system DLL. api-ms-win-crt-* is
# the Universal C Runtime (MinGW's UCRT toolchain and MSVC's /MD both import
# it), part of Windows since 10; api-ms-win-core-* have resolved since 8.
ALLOWED_PREFIXES = {
    "api-ms-win-crt-": "the Universal C Runtime, part of Windows since 10",
    "api-ms-win-core-": "a core API set, resolved by the loader since Windows 8",
}

# The ones worth naming when they appear, because they are what a build that
# links its runtime dynamically imports. Not an exhaustive deny list: anything
# not allowed above fails, named here or not.
WHY_NOT = {
    "libstdc++-6.dll": "MinGW's C++ runtime: link it with -static",
    "libgcc_s_seh-1.dll": "MinGW's GCC runtime: link it with -static",
    "libgcc_s_dw2-1.dll": "MinGW's GCC runtime (32-bit): link it with -static",
    "libwinpthread-1.dll": "MinGW's threads: link it with -static",
    "libc++.dll": "llvm-mingw's C++ runtime: link it with -static",
    "libunwind.dll": "llvm-mingw's unwinder: link it with -static",
    "vcruntime140.dll": "the Visual C++ Redistributable: build with /MT "
                        "(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)",
    "vcruntime140_1.dll": "the Visual C++ Redistributable: build with /MT",
    "msvcp140.dll": "the Visual C++ Redistributable: build with /MT",
    "xinput1_3.dll": "the DirectX SDK redistributable, not Windows: xinput1_4 is",
}

MACHINES = {0x14C: "x86", 0x8664: "x64", 0xAA64: "arm64", 0x1C4: "arm"}

RT_ICON = 3
RT_GROUP_ICON = 14


class NotReadable(Exception):
    """The file is not the kind of binary asked for, or it is truncated."""


def u16(data: bytes, at: int) -> int:
    if at < 0 or at + 2 > len(data):
        raise NotReadable(f"truncated at offset {at:#x}")
    return struct.unpack_from("<H", data, at)[0]


def u32(data: bytes, at: int) -> int:
    if at < 0 or at + 4 > len(data):
        raise NotReadable(f"truncated at offset {at:#x}")
    return struct.unpack_from("<I", data, at)[0]


def cstring(data: bytes, at: int) -> str:
    end = data.find(b"\0", at)
    if at < 0 or at >= len(data) or end < 0:
        raise NotReadable(f"unterminated string at offset {at:#x}")
    return data[at:end].decode("ascii", "replace")


class Pe:
    """The parts of a PE image this check reads: the import tables and the
    resource directory, reached through the section table."""

    def __init__(self, data: bytes):
        if data[:2] != b"MZ":
            raise NotReadable("no MZ header: not a Windows executable")
        pe = u32(data, 0x3C)
        if data[pe:pe + 4] != b"PE\0\0":
            raise NotReadable("no PE signature")
        self.data = data
        self.machine = u16(data, pe + 4)
        sections = u16(data, pe + 6)
        optional_size = u16(data, pe + 20)
        opt = pe + 24
        magic = u16(data, opt)
        if magic == 0x10B:      # PE32
            self.image_base = u32(data, opt + 28)
            count_at, dirs = opt + 92, opt + 96
        elif magic == 0x20B:    # PE32+
            self.image_base = struct.unpack_from("<Q", data, opt + 24)[0]
            count_at, dirs = opt + 108, opt + 112
        else:
            raise NotReadable(f"unknown optional header magic {magic:#x}")
        self.size_of_headers = u32(data, opt + 60)
        count = min(u32(data, count_at), 16)
        self.directories = [(u32(data, dirs + 8 * i), u32(data, dirs + 8 * i + 4))
                            for i in range(count)]
        self.sections = []
        table = opt + optional_size
        for i in range(sections):
            s = table + 40 * i
            self.sections.append((u32(data, s + 12), max(u32(data, s + 8), u32(data, s + 16)),
                                  u32(data, s + 20), data[s:s + 8].rstrip(b"\0").decode(
                                      "ascii", "replace")))

    def offset(self, rva: int) -> int:
        """File offset of a relative virtual address."""
        if rva < self.size_of_headers:
            return rva
        for va, size, raw, _name in self.sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise NotReadable(f"RVA {rva:#x} is in no section")

    def directory(self, index: int) -> tuple[int, int]:
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def imports(self) -> list[tuple[str, str]]:
        """(dll, how) for every import: "import" or "delay"."""
        out = []
        rva, size = self.directory(1)
        if rva and size:
            at = self.offset(rva)
            while True:
                name = u32(self.data, at + 12)
                if not name and not u32(self.data, at + 16):
                    break
                out.append((cstring(self.data, self.offset(name)), "import"))
                at += 20
        rva, size = self.directory(13)
        if rva and size:
            at = self.offset(rva)
            while True:
                attributes, name = u32(self.data, at), u32(self.data, at + 4)
                if not name:
                    break
                # Attribute bit 0 clear is the old format, which stored VAs.
                if not attributes & 1:
                    name -= self.image_base
                out.append((cstring(self.data, self.offset(name)), "delay"))
                at += 32
        return out

    def _entries(self, base: int, directory: int) -> list[tuple[int | str, int]]:
        """(id or name, offset) for each entry of one resource directory.

        Named entries count: `IDI_ICON1 ICON "app_icon.ico"`, the line the
        generator writes, names the group with a STRING, because IDI_ICON1 is
        defined nowhere. Reading numeric ids only, the icon was not there."""
        named, ids = u16(self.data, directory + 12), u16(self.data, directory + 14)
        out: list[tuple[int | str, int]] = []
        for i in range(named + ids):
            e = directory + 16 + 8 * i
            key: int | str = u32(self.data, e)
            if key & 0x80000000:
                at = base + (key & 0x7FFFFFFF)
                length = u16(self.data, at)
                key = self.data[at + 2:at + 2 + 2 * length].decode("utf-16-le", "replace")
            out.append((key, u32(self.data, e + 4)))
        return out

    def _leaves(self, base: int, offset: int) -> list[tuple[int | str, bytes]]:
        """(id or name, data) under one type: name level, then language level."""
        out = []
        for name_id, to_lang in self._entries(base, base + (offset & 0x7FFFFFFF)):
            if not to_lang & 0x80000000:
                continue
            for _lang, to_data in self._entries(base, base + (to_lang & 0x7FFFFFFF)):
                if to_data & 0x80000000:
                    continue
                entry = base + to_data
                start = self.offset(u32(self.data, entry))
                out.append((name_id, self.data[start:start + u32(self.data, entry + 4)]))
        return out

    def icon_groups(self) -> list[list[int]]:
        """The sizes in each icon group whose icons are really there (256 is
        stored as 0)."""
        rva, size = self.directory(2)
        if not rva or not size:
            return []
        base = self.offset(rva)
        types = dict(self._entries(base, base))
        if RT_GROUP_ICON not in types or RT_ICON not in types:
            return []
        icons = {name for name, _ in self._leaves(base, types[RT_ICON])}
        groups = []
        for _name, blob in self._leaves(base, types[RT_GROUP_ICON]):
            if len(blob) < 6 or u16(blob, 2) != 1:
                continue
            sizes = []
            for i in range(u16(blob, 4)):
                e = 6 + 14 * i
                if e + 14 <= len(blob) and u16(blob, e + 12) in icons:
                    sizes.append(blob[e] or 256)
            if sizes:
                groups.append(sorted(sizes))
        return groups


def why_allowed(dll: str) -> str | None:
    low = dll.lower()
    if low in ALLOWED:
        return ALLOWED[low]
    for prefix, why in ALLOWED_PREFIXES.items():
        if low.startswith(prefix):
            return why
    return None


def check_windows(paths: list[str], require_icon: bool) -> int:
    failed = 0
    for path in paths:
        try:
            pe = Pe(Path(path).read_bytes())
            imports = pe.imports()
            groups = pe.icon_groups()
        except (OSError, NotReadable) as e:
            print(f"FAIL: {path}: {e}")
            return 2
        arch = MACHINES.get(pe.machine, f"machine {pe.machine:#x}")
        print(f"== {path} ({arch}, {len(imports)} DLLs)")
        if not imports:
            # Every Windows executable imports KERNEL32 at least. None read
            # means this script misread the file, and that must not be a pass.
            print("  FAIL  no imports at all: this is not how a Windows executable looks")
            failed += 1
        for dll, how in imports:
            why = why_allowed(dll)
            tag = "" if how == "import" else " (delay-loaded)"
            if why:
                print(f"  ok    {dll}{tag}: {why}")
            else:
                hint = WHY_NOT.get(dll.lower(), "not a part of Windows; the zip does not carry it")
                print(f"  FAIL  {dll}{tag}: {hint}")
                failed += 1
        if groups:
            for sizes in groups:
                print(f"  ok    icon: {' '.join(str(s) for s in sizes)}")
        elif require_icon:
            print("  FAIL  no icon group in the resources: Explorer shows the default icon")
            failed += 1
        else:
            print("  --    no icon")
    if failed:
        print(f"FAIL: {failed} problem(s). A player's machine has Windows and nothing else.")
        return 1
    print(f"PASS: {len(paths)} executable(s) need nothing Windows 10 does not have")
    return 0


# ---------------------------------------------------------------------------
# macOS: the minimum version of every slice
# ---------------------------------------------------------------------------

CPU_TYPES = {7: "i386", 0x01000007: "x86_64", 12: "arm", 0x0100000C: "arm64"}

# The toolchain raises a lower deployment target to this for these slices:
# macOS 11 is the first that ran on Apple silicon.
ARCH_FLOOR = {"arm64": (11, 0)}

LC_VERSION_MIN_MACOSX = 0x24
LC_BUILD_VERSION = 0x32
PLATFORM_MACOS = 1


def version(packed: int) -> tuple[int, int, int]:
    return (packed >> 16, (packed >> 8) & 0xFF, packed & 0xFF)


def show(v) -> str:
    return ".".join(str(n) for n in (v if len(v) < 3 or v[2] else v[:2]))


def thin_minos(data: bytes, at: int) -> tuple[str, tuple[int, int, int] | None, str]:
    """(arch, minimum macOS or None, how it was found) of one thin Mach-O."""
    magic = u32(data, at)
    if magic == 0xFEEDFACF:
        header = 32
    elif magic == 0xFEEDFACE:
        header = 28
    else:
        raise NotReadable(f"no Mach-O header at offset {at:#x}")
    cpu = u32(data, at + 4)
    arch = CPU_TYPES.get(cpu, f"cputype {cpu:#x}")
    ncmds = u32(data, at + 16)
    cmd_at = at + header
    for _ in range(ncmds):
        cmd, size = u32(data, cmd_at), u32(data, cmd_at + 4)
        if size < 8:
            raise NotReadable(f"load command of size {size} at offset {cmd_at:#x}")
        if cmd == LC_BUILD_VERSION:
            platform = u32(data, cmd_at + 8)
            if platform != PLATFORM_MACOS:
                return arch, None, f"LC_BUILD_VERSION for platform {platform}, not macOS"
            return arch, version(u32(data, cmd_at + 12)), "LC_BUILD_VERSION"
        if cmd == LC_VERSION_MIN_MACOSX:
            return arch, version(u32(data, cmd_at + 8)), "LC_VERSION_MIN_MACOSX"
        cmd_at += size
    return arch, None, "no LC_BUILD_VERSION or LC_VERSION_MIN_MACOSX"


def slices(data: bytes) -> list[tuple[str, tuple[int, int, int] | None, str]]:
    if len(data) < 8:
        raise NotReadable("too short to be a Mach-O")
    fat = struct.unpack_from(">I", data, 0)[0]
    if fat in (0xCAFEBABE, 0xCAFEBABF):
        count = struct.unpack_from(">I", data, 4)[0]
        wide = fat == 0xCAFEBABF
        out = []
        for i in range(count):
            if wide:
                entry = 8 + 32 * i
                if entry + 32 > len(data):
                    raise NotReadable("fat header truncated")
                offset = struct.unpack_from(">Q", data, entry + 8)[0]
            else:
                entry = 8 + 20 * i
                if entry + 20 > len(data):
                    raise NotReadable("fat header truncated")
                offset = struct.unpack_from(">I", data, entry + 8)[0]
            out.append(thin_minos(data, offset))
        if not out:
            raise NotReadable("a fat header with no slices")
        return out
    return [thin_minos(data, 0)]


def parse_version(text: str) -> tuple[int, int, int]:
    parts = text.strip().split(".")
    if not 1 <= len(parts) <= 3 or not all(p.isdigit() for p in parts):
        raise ValueError(text)
    nums = [int(p) for p in parts] + [0] * (3 - len(parts))
    return (nums[0], nums[1], nums[2])


def check_macos(path: str, min_os: str) -> int:
    try:
        declared = parse_version(min_os)
    except ValueError:
        print(f"FAIL: --min-os {min_os!r} is not a version like 10.15 -- "
              "the deployment target was not found where it should be")
        return 2
    try:
        found = slices(Path(path).read_bytes())
    except (OSError, NotReadable) as e:
        print(f"FAIL: {path}: {e}")
        return 2
    print(f"== {path} ({len(found)} slice(s)), declared deployment target {show(declared)}")
    failed = 0
    for arch, minos, how in found:
        floor = ARCH_FLOOR.get(arch, (0, 0))
        allowed = max(declared, (floor[0], floor[1], 0))
        if minos is None:
            print(f"  FAIL  {arch}: {how}, so nothing says which macOS it needs")
            failed += 1
        elif minos > allowed:
            print(f"  FAIL  {arch}: needs macOS {show(minos)} ({how}); "
                  f"the target is {show(allowed)}")
            failed += 1
        else:
            print(f"  ok    {arch}: macOS {show(minos)} ({how})")
    if failed:
        print("FAIL: a Mac older than that refuses to open the game, with no message "
              "that says why. Set CMAKE_OSX_DEPLOYMENT_TARGET before project().")
        return 1
    print("PASS: every slice runs on the macOS it declares")
    return 0


# ---------------------------------------------------------------------------
# ELF: what the release was hardened with
# ---------------------------------------------------------------------------

ET_EXEC, ET_DYN = 2, 3
PT_DYNAMIC, PT_INTERP = 2, 3
PT_GNU_STACK, PT_GNU_RELRO = 0x6474E551, 0x6474E552
PF_X = 1
DT_NULL, DT_BIND_NOW, DT_FLAGS, DT_FLAGS_1 = 0, 24, 30, 0x6FFFFFFB
DF_BIND_NOW = 0x8
DF_1_NOW, DF_1_PIE = 0x1, 0x08000000


class Elf:
    """The header, the program headers and the dynamic section of an ELF file,
    32- or 64-bit, either byte order -- the targets are x86-64, arm64 and
    riscv64, all little-endian, and nothing here assumes it."""

    def __init__(self, data: bytes):
        if data[:4] != b"\x7fELF":
            raise NotReadable("not an ELF file")
        if len(data) < 52 or data[4] not in (1, 2) or data[5] not in (1, 2):
            raise NotReadable("an ELF header this does not read")
        self.wide = data[4] == 2
        order = "<" if data[5] == 1 else ">"
        self.data, self.order = data, order

        def field(fmt: str, at: int) -> int:
            size = struct.calcsize(order + fmt)
            if at < 0 or at + size > len(data):
                raise NotReadable(f"truncated at offset {at:#x}")
            return struct.unpack_from(order + fmt, data, at)[0]
        self.field = field

        self.type = field("H", 16)
        if self.wide:
            phoff, phentsize, phnum = field("Q", 32), field("H", 54), field("H", 56)
        else:
            phoff, phentsize, phnum = field("I", 28), field("H", 42), field("H", 44)
        if phnum == 0:
            raise NotReadable("no program headers: an object file, not a program")
        # (type, flags, offset, size in the file) of each program header
        self.segments = []
        for i in range(phnum):
            at = phoff + i * phentsize
            if self.wide:
                kind, flags, offset, filesz = (field("I", at), field("I", at + 4),
                                               field("Q", at + 8), field("Q", at + 32))
            else:
                kind, offset, filesz, flags = (field("I", at), field("I", at + 4),
                                               field("I", at + 16), field("I", at + 24))
            self.segments.append((kind, flags, offset, filesz))

    def segment(self, kind: int):
        return next((s for s in self.segments if s[0] == kind), None)

    def dynamic(self) -> dict[int, int]:
        """tag -> value for every entry of the dynamic section, up to DT_NULL."""
        seg = self.segment(PT_DYNAMIC)
        if seg is None:
            return {}
        _kind, _flags, offset, filesz = seg
        size, fmt = (16, "q") if self.wide else (8, "i")
        out: dict[int, int] = {}
        for at in range(offset, offset + filesz - size + 1, size):
            tag = self.field(fmt, at) & ((1 << (size * 4)) - 1)
            value = self.field(fmt.upper(), at + size // 2)
            if tag == DT_NULL:
                break
            out[tag] = value
        return out


def elf_findings(elf: Elf) -> list[tuple[bool, str]]:
    """(holds, sentence) for each property a release can have: PIE first."""
    dynamic = elf.dynamic()
    flags, flags_1 = dynamic.get(DT_FLAGS, 0), dynamic.get(DT_FLAGS_1, 0)
    interp = elf.segment(PT_INTERP) is not None
    stack = elf.segment(PT_GNU_STACK)
    pie = elf.type == ET_DYN and (interp or bool(flags_1 & DF_1_PIE))
    now = DT_BIND_NOW in dynamic or bool(flags & DF_BIND_NOW) or bool(flags_1 & DF_1_NOW)
    return [
        (pie, "position-independent (PIE): ASLR can load it anywhere" if pie else
              ("ET_EXEC, not position-independent: it loads at the one address the "
               "linker chose, and ASLR cannot move it -- link with -pie"
               if elf.type == ET_EXEC else
               "a shared library or an ELF of an unexpected type, not a program")),
        (elf.segment(PT_GNU_RELRO) is not None,
         "RELRO: the relocated data is read-only once the loader is done"
         if elf.segment(PT_GNU_RELRO) is not None else
         "no PT_GNU_RELRO segment: link with -Wl,-z,relro"),
        (now, "BIND_NOW: every symbol is resolved at start, so the GOT can be read-only"
              if now else "no BIND_NOW: link with -Wl,-z,now"),
        (stack is not None and not stack[1] & PF_X,
         "a stack that is not executable" if stack is not None and not stack[1] & PF_X else
         ("an EXECUTABLE stack (PT_GNU_STACK has X): link with -Wl,-z,noexecstack"
          if stack is not None else
          "no PT_GNU_STACK segment, which many kernels read as an executable stack: "
          "link with -Wl,-z,noexecstack")),
    ]


def check_elf(paths: list[str], pie: bool = False) -> int:
    worst = 0
    for path in paths:
        try:
            elf = Elf(Path(path).read_bytes())
        except (OSError, NotReadable) as e:
            print(f"FAIL: {path}: {e}")
            worst = 2
            continue
        print(f"== {path} ({'64' if elf.wide else '32'}-bit ELF)")
        failed = 0
        for i, (holds, sentence) in enumerate(elf_findings(elf)):
            if i == 0 and not pie and not holds:
                print(f"  --    {sentence} (not required: no --pie)")
                continue
            print(f"  {'ok  ' if holds else 'FAIL'}  {sentence}")
            failed += not holds
        if failed:
            worst = max(worst, 1)
    if worst == 1:
        print("FAIL: the release is not linked with the hardening CMakeLists.txt asks for. "
              "See RMP_RELEASE_HARDENING there.")
    elif worst == 0:
        print("PASS: " + ("PIE, " if pie else "") + "RELRO, BIND_NOW and a non-executable stack")
    return worst


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        prog="binary_check.py",
        description="What a shipped binary asks of the player's machine.")
    sub = ap.add_subparsers(dest="os", required=True)
    win = sub.add_parser("windows", help="the DLLs an .exe imports, and its icon")
    win.add_argument("exe", nargs="+")
    win.add_argument("--require-icon", action="store_true")
    mac = sub.add_parser("macos", help="the minimum macOS of every slice")
    mac.add_argument("binary")
    mac.add_argument("--min-os", required=True)
    elf = sub.add_parser("elf", help="PIE, RELRO, BIND_NOW and a non-executable stack")
    elf.add_argument("binary", nargs="+")
    elf.add_argument("--pie", action="store_true",
                     help="require a position-independent executable too")
    args = ap.parse_args(argv)
    if args.os == "windows":
        return check_windows(args.exe, args.require_icon)
    if args.os == "elf":
        return check_elf(args.binary, args.pie)
    return check_macos(args.binary, args.min_os)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
