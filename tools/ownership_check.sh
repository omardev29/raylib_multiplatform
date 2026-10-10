#!/usr/bin/env bash
# Fail if the framework owns memory, or a raylib resource, by hand.
#
# The rules (GUIDELINES.md, "Punteros y propiedad"):
#
#   O1  no bare new/delete, no malloc/free, under include/rmp/ or src/rmp/.
#       What we own is a unique_ptr or a value; what we refer to is a Handle,
#       an rmp::Ref or a reference.
#   O2  no raylib Load*/Unload* outside the files that own what it makes,
#       under include/rmp/, src/rmp/ and examples/ (C++ only: the plain C
#       example has no destructor to put one in). LoadTexture goes inside the
#       rmp::Texture that frees it, and Unload* appears where the owner lets go
#       -- not in a scene, a widget or an example, where a forgotten Unload is
#       a leak and a second one is a double free. The names are read live from
#       raylib.h and rres-raylib.h, so a raylib update brings its new ones.
#
# Each rule is a ratchet, like seam_check.sh: the files below may keep their
# calls, each with the reason, and the lists only ever shrink. Adding a file
# is a review question, not a fix.
#
# Comments and strings do not count: they are blanked first (tools/cpp_text.py),
# so a comment may name what is forbidden.
#
# Usage:
#   tools/ownership_check.sh            the tree
#   tools/ownership_check.sh FILE...    exactly those files, with both rules --
#                                       how the tests and the docs watch it go red
# Exit:   0 = clean, 1 = a call outside its list (with file:line)

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$@" <<'PY'
import pathlib
import re
import sys

sys.path.insert(0, "tools")
from cpp_text import Lines, blank, tree  # noqa: E402  (the shared scanner)

RULES = {
    "O1": "owns memory by hand: std::unique_ptr or a value owns it; rmp::Handle<T>, "
          "rmp::Ref<T> or a reference refers to it",
    "O2": "a raylib resource made or freed outside its owner: load it through "
          "rmp::assets, which frees it with the handle",
}

# Files allowed to keep their calls, and why. THE LIST ONLY SHRINKS.
ALLOWED = {
    "O1": {
        # raylib's LoadFileData contract: the buffer is RL_MALLOC'd by us and
        # RL_FREE'd by raylib's caller. C ABI, not ours to own.
        "src/rmp/loader_hook.cpp",
        # The same contract on the way out of the pack: pack_read() returns
        # bytes UnloadFileData() frees.
        "src/rmp/pack.cpp",
    },
    "O2": {
        # The loaders: every resource rmp::assets hands out is made here, into
        # the handle whose last release frees it.
        "src/rmp/assets.cpp",
        # The resource table: where a handle's last release calls Unload*.
        "src/rmp/resource.cpp",
        # rmp::audio's voices and music streams, made and freed by the mixer
        # that owns them.
        "src/rmp/audio.cpp",
        # An aseprite sheet: its frames are packed into an Image, freed on the
        # spot, and a Texture the sheet owns.
        "src/rmp/animation.cpp",
        # raylib's file-loading callback, which IS LoadFileData's contract.
        "src/rmp/loader_hook.cpp",
        # A chunk of the rres pack, decoded and handed to the loaders above.
        "src/rmp/pack.cpp",
    },
}

# `new` as an allocation (`new T`, `new T[`, `new (place) T`), `delete`, and
# the C allocator. Placement new is `new (` and is caught on purpose: it is an
# allocation decision too. `= delete` is a deleted function, not a free.
ALLOCATION = re.compile(r"(?<![\w:])(new|delete)(?![\w])|(?<![\w])(malloc|calloc|realloc|free)\s*\(")
DELETED = re.compile(r"=\s*delete\b|\boperator\s+(new|delete)\b")

RAYLIB_HEADERS = ["thirdparty/raylib/src/raylib.h", "thirdparty/rres/rres-raylib.h"]


def raylib_lifetimes():
    """Every Load*/Unload* function raylib and rres-raylib declare."""
    names = set()
    for header in RAYLIB_HEADERS:
        path = pathlib.Path(header)
        if not path.is_file():
            print(f"  FAIL  {header} is gone: O2 reads its Load*/Unload* names")
            sys.exit(1)
        for m in re.finditer(r"^RLAPI\b[^(;]*?\b((?:Load|Unload)\w*)\s*\(",
                             path.read_text(encoding="utf-8", errors="replace"), re.M):
            names.add(m.group(1))
    return names


def in_scope(posix, rule, explicit):
    # C has no destructor to put an Unload in: examples/plain_c pairs its
    # Load and Unload in one function, which is how C owns anything.
    if rule == "O2" and (posix.endswith(".c") or posix.endswith("smoke_test.h")):
        return False
    if explicit:
        return True
    if posix.endswith(("_impl.cpp", "_impl.c")):
        return False  # a vendored library compiled once: its own business
    if rule == "O1":
        return posix.startswith(("include/rmp/", "src/rmp/"))
    return posix.startswith(("include/rmp/", "src/rmp/", "examples/"))


def check(path, lifetimes, explicit):
    posix = path.as_posix()
    code = blank(path.read_text(encoding="utf-8", errors="replace"))
    line_of = Lines(code).of
    found = []
    if in_scope(posix, "O1", explicit) and posix not in ALLOWED["O1"]:
        allocating = DELETED.sub(lambda m: " " * len(m.group(0)), code)
        for m in ALLOCATION.finditer(allocating):
            found.append(("O1", line_of(m.start()), m.group(1) or m.group(2)))
    if in_scope(posix, "O2", explicit) and posix not in ALLOWED["O2"]:
        for m in re.finditer(r"(?<![\w.>])(\w+)\s*\(", code):
            if m.group(1) in lifetimes:
                found.append(("O2", line_of(m.start(1)), m.group(1)))
    return found


args = sys.argv[1:]
explicit = bool(args)
files = [pathlib.Path(a) for a in args] if explicit else tree(("include", "src", "examples"))
lifetimes = raylib_lifetimes()
fails = 0
for path in files:
    if not path.is_file():
        print(f"  FAIL  {path} does not exist")
        fails += 1
        continue
    for rule, line, what in check(path, lifetimes, explicit):
        print(f"  FAIL  {path}:{line}: {rule} {what} -- {RULES[rule]}")
        fails += 1

if fails:
    print()
    print(f"FAIL: {fails} call(s) outside the allowed lists. tools/ownership_check.sh")
    print("      says which file owns what, and why.")
    sys.exit(1)
print(f"  ok    nothing owned by hand in {len(files)} file(s): new/delete/malloc/free "
      f"only in the {len(ALLOWED['O1'])} allowed, raylib's {len(lifetimes)} Load*/Unload* "
      f"only in the {len(ALLOWED['O2'])} owners")
PY
