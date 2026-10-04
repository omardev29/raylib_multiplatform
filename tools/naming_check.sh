#!/usr/bin/env bash
# Fail if a name in our C and C++ breaks the naming rules clang-tidy cannot see.
#
# readability-identifier-naming checks the declarations the compiler parsed,
# and the compiler parses ONE branch of every #if: the web, iOS, Android and
# Windows code is invisible to a lint run on Linux. It cannot see member access
# at all (`foo_.x` is a use, not a declaration), and it says nothing about a
# macro that names something else's macro. So the rules are restated here, as
# text, over every branch:
#
#   R1  no `g_` names            file state is a struct per concern: `frame.open`
#   R2  no `kName` constants     constants are CONSTANT_CASE: `MAX_TAG_NAME`
#   R3  no `name_.x` / `name_->x`  an underscore never touches a dot or an arrow
#   R4  no `x._name` / `x->_name`  except a call to one of the eight hooks
#   R5  every #define we write starts with RMP_   (a vendored library's own
#       knobs, set in the file that configures it, are listed below by file)
#   R6  no constant named like somebody else's macro: `PI` is raylib's, `MIN`
#       and `MAX` are BSD <sys/param.h>'s -- the preprocessor replaces the name
#       on exactly the platforms that define it, green on Linux and red on
#       NetBSD twenty minutes later. And no constant starts with RMP_, which is
#       the macros' namespace.
#   R7  no retired name: `#if APP_SAVE_PORTABLE` after the rename is not an
#       error, it is 0, and it would have turned portable saves off on Windows
#       and nowhere else.
#
# Comments, strings and character literals do not count: they are blanked
# before any rule runs (a comment may well say what is forbidden).
#
# Usage:
#   tools/naming_check.sh            the tree, with the rules in ENFORCED
#   tools/naming_check.sh --all      the tree, with every rule (a report)
#   tools/naming_check.sh --macros   the names R6 forbids for a constant
#   tools/naming_check.sh FILE...    exactly those files, with every rule --
#                                    which is how the tests watch it go red
#
# The tree is include/, src/, tests/ and examples/, minus generated headers and
# tests/fixtures/ (third-party-shaped files kept for the licence tests).

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 - "$@" <<'PY'
import pathlib
import re
import sys

# The rules that hold on the tree today. A rule is written, proven red on a
# fixture, and joins this set the commit its rename lands.
ENFORCED = {"R4", "R5", "R6", "R7"}

RULES = {
    "R1": "a `g_` name: file state is a struct per concern (`frame.open`), "
          "examples use plain snake_case",
    "R2": "a `kName` constant: constants are CONSTANT_CASE",
    "R3": "an underscore touching a dot or an arrow (`name_.x`): private "
          "members are `_name`",
    "R4": "another object's private member (`x._name`): reach it through a "
          "method that says what is taken",
    "R5": "a macro without the RMP_ prefix",
    "R6": "a constant named like a macro somebody else defines, or starting "
          "with RMP_",
    "R7": "a retired name",
}

# The hooks the framework calls on your type. Calling one on another object
# (`a._collision(b)`, `behavior->_update(o, delta)`) is the one `._` allowed.
HOOKS = {"_ready", "_update", "_late_update", "_draw", "_collision", "_end",
         "_suspend", "_resume"}

# R3's single exception: cute_tiled names a struct field `class_` (C++ cannot
# have `class`), and tilemap.cpp reads `layer->class_.ptr`. It is not ours to
# rename.
FOREIGN_TRAILING = {"class_"}

# R5: a vendored library is configured by #defining ITS names, in the one file
# that compiles it. Listed by file, so the same name anywhere else still fails.
VENDOR_KNOBS = {
    "src/rmp/aseprite_impl.cpp": {"CUTE_ASEPRITE_IMPLEMENTATION"},
    "src/rmp/tiled_impl.cpp": {"CUTE_TILED_IMPLEMENTATION",
                               "CUTE_TILED_NO_EXTERNAL_TILESET_WARNING"},
    "src/rmp/rres_impl.cpp": {"RRES_IMPLEMENTATION", "RRES_RAYLIB_IMPLEMENTATION",
                              "RRES_SUPPORT_ENCRYPTION_AES",
                              "RRES_SUPPORT_ENCRYPTION_XCHACHA20"},
    "src/rmp/cjson_impl.c": {"CJSON_NESTING_LIMIT"},
    "src/rmp/ui/clay_impl.cpp": {"CLAY_IMPLEMENTATION"},
    "tests/unit_test.cpp": {"DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN"},
}

# R6: every macro the vendored headers define, read live so an update brings
# its new names with it...
VENDORED_HEADERS = [
    "thirdparty/raylib/src/raylib.h", "thirdparty/raylib/src/raymath.h",
    "thirdparty/raylib/src/rlgl.h", "thirdparty/clay/clay.h",
    "thirdparty/rres/rres.h", "thirdparty/rres/rres-raylib.h",
    "thirdparty/cute_tiled/cute_tiled.h", "thirdparty/cute_aseprite/cute_aseprite.h",
    "thirdparty/cJSON/cJSON.h", "thirdparty/raymob/raymob.h",
    "thirdparty/doctest/doctest.h",
]
# ...and the C library's, POSIX's and the BSDs' that bite as constant names.
# Windows' are left out on purpose: windows.h defines IN, OUT and NEAR, but it
# cannot share a translation unit with raylib.h at all (Rectangle, CloseWindow
# and DrawText collide), so no file of ours ever sees both.
SYSTEM_MACROS = {
    "MIN", "MAX", "ALIGN", "HZ", "NBBY", "NODEV", "BSD", "MAXPATHLEN", "PAGE_SIZE",
    "EOF", "NULL", "BUFSIZ", "FILENAME_MAX", "PATH_MAX", "NAME_MAX", "RAND_MAX",
    "CHAR_BIT", "EXIT_SUCCESS", "EXIT_FAILURE", "SEEK_SET", "SEEK_CUR", "SEEK_END",
    "INFINITY", "NAN", "HUGE_VAL", "DOMAIN", "SING", "OVERFLOW", "UNDERFLOW",
    "TLOSS", "PLOSS", "LITTLE_ENDIAN", "BIG_ENDIAN", "BYTE_ORDER", "CLOCKS_PER_SEC",
    "ERANGE", "EDOM", "EINVAL", "ENOENT", "EACCES", "EEXIST", "ENOMEM",
    "SIGINT", "SIGTERM", "SIGSEGV", "SIGABRT", "STDIN_FILENO", "STDOUT_FILENO",
    "STDERR_FILENO", "MB_LEN_MAX", "INT_MAX", "INT_MIN", "UINT_MAX", "SIZE_MAX",
    "FLT_MAX", "FLT_MIN", "FLT_EPSILON", "DBL_MAX", "DBL_EPSILON", "M_PI",
    # Apple's headers define TRUE and FALSE, and every iOS build sees them.
    "TRUE", "FALSE",
    # Not the C library's but ours: raymob's debug build passes -DDEBUG and
    # -D_DEBUG, so a constant called DEBUG breaks on Android only.
    "DEBUG", "NDEBUG",
}

# R7: names that existed and must not come back. A whole prefix where the
# family is retired (APP_), exact names otherwise.
RETIRED_PREFIXES = ("APP_",)
RETIRED_NAMES = {
    "PRODUCTION_BUILD", "RESOURCES_PATH", "RRES_PASSWORD", "RRES_PACK_FILE",
    "SMOKE_TEST_H", "RAY_TEST_REPORT_BOOT", "ALICEBLUE", "GIORNOGOLD",
}

SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cpp"}


def blank(text):
    """The text with comments, strings and character literals turned into
    spaces, newlines kept, so every offset and line number still holds.

    A scanner and not a regex, because the cases interleave: a `//` inside a
    string is not a comment, a quote inside a comment is not a string, a raw
    string R"x( ... )x" may hold both, and `1'000` is a digit separator rather
    than the start of a character literal."""
    out = list(text)
    n = len(text)
    i = 0

    def wipe(a, b):
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            wipe(i, j)
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            wipe(i, j)
            i = j
        elif c == '"':
            # A raw string: R"delim( ... )delim", with an optional prefix.
            if i > 0 and text[i - 1] == "R" and (i < 2 or not (text[i - 2].isalnum() or text[i - 2] == "_")
                                                 or text[i - 2:i] in ("8R", "LR", "uR", "UR")):
                paren = text.find("(", i)
                delim = text[i + 1:paren]
                end = text.find(")" + delim + '"', paren)
                j = n if end < 0 else end + len(delim) + 2
                wipe(i, j)
                i = j
                continue
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            wipe(i, min(j + 1, n))
            i = j + 1
        elif c == "'":
            # A digit separator when the token before it is a number.
            k = i - 1
            while k >= 0 and (text[k].isalnum() or text[k] in "_."):
                k -= 1
            token = text[k + 1:i]
            if token[:1].isdigit():
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            wipe(i, min(j + 1, n))
            i = j + 1
        else:
            i += 1
    return "".join(out)


def vendored_macros():
    names = set()
    for header in VENDORED_HEADERS:
        path = pathlib.Path(header)
        if not path.is_file():
            print(f"  FAIL  {header} is gone: R6 reads its macro names")
            sys.exit(1)
        for m in re.finditer(r"^\s*#\s*define\s+([A-Za-z_]\w*)",
                             path.read_text(encoding="utf-8", errors="replace"), re.M):
            names.add(m.group(1))
    return names


def tree():
    files = []
    for root in ("include", "src", "tests", "examples"):
        for path in sorted(pathlib.Path(root).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            posix = path.as_posix()
            if "/generated/" in posix or posix.startswith("tests/fixtures/"):
                continue
            files.append(path)
    return files


CONSTANT_DECL = re.compile(
    r"\b(?:constexpr|constinit|const)\b[^;={}()]*?\b([A-Za-z_]\w*)\s*(?:=|\{|;|\[)")
ENUM_BODY = re.compile(r"\benum\b(?:\s+(?:class|struct))?(?:\s+\w+)?(?:\s*:\s*[\w:\s]+)?\s*\{([^}]*)\}")


def check(path, rules, forbidden_macros):
    posix = path.as_posix()
    raw = path.read_text(encoding="utf-8", errors="replace")
    code = blank(raw)
    lines = code.splitlines()
    starts = [0]
    for k, ch in enumerate(code):
        if ch == "\n":
            starts.append(k + 1)

    def line_of(offset):
        lo, hi = 0, len(starts)
        while lo + 1 < hi:
            mid = (lo + hi) // 2
            if starts[mid] <= offset:
                lo = mid
            else:
                hi = mid
        return lo + 1

    found = []

    def hit(rule, offset, what):
        if rule in rules:
            found.append((rule, line_of(offset), what))

    for m in re.finditer(r"^[ \t]*#[ \t]*include\b.*$", code, re.M):
        # An include names a file, not an identifier.
        code = code[:m.start()] + " " * (m.end() - m.start()) + code[m.end():]

    for m in re.finditer(r"\bg_\w+", code):
        hit("R1", m.start(), m.group(0))
    for m in re.finditer(r"\bk[A-Z]\w*", code):
        hit("R2", m.start(), m.group(0))
    for m in re.finditer(r"\b(\w*[A-Za-z0-9]_)\s*(\.|->)(?!\.)", code):
        if m.group(1) in FOREIGN_TRAILING:
            continue
        hit("R3", m.start(), m.group(0).strip())
    for m in re.finditer(r"(?:\.|->)\s*(_\w+)", code):
        if m.group(1) not in HOOKS:
            hit("R4", m.start(), m.group(0))
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)", code, re.M):
        name = m.group(1)
        if not name.startswith("RMP_") and name not in VENDOR_KNOBS.get(posix, ()):
            hit("R5", m.start(1), f"#define {name}")
    names = [(m.group(1), m.start(1)) for m in CONSTANT_DECL.finditer(code)]
    for body in ENUM_BODY.finditer(code):
        for m in re.finditer(r"(?:^|,)\s*([A-Za-z_]\w*)", body.group(1)):
            names.append((m.group(1), body.start(1) + m.start(1)))
    for name, offset in names:
        if name in forbidden_macros:
            hit("R6", offset, f"{name} is a macro elsewhere")
        elif name.startswith("RMP_"):
            hit("R6", offset, f"{name} is in the macros' namespace")
    for m in re.finditer(r"\b[A-Za-z_]\w*\b", code):
        word = m.group(0)
        if word in RETIRED_NAMES or word.startswith(RETIRED_PREFIXES):
            hit("R7", m.start(), word)
    return found


args = sys.argv[1:]
forbidden = vendored_macros() | SYSTEM_MACROS
if args == ["--macros"]:
    print("\n".join(sorted(forbidden)))
    sys.exit(0)
report = False
if args and args[0] == "--all":
    report = True
    args = args[1:]

if args:
    files = [pathlib.Path(a) for a in args]
    rules = set(RULES)
else:
    files = tree()
    rules = set(RULES) if report else ENFORCED

fails = 0
per_rule = {}
for path in files:
    if not path.is_file():
        print(f"  FAIL  {path} does not exist")
        fails += 1
        continue
    for rule, line, what in check(path, rules, forbidden):
        per_rule[rule] = per_rule.get(rule, 0) + 1
        if not report:
            print(f"  FAIL  {path}:{line}: {rule} {what} -- {RULES[rule]}")
        fails += 1

if report:
    for rule in sorted(RULES):
        mark = "enforced" if rule in ENFORCED else "not yet"
        print(f"  {rule}  {per_rule.get(rule, 0):5d}  ({mark})  {RULES[rule]}")
    sys.exit(0)
if fails:
    print()
    print(f"FALLA: {fails} name(s) break the naming rules. CLAUDE.md, \"Naming\", says")
    print("       what to write instead and why.")
    sys.exit(1)
print(f"  ok    names follow the convention in {len(files)} file(s) "
      f"(rules: {' '.join(sorted(rules))})")
PY
