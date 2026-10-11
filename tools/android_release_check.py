#!/usr/bin/env python3
"""Fail unless Gradle's release variant compiled the game as a release.

Gradle configures its debug variant with CMAKE_BUILD_TYPE=Debug and its
release variant with RelWithDebInfo, and it never passed the PRODUCTION_BUILD
option raymob's CMakeLists.txt used to read. That option was therefore OFF for
both, and every release APK and AAB was compiled at -O0 with _DEBUG defined --
nothing failed, the games just ran slower than they should and with their debug
code in. The build type decides now, and this reads what the compiler was
actually told, from the compile database AGP writes for each variant and ABI.

Usage: tools/android_release_check.py [raymob/app/.cxx]
"""

import json
import pathlib
import shlex
import sys

RELEASE_VARIANTS = ("RelWithDebInfo", "Release", "MinSizeRel")


class Unreadable(Exception):
    """A compile database, or an entry of one, that this cannot read."""


def entries(db):
    """The entries of one compile database: a list of objects, or Unreadable.
    A build stopped halfway leaves half a file, and that is a FAIL naming it,
    not a traceback in the Android job's log."""
    try:
        data = json.loads(db.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, ValueError) as e:
        raise Unreadable(f"not a compile database this reads: {e}")
    if not isinstance(data, list) or not all(isinstance(e, dict) for e in data):
        raise Unreadable("not a list of compile commands")
    return data


def source_of(entry):
    source = entry.get("file", "")
    if not isinstance(source, str):
        raise Unreadable(f"an entry whose file is {source!r}, not a path")
    return source.replace("\\", "/")


def flags(entry):
    if "arguments" in entry:
        args = entry["arguments"]
        if not isinstance(args, list) or not all(isinstance(a, str) for a in args):
            raise Unreadable(f"an entry whose arguments are {args!r}, not a list of words")
        return list(args)
    command = entry.get("command", "")
    if not isinstance(command, str):
        raise Unreadable(f"an entry whose command is {command!r}, not a line")
    try:
        return shlex.split(command)
    except ValueError as e:
        raise Unreadable(f"an entry whose command does not split into words ({e}): {command}")


def main(argv):
    root = pathlib.Path(argv[1] if len(argv) > 1 else "raymob/app/.cxx")
    databases = sorted(p for p in root.rglob("compile_commands.json")
                       if any(v in p.parts for v in RELEASE_VARIANTS))
    if not databases:
        print(f"FAIL: no release compile database under {root} -- did bundleRelease run?")
        return 1

    fails = 0
    unread = 0
    checked = 0
    for db in databases:
        try:
            read = [(source_of(entry), flags(entry)) for entry in entries(db)]
        except Unreadable as e:
            unread += 1
            print(f"  FAIL  {db}: {e}")
            continue
        for source, args in read:
            problems = []
            if "/thirdparty/raylib/" in source:
                # raylib's own copy: the old branch forced -O0 on it as well.
                if "-O0" in args:
                    problems.append("-O0")
            elif "/src/" in source and "/thirdparty/" not in source:
                # The game and the framework: the files the old option got wrong.
                checked += 1
                if "-DRMP_PRODUCTION_BUILD=1" not in args:
                    problems.append("no -DRMP_PRODUCTION_BUILD=1")
                if "-O0" in args:
                    problems.append("-O0")
                if "-D_DEBUG" in args or "-DDEBUG" in args:
                    problems.append("_DEBUG/DEBUG defined")
            else:
                continue
            if problems:
                fails += 1
                print(f"  FAIL  {source}: {', '.join(problems)}  ({db})")
    if unread:
        print(f"FAIL: {unread} release compile database(s) could not be read")
    if fails:
        print(f"FAIL: {fails} release translation unit(s) were compiled as debug")
    if unread or fails:
        return 1
    if checked == 0:
        print(f"FAIL: the release compile databases under {root} hold no file from src/")
        return 1
    print(f"PASS: {checked} release translation unit(s) in {len(databases)} database(s): "
          "RMP_PRODUCTION_BUILD=1, no -O0, no _DEBUG")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
