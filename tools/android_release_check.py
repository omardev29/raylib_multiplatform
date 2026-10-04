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


def flags(entry):
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry.get("command", ""))


def main(argv):
    root = pathlib.Path(argv[1] if len(argv) > 1 else "raymob/app/.cxx")
    databases = sorted(p for p in root.rglob("compile_commands.json")
                       if any(v in p.parts for v in RELEASE_VARIANTS))
    if not databases:
        print(f"FAIL: no release compile database under {root} -- did bundleRelease run?")
        return 1

    fails = 0
    checked = 0
    for db in databases:
        for entry in json.loads(db.read_text(encoding="utf-8")):
            source = entry.get("file", "").replace("\\", "/")
            args = flags(entry)
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
    if checked == 0:
        print(f"FAIL: the release compile databases under {root} hold no file from src/")
        return 1
    if fails:
        print(f"FAIL: {fails} release translation unit(s) were compiled as debug")
        return 1
    print(f"PASS: {checked} release translation unit(s) in {len(databases)} database(s): "
          "RMP_PRODUCTION_BUILD=1, no -O0, no _DEBUG")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
