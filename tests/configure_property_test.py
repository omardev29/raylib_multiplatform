"""tools/configure.py held to its promises for ANY .toml.

A .toml can say anything TOML can say -- a list where a string goes, nan,
2**63 - 1, a title in Arabic, a password with `${HOME}` in it -- and
configure.py answers each with one of two things: a ConfigError that names
its line, or build files that hand every build system exactly what was
written. Nothing in between: no traceback (R7 in the phase 16 design), and no
value that reaches a compiler, CMake, Gradle or XcodeGen as something else.

  validate()             total: refused at the line that says it, or taken,
                         and what it takes has the shape it promises
  what it takes          config.h decodes, and compiles, to the same bytes;
                         CMake reads project.cmake back to the same values;
                         Gradle reads generated.properties back (ISO-8859-1,
                         java.util.Properties); project.yml reads back as
                         YAML; the manifest is XML
  resolve_version()      total, ASCII, at least 1, and a code is one release's
  locate(), toml_comments(), narrow_to(), expand_targets(), expand_upx()  total

Every property was seen red: the commits say how. A counterexample is also a
named case in tests/configure_test.py, which runs without Hypothesis.
"""

from __future__ import annotations

import contextlib
import copy
import datetime
import importlib.util
import io
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib
import unittest
import xml.etree.ElementTree as ElementTree
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hypothesis_profiles  # noqa: E402,F401  (first: the profile, or the skip)
from hypothesis import example, given, settings, strategies as st  # noqa: E402

REPO = hypothesis_profiles.REPO
IN_BUILD_IMAGE = Path("/etc/raylib-build-image.json").is_file()


def load(name: str):
    """tools/<name>.py by its path, as `tools.<name>`: the name mutmut gives
    the file. It records which tests reach a function under its module's own
    name, and a test that loaded it under another reaches nothing to mutate."""
    spec = importlib.util.spec_from_file_location(f"tools.{name}", REPO / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


configure = load("configure")

try:
    import yaml
except ImportError:
    yaml = None


@contextlib.contextmanager
def quiet():
    """configure.py warns on stderr and reports on stdout; a test is silent."""
    with contextlib.redirect_stderr(io.StringIO()), contextlib.redirect_stdout(io.StringIO()):
        yield


# No tag: resolve_version() reads the environment, and a CI run on a tag sets it.
NO_TAG = {"GITHUB_REF_TYPE": "", "GITHUB_REF_NAME": ""}


def base_config() -> dict:
    """DEFAULTS with the example identifiers made real, as a game has them."""
    cfg = copy.deepcopy(configure.DEFAULTS)
    cfg["android"]["application_id"] = "com.real.game"
    cfg["ios"]["bundle_id"] = "com.real.game"
    return cfg


def leaves(node: dict, path: tuple = ()):
    for key, value in node.items():
        if isinstance(value, dict) and path + (key,) != ("ios", "settings"):
            yield from leaves(value, path + (key,))
        else:
            yield path + (key,), value


LEAVES = dict(leaves(configure.DEFAULTS))
TABLES = sorted({path[:i] for path in LEAVES for i in range(1, len(path))})


def same(a, b) -> bool:
    """==, with nan equal to nan: what the writer below wrote is what TOML read."""
    if isinstance(a, float) and isinstance(b, float) and math.isnan(a) and math.isnan(b):
        return True
    if type(a) is not type(b):
        return False
    if isinstance(a, dict):
        return a.keys() == b.keys() and all(same(a[k], b[k]) for k in a)
    if isinstance(a, list):
        return len(a) == len(b) and all(same(x, y) for x, y in zip(a, b))
    return a == b


# ---------------------------------------------------------------------------
# What a .toml can say
# ---------------------------------------------------------------------------

# What the code under test decides on: the C, CMake, YAML, .properties and
# regex characters, digits that are not ASCII, words YAML reads as something
# else, line breaks that are not "\n".
DECISIVE = st.sampled_from([
    "??/", '"', "'", "\\", "\\n", "${x}", "$ENV{HOME}", "@x@", ";", "#", ":", "=", "!",
    "\n", "\r", "\t", "\x00", "\x07", "\x1b", "\x7f", "\x85", " ", "﻿", "‮",
    "é", "日本", "😀", "١", " ", "- ", "*", "&", "? ", "%", "`", "|", ">", "{", "}",
    "[", "]", ",", "true", "no", "null", "~", "nan", "1.5", "0x1", "\\u00e9", "a",
])
TEXT = st.one_of(st.text(max_size=12),
                 st.lists(st.one_of(DECISIVE, st.text(max_size=3)), max_size=6).map("".join))
SCALAR = st.one_of(TEXT, st.integers(-2**63, 2**63 - 1), st.floats(), st.booleans(),
                   st.dates(), st.datetimes(), st.times())
VALUE = st.recursive(SCALAR, lambda inner: st.one_of(
    st.lists(inner, max_size=3), st.dictionaries(st.text(max_size=5), inner, max_size=3)),
    max_leaves=6)
NAME = st.text(alphabet="abcdefghijklmnopqrstuvwxyz_", min_size=1, max_size=8)
SETTING = st.one_of(st.sampled_from(["DEVELOPMENT_TEAM", "CODE_SIGN_STYLE",
                                     "OTHER_CPLUSPLUSFLAGS", "OTHER_LDFLAGS[sdk=iphoneos*]",
                                     "ENABLE_BITCODE"]), TEXT.filter(bool))


def near(path: tuple):
    """A value of the type `path` takes, mostly fine, sometimes not."""
    default = LEAVES[path]
    dotted = ".".join(path)
    if isinstance(default, dict):   # [ios.settings]: Xcode's names, any value
        return st.dictionaries(SETTING, VALUE, max_size=3)
    if isinstance(default, bool):
        return st.booleans()
    if isinstance(default, int) or dotted == "ui.scale":
        return st.one_of(st.integers(-2**63, 2**63 - 1), st.integers(-2, 70000),
                         st.sampled_from([0, 1, 15, 16, 21, 36, 1000, 1001, 16384, 2_000_000_000]),
                         *([st.floats()] if dotted == "ui.scale" else []))
    if isinstance(default, float):
        return st.one_of(st.floats(), st.floats(0, 1), st.integers(-1, 2))
    if isinstance(default, list):
        items = configure.ALLOWED_ITEMS.get(dotted)
        return st.lists(st.one_of(st.sampled_from(items()), TEXT) if items else TEXT, max_size=4)
    if dotted in configure.ALLOWED:
        return st.one_of(st.sampled_from(sorted(configure.ALLOWED[dotted])), TEXT)
    return st.one_of(TEXT, st.just(default), st.just(default + "\n"),
                     st.builds(lambda a, b: a + default + b, TEXT, TEXT),
                     st.text(alphabet=st.characters(categories=["Nd"]), min_size=1, max_size=3)
                     .map(lambda d: f"{d}.{d}"))


def put(tree: dict, path: tuple, value) -> None:
    for step in path[:-1]:
        node = tree.setdefault(step, {})
        if not isinstance(node, dict):
            return
        tree = node
    tree[path[-1]] = value


@st.composite
def user_tomls(draw) -> dict:
    """What somebody wrote in their .toml: known keys with values near or far
    from their type, a typo of a key, a value where a table goes, and Xcode
    settings of any name."""
    user: dict = {}
    for _ in range(draw(st.integers(0, 6))):
        kind = draw(st.sampled_from(["near", "near", "near", "any", "unknown", "table", "ios"]))
        if kind in ("near", "any"):
            path = draw(st.sampled_from(sorted(LEAVES)))
            value = draw(near(path) if kind == "near" else VALUE)
        elif kind == "unknown":
            path = draw(st.one_of(st.tuples(NAME), st.sampled_from(TABLES).flatmap(
                lambda table: NAME.map(lambda name: table + (name,)))))
            value = draw(VALUE)
        elif kind == "table":
            path, value = draw(st.sampled_from(TABLES)), draw(SCALAR)
        else:
            path, value = ("ios", "settings", draw(SETTING)), draw(VALUE)
        put(user, path, value)
    return user


# ---------------------------------------------------------------------------
# Writing it down
# ---------------------------------------------------------------------------

def toml_string(text: str) -> str:
    out = []
    for c in text:
        if c in '"\\':
            out.append("\\" + c)
        elif ord(c) < 0x20 or ord(c) == 0x7F:
            out.append(f"\\u{ord(c):04x}")
        else:
            out.append(c)
    return '"' + "".join(out) + '"'


def toml_key(key: str) -> str:
    return key if re.fullmatch(r"[A-Za-z0-9_-]+", key) else toml_string(key)


def toml_value(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if isinstance(value, float):
        if math.isnan(value):
            return "nan"
        if math.isinf(value):
            return "inf" if value > 0 else "-inf"
        return repr(value)
    if isinstance(value, str):
        return toml_string(value)
    if isinstance(value, (datetime.date, datetime.time)):
        return value.isoformat()
    if isinstance(value, list):
        return "[" + ", ".join(toml_value(v) for v in value) + "]"
    return "{" + ", ".join(f"{toml_key(k)} = {toml_value(v)}" for k, v in value.items()) + "}"


def known_table(path: tuple) -> bool:
    """A table the schema has."""
    node = configure.DEFAULTS
    for step in path:
        if not isinstance(node, dict) or step not in node:
            return False
        node = node[step]
    return isinstance(node, dict)


def first(path: tuple, choices: list[str]) -> str:
    return choices[0]


def toml_document(user: dict, pick=first) -> tuple[str, dict]:
    """The .toml a person writes for `user`: values above every section, then
    its tables. Each table is written as `pick(path, choices)` says -- a
    [section] of its own, a value inline, `{...}`, or dotted keys, `name.key =
    value` -- by default a [section] for one the schema has and for a new
    one above every section, inline for any other. And the line of every key,
    as (section, key) -> line -- the first line that names it, the line of a
    table that holds it inline -- and of every header, as (section, None)."""
    lines: list[str] = []
    where: dict = {}

    def claim(path: tuple, node: dict):
        """Every key inside a table written on the line just written."""
        for key, value in node.items():
            where.setdefault((".".join(path), key), len(lines))
            if isinstance(value, dict):
                where.setdefault((".".join(path + (key,)), None), len(lines))
                claim(path + (key,), value)

    def dotted(section: tuple, prefix: tuple, node: dict):
        for key, value in node.items():
            names = prefix + (key,)
            if isinstance(value, dict) and value and pick(section + names,
                                                          ["inline", "dotted"]) == "dotted":
                dotted(section, names, value)
                continue
            lines.append(f"{'.'.join(toml_key(p) for p in names)} = {toml_value(value)}")
            for i in range(len(names)):
                where.setdefault((".".join(section + names[:i]), names[i]), len(lines))
                if i:   # a table the line writes, with no header of its own
                    where.setdefault((".".join(section + names[:i]), None), len(lines))
            if isinstance(value, dict):
                where.setdefault((".".join(section + names), None), len(lines))
                claim(section + names, value)

    def table(path: tuple, node: dict):
        name = ".".join(path)
        if path:
            lines.append(f"[{'.'.join(toml_key(p) for p in path)}]")
            where[(name, None)] = len(lines)
            # The line that writes key path[-1] of the table above, too.
            where.setdefault((".".join(path[:-1]), path[-1]), len(lines))
        subtables = []
        for key, value in node.items():
            here = path + (key,)
            style = "inline"
            if isinstance(value, dict):
                style = pick(here, (["header", "inline"] if known_table(here) or not path
                                    else ["inline", "header"]) + (["dotted"] if value else []))
            if style == "header":
                subtables.append(key)
            elif style == "dotted":
                dotted(path, (key,), value)
            else:
                lines.append(f"{toml_key(key)} = {toml_value(value)}")
                where[(name, key)] = len(lines)
                if isinstance(value, dict):
                    where.setdefault((".".join(here), None), len(lines))
                    claim(here, value)
        for key in subtables:
            table(path + (key,), node[key])

    table((), user)
    return "\n".join(lines) + "\n", where


# ---------------------------------------------------------------------------
# Reading back what was generated, as each build system reads it
# ---------------------------------------------------------------------------

C_SIMPLE = {"'": 0x27, '"': 0x22, "?": 0x3F, "\\": 0x5C, "a": 7, "b": 8, "f": 12, "n": 10,
            "r": 13, "t": 9, "v": 11}


def c_string(literal: str) -> bytes:
    """The bytes a C string literal holds, or AssertionError when it is not one
    -- a raw quote, newline or control character inside it, an unknown escape."""
    assert len(literal) >= 2 and literal[0] == literal[-1] == '"', literal
    body, out, i = literal[1:-1], bytearray(), 0
    # A trigraph is another character where C reads them, and a warning --
    # an error under -Werror -- where it does not.
    assert not re.search(r"\?\?[=(/)'<!>-]", body), f"a trigraph in {literal!r}"
    while i < len(body):
        c = body[i]
        assert c != '"' and (ord(c) >= 0x20 and ord(c) != 0x7F), \
            f"{c!r} raw inside a C string literal: {literal!r}"
        if c != "\\":
            out += c.encode("utf-8")
            i += 1
            continue
        i += 1
        assert i < len(body), f"a lone backslash ends {literal!r}"
        e = body[i]
        if e in C_SIMPLE:
            out.append(C_SIMPLE[e])
            i += 1
        elif e in "01234567":
            digits = re.match(r"[0-7]{1,3}", body[i:]).group(0)
            out.append(int(digits, 8))
            i += len(digits)
        elif e == "x":
            digits = re.match(r"[0-9A-Fa-f]+", body[i + 1:])
            assert digits, f"\\x with no digit in {literal!r}"
            out.append(int(digits.group(0), 16))
            i += 1 + len(digits.group(0))
        else:
            raise AssertionError(f"unknown escape \\{e} in {literal!r}")
    return bytes(out)


def c_float_of(value) -> float | None:
    """What C's float makes of `value`: None for nan, an infinity or a number
    beyond its range, none of which is a float. A number this close to 0 is 0
    -- and a compiler warns about the literal (clang -Wliteral-range, gcc
    -Woverflow), which the framework's -Werror makes an error."""
    try:
        number = float(value)
        return struct.unpack("<f", struct.pack("<f", number))[0] if math.isfinite(number) else None
    except OverflowError:
        return None


C_FLOAT = re.compile(r"(?:[0-9]+\.[0-9]*|\.[0-9]+)(?:[eE][-+]?[0-9]+)?f|[0-9]+[eE][-+]?[0-9]+f")


def c_defines(header: str) -> dict[str, str]:
    """`#define NAME VALUE` in the generated header, and nothing raw in it a C
    file cannot hold."""
    assert not re.search(r"[\x00-\x08\x0b-\x1f\x7f]", header), "a control character in config.h"
    return {m.group(1): m.group(2) for m in re.finditer(r"^#define (RMP_\w+) +(.*)$", header, re.M)}


def java_properties(data: bytes) -> dict[str, str]:
    """java.util.Properties.load(InputStream), as raymob/settings.gradle reads
    generated.properties: OpenJDK's LineReader.readLine(), load0() and
    loadConvert(), line by line -- ISO-8859-1, `#`/`!` comments, a backslash
    before a line end continuing the line, the key up to the first unescaped
    `=`, `:` or blank. Checked against the JVM itself:
    ToolsReadBackTest.test_java_reads_generated_properties_as_written."""
    text = data.decode("latin-1")
    pos, size = 0, len(text)

    def read_line():
        nonlocal pos
        buf: list[str] = []
        skip_white, appended, backslash, skip_lf = True, False, False, False
        while True:
            if pos >= size:
                if not buf:
                    return None
                return "".join(buf[:-1] if backslash else buf)
            c = text[pos]
            pos += 1
            if skip_lf:
                skip_lf = False
                if c == "\n":
                    continue
            if skip_white:
                if c in " \t\f" or (not appended and c in "\r\n"):
                    continue
                skip_white, appended = False, False
            if not buf and c in "#!":
                while pos < size and text[pos] not in "\r\n":
                    pos += 1
                if pos >= size:
                    return None
                skip_white = True
                continue
            if c not in "\r\n":
                buf.append(c)
                backslash = (not backslash) if c == "\\" else False
                continue
            if not buf:
                skip_white = True
                continue
            if pos >= size:
                return "".join(buf[:-1] if backslash else buf)
            if not backslash:
                if c == "\r" and text[pos] == "\n":
                    pos += 1
                return "".join(buf)
            buf.pop()
            skip_white, appended, backslash = True, True, False
            if c == "\r" and text[pos] == "\n":
                pos += 1

    def convert(s: str) -> str:
        out, j = [], 0
        while j < len(s):
            c = s[j]
            j += 1
            if c != "\\":
                out.append(c)
                continue
            e = s[j] if j < len(s) else ""
            j += 1
            if e == "u":
                code = s[j:j + 4]
                assert re.fullmatch(r"[0-9A-Fa-f]{4}", code), f"Malformed \\uxxxx encoding: {s!r}"
                out.append(chr(int(code, 16)))
                j += 4
            else:
                out.append({"t": "\t", "r": "\r", "n": "\n", "f": "\f"}.get(e, e))
        # UTF-16 surrogate pairs from \u escapes, as Java strings hold them;
        # a lone one stays, as it does in Java.
        return "".join(out).encode("utf-16", "surrogatepass").decode("utf-16", "surrogatepass")

    props: dict[str, str] = {}
    while (line := read_line()) is not None:
        key_len, value_start, has_sep, backslash = 0, len(line), False, False
        while key_len < len(line):
            c = line[key_len]
            if c in "=:" and not backslash:
                value_start, has_sep = key_len + 1, True
                break
            if c in " \t\f" and not backslash:
                value_start = key_len + 1
                break
            backslash = (not backslash) if c == "\\" else False
            key_len += 1
        while value_start < len(line):
            c = line[value_start]
            if c not in " \t\f":
                if not has_sep and c in "=:":
                    has_sep = True
                else:
                    break
            value_start += 1
        props[convert(line[:key_len])] = convert(line[value_start:])
    return props


def generate(cfg: dict) -> dict[str, str]:
    """Every build file the generators write for `cfg`, by path, written
    nowhere: write() is captured, and the raylib flags (raylib's config.h,
    the same for every .toml) are left out."""
    out: dict[str, str] = {}

    def capture(path, content):
        out[Path(path).relative_to(REPO).as_posix()] = content
        return True

    targets = configure.expand_targets(cfg["targets"]["enabled"], cfg["targets"]["disabled"])
    with mock.patch.object(configure, "write", capture), \
            mock.patch.object(configure, "gen_raylib_flags", lambda _cfg: None), \
            mock.patch.dict(os.environ, NO_TAG), quiet():
        configure.gen_cmake(cfg)
        configure.gen_app_config(cfg)
        configure.gen_gradle_properties(cfg, targets)
        configure.gen_android_manifest(cfg, targets)
        configure.gen_ios_project(cfg)
    return out


# A valid .toml, changed where a game would change it: free text, the
# numbers, the choices, and Xcode settings.
FREE = [("window", "title"), ("resources", "rres_password"), ("ui", "font"),
        ("deploy", "credits_note"), ("icon", "source"), ("android", "admob", "app_id"),
        ("android", "admob", "interstitial_id"), ("android", "admob", "rewarded_id"),
        ("ios", "deployment_target"), ("project", "name"), ("android", "application_id"),
        ("ios", "bundle_id")]


@st.composite
def changed_configs(draw) -> dict:
    cfg = base_config()
    for _ in range(draw(st.integers(1, 4))):
        path = draw(st.one_of(st.sampled_from(FREE), st.sampled_from(sorted(LEAVES))))
        put(cfg, path, draw(near(path)))
    cfg["ios"]["settings"] = draw(st.dictionaries(
        SETTING, st.one_of(TEXT, st.booleans(), st.integers(-5, 5), st.lists(TEXT, max_size=3),
                           VALUE), max_size=3))
    return cfg


def taken(cfg: dict) -> bool:
    """validate() takes it. A refusal is R7's to check (TotalTest)."""
    try:
        with quiet():
            configure.validate(cfg, False)
    except configure.ConfigError:
        return False
    return True


# How toml_document() is asked to write a table: one the schema has, or a new
# one above every section; a new one inside a table. An @example names its
# tables' forms, in the order they are written, through the seed that picks them.
KNOWN = ["header", "inline", "dotted"]
NEW = ["inline", "header", "dotted"]
TOP = (KNOWN, "header")


def seed_for(*picks: tuple[list[str], str]) -> int:
    """The first seed whose picks, one a table in turn, are `picks`."""
    for seed in range(100_000):
        chooser = random.Random(seed)
        if all(chooser.choice(choices) == want for choices, want in picks):
            return seed
    raise AssertionError(f"no seed picks {picks}")


class TotalTest(unittest.TestCase):
    """R7: a .toml is refused with a ConfigError that names its line, or taken.
    Never another exception, and never a refusal nobody can find."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.toml = Path(cls._tmp.name) / "raylib_multiplatform.toml"

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def run_toml(self, user: dict, strict: bool, seed: int = 0):
        chooser = random.Random(seed)
        text, where = toml_document(user, lambda path, choices: chooser.choice(choices))
        self.assertTrue(same(tomllib.loads(text), user), f"the test's writer:\n{text}")
        self.toml.write_text(text, encoding="utf-8")
        with mock.patch.object(configure, "TOML", self.toml), mock.patch.dict(os.environ, NO_TAG), \
                quiet():
            try:
                cfg = configure.load_config()
                configure.validate(cfg, strict)
                targets = configure.expand_targets(cfg["targets"]["enabled"],
                                                   cfg["targets"]["disabled"])
                configure.expand_upx(cfg, targets)
                configure.app_defines(cfg)
            except configure.ConfigError as refused:
                return refused, configure.locate_from(refused), where, text
        return None, None, where, text

    @example(user={"zzz": 1}, seed=0)
    @example(user={"app": {"max_delta": math.nan}}, seed=seed_for(TOP))
    # Tables nobody's schema has, written each way TOML has: [window.zzz]
    # pointed at [window] -- or nowhere, with no [window] in the file --
    # `zzz.a = 1` and `title.x = 1` nowhere.
    @example(user={"window": {"zzz": {"a": 1}}}, seed=seed_for(TOP, (NEW, "header")))
    @example(user={"android": {"admob": {"zzz": {"a": 1}}}},
             seed=seed_for(TOP, TOP, (NEW, "header")))
    @example(user={"zzz": {"a": 1}}, seed=seed_for((KNOWN, "dotted")))
    @example(user={"window": {"title": {"x": 1}}}, seed=seed_for(TOP, (NEW, "dotted")))
    # U+0085 in a string: splitlines() broke the line there, and the error
    # under it pointed one line too far down.
    @example(user={"android": {"admob": {"app_id": "\x85"}}, "deploy": {"licenses": False}},
             seed=seed_for(TOP, TOP, TOP))
    # A header's names are TOML's: [ios.settings.":"] was the text `ios.settings.":"`.
    @example(user={"ios": {"settings": {":": {}}}},
             seed=seed_for(TOP, TOP, (["inline", "header"], "header")))
    # A table the schema has, written inline or dotted: `window.title = 5` and
    # `window = {title = 5}` above every section pointed nowhere, and so did
    # `admob.app_id = 5` in [android].
    @example(user={"window": {"title": 5}}, seed=seed_for((KNOWN, "inline")))
    @example(user={"window": {"title": 5}}, seed=seed_for((KNOWN, "dotted")))
    @example(user={"android": {"admob": {"app_id": 5}}}, seed=seed_for(TOP, (KNOWN, "dotted")))
    # The nightly profile's: "[targets]: nothing left to build" names the
    # table, and `targets.enabled = []` has no [targets] header to point at.
    @example(user={"targets": {"enabled": []}}, seed=seed_for((KNOWN, "dotted")))
    @example(user={"targets": {"enabled": []}}, seed=seed_for((KNOWN, "inline")))
    @given(user_tomls(), st.integers(0, 2**32))
    def test_any_toml_is_refused_at_its_line_or_taken(self, user, seed):
        refused, spot, where, text = self.run_toml(user, strict=False, seed=seed)
        if refused is None:
            return
        self.assertIsNotNone(spot, f"refused with no line to point at:\n{refused}\n---\n{text}")
        named = refused.where
        if named is None:
            m = configure._WHERE_RE.match(str(refused))
            named = (m.group(1), m.group(2)) if m else None
        if named in where:
            self.assertEqual(spot[0], where[named],
                             f"{refused}\npointed at line {spot[0]}, and the key is on line "
                             f"{where[named]}:\n{text}")

    @given(user_tomls(), st.integers(0, 2**32))
    def test_a_release_check_is_total_too(self, user, seed):
        self.run_toml(user, strict=True, seed=seed)

    # A .toml an editor saved as Latin-1 -- or anything else that is not
    # UTF-8 -- was a UnicodeDecodeError, and arrays nested deeper than
    # tomllib's recursion a RecursionError: both a traceback.
    @example(data='[window]\ntitle = "Café"\n'.encode("latin-1"))
    @example(data=b"[window]\ntitle = " + b"[" * 2000 + b"]" * 2000 + b"\n")
    @example(data=b"a = " + b"{b = " * 1500 + b"1" + b"}" * 1500 + b"\n")
    @given(st.one_of(
        st.binary(max_size=64),
        st.tuples(user_tomls(), st.integers(0, 2**16), st.binary(min_size=1, max_size=2)).map(
            lambda t: (lambda text: text[:t[1] % (len(text) + 1)] + t[2]
                       + text[t[1] % (len(text) + 1):])(toml_document(t[0])[0].encode("utf-8")))))
    def test_any_bytes_are_refused_or_read(self, data):
        """Whatever the file holds, load_config() reads it or refuses it, and
        the refusal is reported -- report() reads the file again to point at
        the line."""
        self.toml.write_bytes(data)
        with mock.patch.object(configure, "TOML", self.toml), quiet():
            try:
                configure.load_config()
            except configure.ConfigError as refused:
                configure.report(refused)


def config_with(path: tuple, value) -> dict:
    """A valid config but for `path`, which holds `value`."""
    cfg = base_config()
    cfg["deploy"]["itch"].update(user="me", game="game")   # a pair, or neither
    put(cfg, path, value)
    return cfg


class TakenTest(unittest.TestCase):
    """What validate() takes has the shape it says it takes: a pattern it
    checks with is the whole value, not a prefix of it."""

    SHAPES = {
        ("project", "name"): r"[A-Za-z][A-Za-z0-9_-]*",
        ("android", "application_id"): r"[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z][a-zA-Z0-9_]*)+",
        ("ios", "bundle_id"): r"[a-zA-Z][a-zA-Z0-9-]*(\.[a-zA-Z][a-zA-Z0-9-]*)+",
        ("ios", "deployment_target"): r"[0-9]+\.[0-9]+(\.[0-9]+)?",
        ("linux", "glibc"): r"|2\.(1[7-9]|[2-9][0-9])",
        ("icon", "adaptive_background"): r"|#[0-9a-fA-F]{6}",
        ("deploy", "itch", "user"): r"[A-Za-z0-9_-]*",
        ("deploy", "itch", "game"): r"[A-Za-z0-9_-]*",
        ("deploy", "firebase", "project_id"): r"[a-z0-9-]*",
        ("deploy", "firebase", "device"): r"[A-Za-z0-9_.,=-]*",
    }

    # Each shape's value with a newline after it: `$` took every one of them.
    @example(cfg=config_with(("project", "name"), "game\n"))
    @example(cfg=config_with(("android", "application_id"), "com.real.game\n"))
    @example(cfg=config_with(("ios", "bundle_id"), "com.real.game\n"))
    @example(cfg=config_with(("ios", "deployment_target"), "15.6\n"))
    @example(cfg=config_with(("linux", "glibc"), "2.28\n"))
    @example(cfg=config_with(("icon", "adaptive_background"), "#3DDC84\n"))
    @example(cfg=config_with(("deploy", "itch", "user"), "me\n"))
    @example(cfg=config_with(("deploy", "itch", "game"), "game\n"))
    @example(cfg=config_with(("deploy", "firebase", "project_id"), "my-project\n"))
    @example(cfg=config_with(("deploy", "firebase", "device"), "model=x\n"))
    @given(changed_configs())
    def test_what_is_taken_has_its_shape(self, cfg):
        if not taken(cfg):
            return
        for path, shape in self.SHAPES.items():
            value = cfg
            for step in path:
                value = value[step]
            self.assertRegex(str(value), rf"\A(?:{shape})\Z", f"[{'.'.join(path[:-1])}] {path[-1]}")


class BuildFilesTest(unittest.TestCase):
    """Whatever validate() takes reaches every build system as what was written."""

    @example(cfg=dict(base_config(), app={"max_delta": math.nan}))
    @example(cfg=dict(base_config(), ui=dict(base_config()["ui"], scale=math.nan)))
    @example(cfg=config_with(("ui", "scale"), 1e-50))
    @example(cfg=config_with(("input", "deadzone"), 5e-324))
    @given(changed_configs())
    def test_config_h_holds_every_value(self, cfg):
        if not taken(cfg):
            return
        defines = c_defines(generate(cfg)["include/rmp/generated/config.h"])
        for entry in configure.APP_DEFINES:
            if isinstance(entry, str):
                continue
            name, key, render = entry
            value, literal = configure.setting(cfg, key), defines[name]
            if render is configure._c_string:
                self.assertEqual(c_string(literal), value.encode("utf-8"), name)
            elif render is configure._c_float:
                self.assertRegex(literal, rf"\A-?(?:{C_FLOAT.pattern})\Z", name)
                self.assertEqual(float(literal[:-1]), float(value), name)
                single = c_float_of(literal[:-1])
                self.assertIsNotNone(single, f"{name}: C's float cannot hold {literal}")
                self.assertEqual(single == 0, value == 0, f"{name}: C reads {literal} as 0")
            elif render is configure._c_int:
                self.assertRegex(literal, r"\A-?[0-9]+\Z", name)
                self.assertEqual(int(literal), value, name)
            elif render is configure._c_stretch:
                # A mode, by its place in STRETCH_MODES: src/rmp/stretch.cpp
                # reads the number as rmp::app::Stretch.
                self.assertEqual(literal, str(configure.STRETCH_MODES.index(value)), name)
            else:
                self.assertEqual(literal, "1" if value else "0", name)

    @example(cfg=dict(base_config(), window=dict(base_config()["window"], title="Café")))
    @given(changed_configs())
    def test_gradle_reads_back_every_property(self, cfg):
        if not taken(cfg):
            return
        data = generate(cfg)["raymob/generated.properties"].encode("utf-8")
        props = java_properties(data)
        android = cfg["android"]
        expected = {
            "projectName": cfg["project"]["name"], "app.name": cfg["window"]["title"],
            "app.native_library_name": cfg["project"]["name"],
            "app.application_id": android["application_id"],
            "app.min_sdk": str(android["min_sdk"]), "app.category": android["category"],
            "app.orientation": cfg["window"]["orientation"],
            "gl.version": android["gl_version"],
            "admob.app_id": android["admob"]["app_id"],
            "admob.interstitial_id": android["admob"]["interstitial_id"],
            "admob.rewarded_id": android["admob"]["rewarded_id"],
            "gradle.offline": str(android["gradle_offline"]).lower(),
        }
        for key, value in expected.items():
            self.assertEqual(props.get(key), value, key)
        self.assertEqual(set(props), set(self.reference_keys()), "a key that was not written")

    @classmethod
    def reference_keys(cls):
        if not hasattr(cls, "_keys"):
            data = generate(base_config())["raymob/generated.properties"].encode("utf-8")
            cls._keys = sorted(java_properties(data))
        return cls._keys

    @unittest.skipUnless(yaml is not None or IN_BUILD_IMAGE, "PyYAML is not installed")
    @example(cfg=dict(base_config(), window=dict(base_config()["window"], title="true")))
    @example(cfg=dict(base_config(), window=dict(base_config()["window"], title="*x")))
    # No build setting: a table reached Xcode as the text of a Python dict.
    @example(cfg=config_with(("ios", "settings"), {"OTHER_LDFLAGS": {"a": 1}}))
    @example(cfg=config_with(("ios", "settings"), {"WHEN": datetime.date(2026, 10, 11)}))
    # The nightly profile's: `name: NULL` is YAML's null, and `bundleIdPrefix:
    # yes` its true -- names and ids NAME_RE and BUNDLE_RE take.
    @example(cfg=config_with(("project", "name"), "NULL"))
    @example(cfg=config_with(("ios", "bundle_id"), "yes.game"))
    @given(changed_configs())
    def test_xcodegen_reads_back_the_project(self, cfg):
        self.assertIsNotNone(yaml, "PyYAML is missing inside the build image")
        if not taken(cfg):
            return
        spec = yaml.safe_load(generate(cfg)["ios/project.yml"])
        name = cfg["project"]["name"]
        self.assertEqual(spec["name"], name)
        self.assertEqual(list(spec["targets"]), [name])
        self.assertEqual(spec["options"]["bundleIdPrefix"], cfg["ios"]["bundle_id"].rsplit(".", 1)[0])
        base = spec["targets"][name]["settings"]["base"]
        self.assertEqual(base["PRODUCT_NAME"], name)
        self.assertEqual(base["PRODUCT_BUNDLE_IDENTIFIER"], cfg["ios"]["bundle_id"])
        self.assertEqual(base["INFOPLIST_KEY_CFBundleDisplayName"], cfg["window"]["title"])
        self.assertEqual(base["IPHONEOS_DEPLOYMENT_TARGET"] if "IPHONEOS_DEPLOYMENT_TARGET" in base
                         else spec["settings"]["base"]["IPHONEOS_DEPLOYMENT_TARGET"],
                         str(cfg["ios"]["deployment_target"]))
        for key, value in cfg["ios"]["settings"].items():
            if key == "OTHER_CPLUSPLUSFLAGS":
                continue
            self.assertIn(key, base)
            self.assertEqual(as_setting(base[key]), as_setting(value), key)

    @given(changed_configs())
    def test_the_manifest_is_xml(self, cfg):
        if not taken(cfg):
            return
        root = ElementTree.fromstring(generate(cfg)["raymob/app/generated/AndroidManifest.xml"])
        self.assertEqual(root.tag, "manifest")


def as_setting(value):
    """What an Xcode build setting holds of `value`: text, YES or NO, or a
    list of those -- a number is its text, whichever way YAML typed it. A
    table or a date stays what it is, and equals no setting."""
    if isinstance(value, bool):
        return "YES" if value else "NO"
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, list):
        return [as_setting(v) for v in value]
    return value


# A compiler and CMake are started once an example; fewer of them.
FEWER = settings(max_examples=max(10, settings.default.max_examples // 8))


def octal(data: bytes) -> str:
    return '"' + "".join(f"\\{b:03o}" for b in data) + '"'


# What raymob/settings.gradle does with generated.properties --
# `generated.withInputStream { generatedProps.load(it) }` -- and every value
# printed as the hex of its UTF-8, so no console encoding stands in between.
READ_PROPERTIES = """\
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class ReadProperties {
    public static void main(String[] args) throws Exception {
        Properties properties = new Properties();
        try (InputStream in = new FileInputStream(args[0])) { properties.load(in); }
        StringBuilder out = new StringBuilder();
        for (String key : new TreeSet<>(properties.stringPropertyNames())) {
            out.append(hex(key)).append(' ').append(hex(properties.getProperty(key))).append('\\n');
        }
        System.out.print(out);
    }

    static String hex(String text) {
        StringBuilder out = new StringBuilder();
        for (byte b : text.getBytes(StandardCharsets.UTF_8)) out.append(String.format("%02x", b));
        return out.toString();
    }
}
"""


class ToolsReadBackTest(unittest.TestCase):
    """The compiler and CMake themselves, not a reading of them."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.dir = Path(cls._tmp.name)
        cls.cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
        cls.cmake = shutil.which("cmake")
        cls.java = None
        if shutil.which("javac") and shutil.which("java"):
            (cls.dir / "ReadProperties.java").write_text(READ_PROPERTIES, encoding="utf-8")
            built = subprocess.run(["javac", "-d", str(cls.dir), str(cls.dir / "ReadProperties.java")],
                                   capture_output=True, text=True, stdin=subprocess.DEVNULL)
            assert built.returncode == 0, built.stderr
            cls.java = shutil.which("java")

    def read_with_java(self, data: bytes) -> dict[str, str]:
        (self.dir / "generated.properties").write_bytes(data)
        run = subprocess.run([self.java, "-cp", str(self.dir), "ReadProperties",
                              str(self.dir / "generated.properties")],
                             capture_output=True, text=True, stdin=subprocess.DEVNULL)
        self.assertEqual(run.returncode, 0, run.stderr)
        pairs = (line.split(" ") for line in run.stdout.splitlines())
        return {bytes.fromhex(k).decode("utf-8"): bytes.fromhex(v).decode("utf-8") for k, v in pairs}

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    @FEWER
    @example(cfg=dict(base_config(), resources={"rres_password": "a\nb\x00c"}))
    @example(cfg=config_with(("ui", "scale"), 1e-50))
    # The nightly profile's: gcc "trigraph ??/ ignored [-Werror=trigraphs]".
    @example(cfg=config_with(("window", "title"), "??/"))
    @given(changed_configs())
    def test_the_compiler_reads_config_h_as_written(self, cfg):
        if self.cxx is None:
            self.assertFalse(IN_BUILD_IMAGE, "no C++ compiler in the build image")
            self.skipTest("no C++ compiler")
        if not taken(cfg):
            return
        (self.dir / "config.h").write_text(generate(cfg)["include/rmp/generated/config.h"],
                                           encoding="utf-8")
        checks = []
        for entry in configure.APP_DEFINES:
            if isinstance(entry, str):
                continue
            name, key, render = entry
            value = configure.setting(cfg, key)
            if render is configure._c_string:
                data = value.encode("utf-8") + b"\0"
                checks.append(f"static_assert(sizeof({name}) == {len(data)} && "
                              f"same({name}, {octal(data)}, {len(data)}), \"{name}\");")
            elif render is configure._c_float:
                # A use, or the literal is never read: a float the type cannot
                # hold is a warning there, an error under -Werror.
                checks.append(f"static_assert(({name} != 0.0f) == {str(value != 0).lower()}, "
                              f"\"{name}\");")
            elif render is configure._c_stretch:
                checks.append(f"static_assert({name} == "
                              f"{configure.STRETCH_MODES.index(value)}, \"{name}\");")
            else:
                checks.append(f"static_assert({name} == {int(value)}, \"{name}\");")
        (self.dir / "check.cpp").write_text(
            '#include "config.h"\n'
            "constexpr bool same(const char *a, const char *b, unsigned long n) {\n"
            "    for (unsigned long i = 0; i < n; ++i) { if (a[i] != b[i]) return false; }\n"
            "    return true;\n}\n" + "\n".join(checks) + "\n", encoding="utf-8")
        run = subprocess.run([self.cxx, "-std=c++20", "-fsyntax-only", "-Wall", "-Wextra",
                              "-Werror", str(self.dir / "check.cpp")],
                             capture_output=True, text=True, stdin=subprocess.DEVNULL)
        self.assertEqual(run.returncode, 0, run.stderr)

    @FEWER
    @example(cfg=dict(base_config(), window=dict(base_config()["window"], title=" Café 日本 😀\\")))
    @given(changed_configs())
    def test_java_reads_generated_properties_as_written(self, cfg):
        """The JVM itself; the build image has one (amd64, for Android)."""
        if self.java is None:
            self.assertFalse(IN_BUILD_IMAGE and os.uname().machine == "x86_64",
                             "no JDK in the amd64 build image")
            self.skipTest("no JDK")
        if not taken(cfg):
            return
        data = generate(cfg)["raymob/generated.properties"].encode("utf-8")
        props = self.read_with_java(data)
        self.assertEqual(props["app.name"], cfg["window"]["title"])
        self.assertEqual(props, java_properties(data), "the reader BuildFilesTest trusts")

    @FEWER
    @example(cfg=dict(base_config(), resources={"rres_password": "pa${x}ss"}))
    @example(cfg=dict(base_config(), resources={"rres_password": "a\x00b"}))
    @given(changed_configs())
    def test_cmake_reads_project_cmake_as_written(self, cfg):
        if self.cmake is None:
            self.assertFalse(IN_BUILD_IMAGE, "no cmake in the build image")
            self.skipTest("no cmake")
        if not taken(cfg):
            return
        (self.dir / "project.cmake").write_text(generate(cfg)["cmake/generated/project.cmake"],
                                                encoding="utf-8")
        (self.dir / "read.cmake").write_text(
            'include("${CMAKE_CURRENT_LIST_DIR}/project.cmake")\n'
            'file(WRITE "${CMAKE_CURRENT_LIST_DIR}/name.out" "${TEMPLATE_PROJECT_NAME}")\n'
            'file(WRITE "${CMAKE_CURRENT_LIST_DIR}/password.out" "${TEMPLATE_RRES_PASSWORD}")\n',
            encoding="utf-8")
        run = subprocess.run([self.cmake, "-P", str(self.dir / "read.cmake")],
                             capture_output=True, text=True, stdin=subprocess.DEVNULL)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual((self.dir / "name.out").read_bytes(),
                         cfg["project"]["name"].encode("utf-8"))
        self.assertEqual((self.dir / "password.out").read_bytes(),
                         cfg["resources"]["rres_password"].encode("utf-8"))


class EncodersTest(unittest.TestCase):
    """The writers every generated file goes through, for any value."""

    @example(value=math.nan)
    @example(value=math.inf)
    @example(value=1e-50)
    @example(value=1e39)
    @example(value=1e-40)
    @given(st.one_of(st.floats(), st.integers(-2**63, 2**63 - 1)))
    def test_a_float_is_a_c_float_or_refused(self, value):
        """Refused exactly when C's float cannot hold it, or holds 0 for it."""
        single = c_float_of(value)
        lost = single is None or (single == 0) != (value == 0)
        try:
            literal = configure._c_float(value)
        except configure.ConfigError:
            self.assertTrue(lost, f"refused {value!r}, which a float holds as {single!r}")
            return
        self.assertFalse(lost, f"wrote {literal}, which a float holds as {single!r}")
        self.assertRegex(literal, rf"\A-?(?:{C_FLOAT.pattern})\Z")
        self.assertEqual(float(literal[:-1]), float(value))

    @example(value="a\nb")
    @example(value="??/")
    @example(value="???=")
    @given(st.one_of(st.text(), TEXT))
    def test_a_string_is_a_c_string(self, value):
        self.assertEqual(c_string(configure._c_string(value)), value.encode("utf-8"))

    @example(value=" lead")
    @given(st.one_of(st.text(), TEXT))
    def test_a_property_reads_back(self, value):
        props = java_properties(f"k={configure._java_property(value)}\n".encode("utf-8"))
        self.assertEqual(props, {"k": value})

    @unittest.skipUnless(yaml is not None or IN_BUILD_IMAGE, "PyYAML is not installed")
    # PyYAML keeps a raw U+2028 inside quotes, and trims the blanks around it.
    @example(value="line \u2028 sep")
    @given(st.one_of(st.text(), TEXT, st.booleans(), st.integers(),
                     st.lists(st.one_of(TEXT, st.booleans(), st.integers()), max_size=3)))
    def test_a_yaml_scalar_reads_back(self, value):
        self.assertIsNotNone(yaml, "PyYAML is missing inside the build image")
        self.assertEqual(yaml.safe_load(f"k: {configure.yaml_scalar(value)}\n"), {"k": value})


# ---------------------------------------------------------------------------
# The version
# ---------------------------------------------------------------------------

DIGITS = st.one_of(st.text(alphabet="0123456789", min_size=1, max_size=5),
                   st.text(alphabet=st.characters(categories=["Nd"]), min_size=1, max_size=3))
SUFFIX = st.one_of(st.just(""), st.text(alphabet="0123456789abXZ.-+", max_size=5).map(
    lambda s: "-" + s), st.text(alphabet="0123456789abXZ.-+", max_size=5).map(lambda s: "+" + s))
TAG = st.builds(lambda a, b, c, s: f"v{a}.{b}.{c}{s}", DIGITS, DIGITS, DIGITS, SUFFIX)
# Anything but NUL, which no environment variable -- and no git ref -- holds,
# and a lone surrogate, which no environment holds either.
REF_TEXT = st.text(alphabet=st.characters(exclude_characters="\x00", exclude_categories=["Cs"]),
                   max_size=12)
REF = st.one_of(TAG, REF_TEXT, REF_TEXT.map(lambda s: "v" + s))


def version(ref_name: str, ref_type: str = "tag"):
    """resolve_version() under a GitHub ref; None when it is refused."""
    with mock.patch.dict(os.environ, {"GITHUB_REF_TYPE": ref_type, "GITHUB_REF_NAME": ref_name}), \
            quiet():
        try:
            return configure.resolve_version()
        except configure.ConfigError:
            return None


def base_of(name: str) -> str:
    return re.split(r"[-+]", name, maxsplit=1)[0]


class VersionTest(unittest.TestCase):
    """versionName and versionCode, from any ref: a release name in ASCII, a
    code Android takes (1 to Play's ceiling), and no two releases sharing one."""

    @example(ref="v١.٢.٣", kind="tag")
    @example(ref="v0.0.0", kind="tag")
    @given(REF, st.sampled_from(["tag", "branch", ""]))
    def test_any_ref_is_a_version_or_refused(self, ref, kind):
        got = version(ref, kind)
        if got is None:
            self.assertTrue(kind == "tag" and ref.startswith("v"), "only a v-tag is refused")
            return
        name, code = got
        self.assertTrue(name.isascii(), name)
        self.assertIsInstance(code, int)
        self.assertGreaterEqual(code, 1)
        self.assertLessEqual(code, configure.PLAY_MAX_VERSION_CODE)
        if kind == "tag" and ref.startswith("v"):
            self.assertEqual(name, ref[1:])
            self.assertRegex(base_of(name), r"\A(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
        else:
            self.assertEqual((name, code), ("0.0.0-dev", 1))

    @example(a="v01.2.3", b="v1.2.3")
    @example(a="v1.02.3", b="v1.2.3")
    @given(TAG, TAG)
    def test_two_releases_never_share_a_code(self, a, b):
        first, second = version(a), version(b)
        if first is None or second is None:
            return
        if base_of(first[0]) == base_of(second[0]):
            self.assertEqual(first[1], second[1], "a pre-release shares its release's code")
        else:
            self.assertNotEqual(first[1], second[1], f"{a} and {b}")
            ordered = sorted([first, second], key=lambda v: tuple(int(x) for x in base_of(v[0]).split(".")))
            self.assertLess(ordered[0][1], ordered[1][1], "a later release has a larger code")


# ---------------------------------------------------------------------------
# The readers of the .toml, and the target lists
# ---------------------------------------------------------------------------

TOML_LINE = st.one_of(st.sampled_from([
    "[window]", "[android.admob]", "[ deploy.itch ]", "title = 1", "  title=2", "# title = 3",
    "[", "]", "=", "title", "", "\t[ui] # c", "key = [", "[1, 2]", "app_id = '#'",
    "font = \"a # b\" # c", "[[x]]"]), st.text(max_size=20))
TOML_TEXT = st.lists(TOML_LINE, max_size=16).map("\n".join)
SECTION = st.one_of(st.sampled_from(["", "window", "android.admob", "deploy.itch", "ui", "x"]),
                    st.text(max_size=8))
KEY = st.one_of(st.none(), st.sampled_from(["title", "app_id", "font", "key", "1"]),
                st.text(max_size=6))


class ReadersTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()
        cls.toml = Path(cls._tmp.name) / "raylib_multiplatform.toml"

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def point(self, text: str, where):
        self.toml.write_text(text, encoding="utf-8")
        # As TOML counts lines: LF or CRLF, and nothing else -- read_text()
        # takes a lone CR for a line, and splitlines() U+2028 too, which a
        # TOML string or comment holds as it is.
        lines = [line.removesuffix("\r")
                 for line in self.toml.read_bytes().decode("utf-8").split("\n")]
        with mock.patch.object(configure, "TOML", self.toml):
            spot = where()
        if spot is not None:
            number, line = spot
            self.assertTrue(1 <= number <= len(lines), spot)
            self.assertEqual(line, lines[number - 1])
        return spot

    @given(TOML_TEXT, SECTION, KEY)
    def test_locate_points_at_a_line_or_nowhere(self, text, section, key):
        self.point(text, lambda: configure.locate(section, key))

    @given(TOML_TEXT, st.text(max_size=40),
           st.one_of(st.none(), st.tuples(SECTION, KEY)))
    def test_an_error_is_located_or_not_at_all(self, text, message, where):
        self.point(text, lambda: configure.locate_from(configure.ConfigError(message, where)))

    @given(st.text(alphabet="abcdefghij_.", min_size=1, max_size=8).filter(
               lambda s: s.strip(".") == s and ".." not in s),
           st.text(alphabet="abcdefghij_-0123", min_size=1, max_size=8), TOML_TEXT, TOML_TEXT)
    @example(section="window", key="title", before="# a\u2028b\nx = 'c\u0085d'", after="")
    @example(section="_", key="0", before="\r0", after="")   # read_text() made the CR a line
    def test_a_key_written_in_its_section_is_found_on_its_line(self, section, key, before, after):
        """Whatever comes before the section (but another header) and after
        it: the first line that writes the key in it is the one."""
        before = "\n".join(line for line in before.split("\n") if not line.lstrip().startswith("["))
        text = f"{before}\n[{section}]\n{key} = 1\n{after}"
        number = f"{before}\n[{section}]\n".count("\n") + 1
        self.assertEqual(self.point(text, lambda: configure.locate(section, key)),
                         (number, f"{key} = 1"))

    @given(TOML_TEXT)
    def test_comments_are_read_from_any_text(self, text):
        self.toml.write_text(text, encoding="utf-8")
        comments = configure.toml_comments(self.toml)
        self.assertTrue(all(isinstance(k, str) and isinstance(v, str)
                            for k, v in comments.items()))

    @given(st.text(alphabet="abcdefghij_.", min_size=1, max_size=8),
           st.text(alphabet="abcdefghij_-0123", min_size=1, max_size=8),
           # No lone surrogate: a file cannot be written with one (the nightly
           # profile drew '\ud800', and the test died writing it).
           st.text(alphabet=st.characters(exclude_characters="\r\n\x0b\x0c\x1c\x1d\x1e\x85"
                                          "  ", exclude_categories=["Cs"]), max_size=20))
    def test_the_comment_above_a_key_is_its_own(self, section, key, said):
        self.toml.write_text(f"[{section}]\n# {said}\n{key} = 1 # trailing\n", encoding="utf-8")
        comments = configure.toml_comments(self.toml)
        self.assertEqual(comments[f"{section}.{key}"],
                         "\n".join(x for x in (said.rstrip(), "trailing") if x))


class Scratch(unittest.TestCase):
    """A repository of the test's own, emptied before each example, that
    configure.py is pointed at."""

    @classmethod
    def setUpClass(cls):
        cls._tmp = tempfile.TemporaryDirectory()

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def fresh(self) -> Path:
        repo = Path(self._tmp.name) / "repo"
        shutil.rmtree(repo, ignore_errors=True)
        repo.mkdir()
        return repo


FLAG = st.text(alphabet="ABCDEFGHIJ_", min_size=1, max_size=8).map(lambda s: "SUPPORT_" + s)
GUARD_NOISE = st.sampled_from(["", "// a note", "//#define SUPPORT_COMMENTED 1", "#if defined(X)",
                               "#endif", "    ", "#undef SUPPORT_X"])


class RepoFilesTest(Scratch):
    """What configure.py reads from the repository, from any such file."""

    def config_h(self, repo: Path, text: str) -> None:
        header = repo / "thirdparty" / "raylib" / "src" / "config.h"
        header.parent.mkdir(parents=True)
        header.write_text(text)

    @given(st.dictionaries(FLAG, st.integers(-5, 70000), max_size=5), st.lists(GUARD_NOISE, max_size=4),
           st.sampled_from(["none", "no value", "hex", "missing module", "no guard"]))
    def test_raylib_flags_are_the_defines_inside_the_guard(self, flags, noise, broken):
        flags = dict(flags, **{name: 1 for name in configure.MODULE_FLAG.values()})
        inside = [f"#define {k} {v}" + (" // why" if v % 2 else "") for k, v in flags.items()]
        if broken == "no value":
            inside.append("#define SUPPORT_EMPTY")
        if broken == "hex":
            inside.append("#define SUPPORT_HEX 0x10")
        if broken == "missing module":
            gone = sorted(configure.MODULE_FLAG.values())[0]
            inside = [line for line in inside if not line.startswith(f"#define {gone} ")]
            del flags[gone]
        guard = [] if broken == "no guard" else ["#if !defined(EXTERNAL_CONFIG_FLAGS)"]
        end = [] if broken == "no guard" else ["#endif // !EXTERNAL_CONFIG_FLAGS"]
        text = "\n".join(["#define OUTSIDE_BEFORE 7", *guard, *noise, *inside, *end,
                          "#define OUTSIDE_AFTER 9", ""])
        repo = self.fresh()
        self.config_h(repo, text)
        with mock.patch.object(configure, "REPO", repo):
            try:
                got = configure.parse_raylib_config()
            except configure.ConfigError as refused:
                says = {"no guard": "could not find the EXTERNAL_CONFIG_FLAGS guard",
                        "no value": "SUPPORT_EMPTY", "hex": "SUPPORT_HEX",
                        "missing module": "raylib's layout changed"}[broken]
                self.assertIn(says, str(refused))
                return
        self.assertEqual(broken, "none", "it took what it has to refuse")
        self.assertEqual(got, flags)

    @given(st.lists(st.sampled_from(sorted(configure.OPTIONAL_MODULES)), unique=True), st.booleans())
    def test_the_raylib_flags_file_has_every_flag_and_the_stubs_it_needs(self, disabled, stale):
        repo = self.fresh()
        flags = {name: 1 for name in configure.MODULE_FLAG.values()} | {"SUPPORT_OTHER": 3}
        self.config_h(repo, "#if !defined(EXTERNAL_CONFIG_FLAGS)\n"
                      + "".join(f"#define {k} {v}\n" for k, v in flags.items())
                      + "#endif // !EXTERNAL_CONFIG_FLAGS\n")
        stub = repo / "cmake" / "generated" / "module_stubs.c"
        if stale:
            stub.parent.mkdir(parents=True)
            stub.write_text("old")
        cfg = base_config()
        cfg["raylib"]["disabled_modules"] = list(disabled)
        with mock.patch.object(configure, "REPO", repo):
            configure.gen_raylib_flags(cfg)
        cmake = (repo / "cmake" / "generated" / "raylib_flags.cmake").read_text()
        values = dict(flags, **{configure.MODULE_FLAG[m]: 0 for m in disabled})
        listed = re.findall(r"^    (\S+)$", cmake, re.M)
        self.assertEqual(listed, ["EXTERNAL_CONFIG_FLAGS"] + [f"{k}={v}" for k, v in sorted(values.items())])
        self.assertIn("set(RAYLIB_CONFIG_DEFS\n", cmake)
        stubs = "raudio" in disabled
        self.assertIn('set(RAYLIB_STUB_SOURCES "{}")'.format(
            "${CMAKE_CURRENT_LIST_DIR}/module_stubs.c" if stubs else ""), cmake)
        self.assertEqual(stub.is_file(), stubs)
        if stubs:
            self.assertIn("bool IsAudioDeviceReady(void) { return false; }", stub.read_text())

    @given(st.dictionaries(st.text(alphabet="abcdef_0123", min_size=1, max_size=8),
                           st.text(alphabet="abc 0123:/@._-", min_size=1, max_size=16)
                           .map(str.strip).filter(bool), max_size=5),
           st.lists(st.sampled_from(["", "# a comment", "  ", "lonely"]), max_size=3))
    def test_the_pins_are_the_versions_block_and_only_it(self, pins, noise):
        repo = self.fresh()
        (repo / "thirdparty").mkdir()
        lines = ["outside value", "```versions", *noise, *(f"{k}   {v}" for k, v in pins.items()),
                 "```", "after value"]
        (repo / "thirdparty" / "FROZEN_VERSIONS.md").write_text("\n".join(lines) + "\n")
        with mock.patch.object(configure, "REPO", repo):
            if not pins:
                with self.assertRaises(configure.ConfigError) as refused:
                    configure.frozen_versions()
                self.assertIn("no ```versions block found", str(refused.exception))
                return
            self.assertEqual(configure.frozen_versions(), pins)

    @given(st.integers(0, 2), st.integers(0, 2), st.booleans(), st.booleans(), st.booleans(),
           st.lists(st.sampled_from(["name: CI", "on:", "  workflow_dispatch:", "jobs: {}", ""]),
                    max_size=3))
    def test_ci_yml_gets_the_triggers_on_push_writes_or_a_refusal(self, begins, ends, reversed_,
                                                                 on_push, write_it, outside):
        repo = self.fresh()
        workflow = repo / configure.CI_WORKFLOW
        begin, end = f"  {configure.CI_BEGIN}: edit", f"  {configure.CI_END}"
        markers = [begin] * begins + ["  push: old"] + [end] * ends
        if reversed_ and begins == ends == 1:
            markers = [end, "  push: old", begin]
        text = "\n".join(outside + markers + ["jobs:", "  x: {}"]) + "\n"
        cfg = base_config()
        cfg["ci"]["on_push"] = on_push
        with mock.patch.object(configure, "REPO", repo), quiet():
            self.assertFalse(configure.sync_ci_triggers(cfg, write_it), "no ci.yml, nothing to sync")
            workflow.parent.mkdir(parents=True)
            workflow.write_text(text)
            try:
                wrote = configure.sync_ci_triggers(cfg, write_it)
            except configure.ConfigError as refused:
                once_each = begins == ends == 1 and not reversed_
                self.assertIn("are not the ones it writes" if once_each else "between two lines",
                              str(refused))
                self.assertEqual(refused.where, ("ci", "on_push"))
                self.assertEqual(workflow.read_text(), text, "a refusal writes nothing")
                return
            want = ("\n".join(outside) + ("\n" if outside else "")
                    + configure.ci_trigger_block(cfg) + "jobs:\n  x: {}\n")
            self.assertTrue(wrote)
            self.assertTrue(write_it)
            self.assertEqual(workflow.read_text(), want)
            self.assertFalse(configure.sync_ci_triggers(cfg, False), "written, it agrees")

    @unittest.skipUnless(yaml is not None or IN_BUILD_IMAGE, "PyYAML is not installed")
    @given(st.booleans())
    def test_the_triggers_are_what_on_push_says(self, on_push):
        self.assertIsNotNone(yaml, "PyYAML is missing inside the build image")
        cfg = base_config()
        cfg["ci"]["on_push"] = on_push
        block = configure.ci_trigger_block(cfg)
        self.assertTrue(block.startswith(f"  {configure.CI_BEGIN}"))
        self.assertTrue(block.endswith(f"  {configure.CI_END}\n"))
        triggers = yaml.safe_load("triggers:\n" + block)["triggers"]
        ignore = ["**.md", "LICENSE"]
        self.assertEqual(triggers, {"push": {"branches": ["main"], "tags": ["v*"],
                                             "paths-ignore": ignore},
                                    "pull_request": None} if on_push
                         else {"push": {"tags": ["v*"]}})


COMMENT = st.text(alphabet=st.characters(exclude_characters="\r\n\x0b\x0c\x1c\x1d\x1e\x85"
                                         "\u2028\u2029", exclude_categories=["Cs"]), max_size=12)


@st.composite
def documents(draw) -> tuple[str, dict]:
    """A .toml with comments where the framework's has them -- above a
    section, above a key, above a run of keys, at the end of a line -- and
    the comment toml_comments() owes each name."""
    lines, owed = [], {}
    for section in draw(st.lists(st.text(alphabet="abcdefgh_.", min_size=1, max_size=6),
                                 unique=True, max_size=3)):
        above = draw(st.lists(COMMENT, max_size=2))
        lines += [f"# {c}" for c in above] + [f"[{section}]"]
        owed[section] = "\n".join(c.rstrip() for c in above).strip()
        keys = draw(st.lists(st.text(alphabet="abcxyz_-019", min_size=1, max_size=5), unique=True,
                             max_size=5))
        run = ""
        for key in keys:
            if draw(st.booleans()):
                lines.append("")
                run = ""
            said = draw(st.lists(COMMENT, max_size=2))
            lines += [f"# {c}" for c in said]
            if said:
                run = "\n".join(c.rstrip() for c in said).strip("\n")
            value = draw(st.sampled_from(["1", '"a#b"', "'x#y'", "[1, 2]"]))
            trailing = draw(st.one_of(st.just(""), COMMENT.filter(lambda c: "#" not in c)))
            lines.append(f"{key} = {value}" + (f"  # {trailing}" if trailing else ""))
            owed[f"{section}.{key}"] = "\n".join(x for x in (run, trailing.strip()) if x)
        if draw(st.booleans()):   # a section may end right where the next begins
            lines.append("")
    return "\n".join(lines), owed


class CommentsTest(Scratch):

    @given(documents())
    def test_each_name_has_the_comment_written_for_it(self, document):
        text, owed = document
        path = self.fresh() / "raylib_multiplatform.toml"
        path.write_text(text, encoding="utf-8")
        self.assertEqual(configure.toml_comments(path), owed, text)


# A manifest template, as raymob/app/AndroidManifest.template.xml writes one.
TEMPLATE_LINE = st.one_of(
    st.sampled_from(["<manifest>", "  <uses-permission a='x'/>", "</manifest>", "", "  ",
                     "<!-- #if internet -->", "<!-- #if vibration -->", "<!-- #if admob -->",
                     "  <!--   #if   internet   -->", "<!-- #endif -->", "<!-- #if nothing -->",
                     "<!-- #doc -->", "<!-- #enddoc -->", "  explained here"]),
    st.text(alphabet="<>/ abcxyz='", max_size=12))
PLAIN = st.sampled_from(["  <uses-permission a='x'/>", "  <application/>", "", "  "])
# Mostly well formed -- a block per flag, a #doc block, plain lines -- and now
# and then a line of anything, a stray directive among them.
SEGMENT = st.one_of(
    PLAIN.map(lambda line: [line]),
    st.tuples(st.sampled_from(["internet", "vibration", "admob"]), st.lists(PLAIN, max_size=3))
    .map(lambda b: [f"<!-- #if {b[0]} -->", *b[1], "<!-- #endif -->"]),
    st.lists(PLAIN, max_size=2).map(lambda d: ["<!-- #doc -->", "  why", *d, "<!-- #enddoc -->"]),
    TEMPLATE_LINE.map(lambda line: [line]))
TEMPLATES = st.lists(SEGMENT, max_size=8).map(lambda segments: [x for s in segments for x in s])


def manifest_from(template: list[str], flags: dict) -> list[str] | str:
    """What gen_android_manifest() owes a template: its lines, a block kept or
    dropped by its flag, the #doc blocks gone, no blank line twice or first,
    the notice after the XML declaration -- or the phrase its refusal says."""
    out, keeping, active, in_doc = [], True, None, False
    for line in template:
        if re.fullmatch(r"\s*<!--\s*#doc\s*-->\s*", line):
            in_doc = True
            continue
        if re.fullmatch(r"\s*<!--\s*#enddoc\s*-->\s*", line):
            in_doc = False
            continue
        if in_doc:
            continue
        m = re.fullmatch(r"\s*<!--\s*#if\s+([a-z_]+)\s*-->\s*", line)
        if m:
            if active is not None:
                return "nested #if"
            if m.group(1) not in flags:
                return "unknown flag"
            active, keeping = m.group(1), flags[m.group(1)]
            continue
        if re.fullmatch(r"\s*<!--\s*#endif\s*-->\s*", line):
            if active is None:
                return "#endif without #if"
            active, keeping = None, True
            continue
        if keeping:
            out.append(line)
    if active is not None:
        return "was never closed"
    if in_doc:
        return "#doc was never closed"
    tidy = []
    for line in out:
        if line.strip() or (tidy and tidy[-1].strip()):
            tidy.append(line)
    notice = f"<!-- {configure.GEN_HEADER} -->"
    tidy.insert(1 if tidy and tidy[0].startswith("<?xml") else 0, notice)
    return "\n".join(tidy).rstrip().split("\n")


class GeneratedTest(Scratch):
    """The generators, for any input they read besides the .toml."""

    @given(TEMPLATES, st.booleans(), st.booleans(), st.booleans(),
           st.booleans(), st.sampled_from([["android"], ["web"]]))
    def test_the_manifest_is_its_template_with_each_block_decided(self, lines, xml, internet,
                                                                  vibration, admob, targets):
        template = (['<?xml version="1.0" encoding="utf-8"?>'] if xml else []) + lines
        path = self.fresh() / "AndroidManifest.template.xml"
        path.write_text("\n".join(template) + "\n")
        cfg = base_config()
        cfg["android"]["permissions"].update(internet=internet, vibration=vibration)
        cfg["android"]["admob"]["enabled"] = admob
        on = admob and "android" in targets
        flags = {"internet": internet or on, "vibration": vibration, "admob": on}
        captured = {}
        with mock.patch.object(configure, "MANIFEST_TEMPLATE", path), \
                mock.patch.object(configure, "write", lambda p, c: captured.__setitem__("m", c)):
            try:
                configure.gen_android_manifest(cfg, targets)
            except configure.ConfigError as refused:
                expected = manifest_from(template, flags)
                self.assertIsInstance(expected, str, f"refused what it takes: {refused}")
                self.assertIn(expected, str(refused))
                self.assertIn(path.name, str(refused))
                return
        expected = manifest_from(template, flags)
        self.assertIsInstance(expected, list, f"took what it refuses: {expected}")
        self.assertEqual(captured["m"], "\n".join(expected) + "\n")

    def notice_rows(self, draw, repo: Path):
        """Rows of the record and a component on disk for each: a LICENSE, a
        notice nowhere (part of another, upstream only), or none at all."""
        rows, notices = [], {}
        for i, kind in enumerate(draw(st.lists(st.sampled_from(
                ["file", "file", "part-of", "upstream", "missing"]), max_size=4))):
            name = "raylib_multiplatform" if draw(st.booleans()) and i == 0 else f"dep{i}"
            licences = draw(st.sampled_from(["MIT", "zlib", "MIT|Apache-2.0"]))
            elect = "-" if "|" not in licences else draw(st.sampled_from(licences.split("|")))
            evidence = {"file": "file", "missing": "file", "part-of": "part-of:dep0",
                        "upstream": "upstream:https://example.org/x"}[kind]
            folder = repo / "thirdparty" / f"c{i}"
            folder.mkdir(parents=True)
            if kind == "file":
                notices[name] = f"Notice of {name}, number {i}."
                (folder / "LICENSE").write_text(notices[name] + "\n\n")
            rows.append(configure.license_db.Row(
                name=name, path=f"thirdparty/c{i}", licences=licences, elect=elect,
                modified=draw(st.sampled_from(["no", "yes", "subset"])),
                linked=draw(st.sampled_from(["all", "desktop", "web", "android|ios", "none"])),
                evidence=evidence))
        return rows, notices

    @given(st.data(), st.sampled_from(["linux", "windows", "apple", "bsd", "web", "android", "ios"]),
           st.text(alphabet="abcxyz_", min_size=1, max_size=10))
    def test_licenses_txt_credits_what_reaches_its_family(self, data, family, project):
        repo = self.fresh()
        rows, notices = self.notice_rows(data.draw, repo)
        cfg = base_config()
        cfg["project"]["name"] = project
        reaching = configure.license_db.rows_for(rows, family)
        missing = [row["path"] for row in reaching
                   if row["evidence"] == "file" and row["name"] not in notices]
        with mock.patch.object(configure, "REPO", repo):
            try:
                text = configure.licenses_text(cfg, family, rows)
            except configure.ConfigError as refused:
                self.assertTrue(missing, f"refused a record it can write: {refused}")
                self.assertIn("they cannot go into LICENSES.txt: " + ", ".join(missing),
                              str(refused))
                return
        self.assertFalse(missing, "wrote a notice with a component's text missing")
        lines = text.split("\n")
        self.assertEqual(lines[:2], [f"Third-party licences for {project}", "=" * (30 + len(project))])
        self.assertTrue(text.endswith("\n"))
        for row in rows:
            licence = row["licences"].replace("|", " or ")
            if row["elect"] != "-":
                licence += f" (this build takes the {row['elect']} alternative)"
            heading = f"  {row['name']} -- {licence}"
            shipped = row in reaching and not row["evidence"].startswith("part-of:")
            self.assertEqual(heading in lines, shipped, heading)
            if not shipped:
                continue
            at = lines.index(heading)
            mark = ("The framework this game is built on: src/rmp/ and include/rmp/ in the source."
                    if row["name"] == "raylib_multiplatform" else
                    f"MODIFIED for this build. See thirdparty/{Path(row['path']).name}/PATCHES.md "
                    "in the source." if row["modified"] == "yes" else
                    f"PARTIAL copy. See thirdparty/{Path(row['path']).name}/PATCHES.md in the "
                    "source." if row["modified"] == "subset" else "Unmodified copy.")
            notice = notices.get(row["name"]) or (
                f"No notice travels with this component: it is {row.elected}, under which none "
                "is owed. Upstream: https://example.org/x")
            self.assertEqual(lines[at - 1:at + 5], ["-" * 70, heading, f"  {mark}", "-" * 70, "",
                                                    notice])

    @given(TOML_TEXT)
    def test_a_toml_that_does_not_parse_is_refused_saying_so(self, text):
        toml = self.fresh() / "raylib_multiplatform.toml"
        toml.write_text(text, encoding="utf-8")
        try:
            tomllib.loads(text)
            parses = True
        except tomllib.TOMLDecodeError:
            parses = False
        with mock.patch.object(configure, "TOML", toml), quiet():
            try:
                configure.load_config()
            except configure.ConfigError as refused:
                self.assertEqual("is not valid TOML" in str(refused), not parses, str(refused))
                return
        self.assertTrue(parses)

    @given(st.just(None))
    def test_no_toml_is_the_defaults_and_a_warning(self, _):
        gone = self.fresh() / "raylib_multiplatform.toml"
        err = io.StringIO()
        with mock.patch.object(configure, "TOML", gone), contextlib.redirect_stderr(err):
            cfg = configure.load_config()
        self.assertEqual(cfg, configure.DEFAULTS)
        cfg["window"]["title"] = "changed"
        self.assertNotEqual(configure.DEFAULTS["window"]["title"], "changed", "a copy, not DEFAULTS")
        self.assertIn("raylib_multiplatform.toml not found", err.getvalue())

    @given(changed_configs())
    def test_print_defines_is_the_header_as_a_table(self, cfg):
        if not taken(cfg):
            return
        table = configure.app_defines(cfg)
        entries = [e for e in configure.APP_DEFINES if not isinstance(e, str)]
        self.assertEqual([(row["name"], row["key"]) for row in table],
                         [(name, key) for name, key, _ in entries])
        defines = c_defines(generate(cfg)["include/rmp/generated/config.h"])
        self.assertEqual({row["name"]: row["value"] for row in table}, defines)


TARGET_NAMES = sorted(set(configure.GROUPS) | set(configure.TARGETS))
ENABLED = st.lists(st.sampled_from(list(configure.TARGETS)), min_size=1, unique=True).map(
    lambda ts: [t for t in configure.TARGETS if t in ts])


class TargetsTest(unittest.TestCase):

    @given(st.lists(st.one_of(st.sampled_from(TARGET_NAMES), st.text(max_size=5)), max_size=5),
           st.sampled_from([",", ", ", " ,", ",,"]), ENABLED)
    def test_only_narrows_or_refuses(self, names, comma, targets):
        value = comma.join(names)
        try:
            got = configure.narrow_to(value, targets)
        except configure.ConfigError as refused:
            self.assertIn(self.why_not(value, targets), str(refused))
            return
        self.assertTrue(got)
        self.assertEqual(got, [t for t in targets if t in got])
        asked = [name.strip() for name in value.split(",")]
        self.assertEqual(len(asked), len(set(asked)), "a name given twice is refused")
        named = set()
        for name in value.split(","):
            named |= set(configure.GROUPS.get(name.strip(), [name.strip()]))
        self.assertEqual(set(got), named & set(targets))

    @staticmethod
    def why_not(value: str, targets: list[str]) -> str:
        """What narrow_to() has to say about a value it refuses: the first
        item that is wrong, in the order the items are read."""
        if not value.strip():
            return "--only was given an empty value"
        seen = set()
        for raw in value.split(","):
            name = raw.strip()
            if not name:
                return "is empty -- a comma too many"
            if name in seen:
                return f"{name!r} is named twice"
            seen.add(name)
            group = configure.GROUPS.get(name, [name] if name in configure.TARGETS else None)
            if group is None:
                return ("The names are separated by commas, not spaces" if len(name.split()) > 1
                        else f"--only {name!r} is not a target or a group")
            if not set(group) & set(targets):
                return f"--only {name!r} leaves nothing to build"
        raise AssertionError(f"{value!r} should have been taken")

    @given(st.lists(st.one_of(st.sampled_from(TARGET_NAMES), TEXT), max_size=4),
           st.lists(st.one_of(st.sampled_from(TARGET_NAMES), TEXT), max_size=4))
    def test_targets_expand_or_refuse(self, enabled, disabled):
        try:
            got = configure.expand_targets(enabled, disabled)
        except configure.ConfigError as refused:
            known = set(configure.GROUPS) | set(configure.TARGETS)
            first = next((("enabled", n) for n in enabled if n not in known),
                         next((("disabled", n) for n in disabled if n not in known), None))
            self.assertIn(f"[targets] {first[0]}: unknown target or group {first[1]!r}" if first
                          else "[targets]: nothing left to build", str(refused))
            return

        def resolved(names):
            return set().union(*(set(configure.GROUPS.get(n, [n])) for n in names))
        self.assertEqual(got, [t for t in configure.TARGETS if t in resolved(enabled) - resolved(disabled)])
        self.assertTrue(got)

    @given(st.lists(st.sampled_from(sorted(set(configure.UPX_GROUPS) | set(configure.UPX_TARGETS)
                                           | set(configure.UPX_REFUSED)) + ["x"]), max_size=4),
           st.lists(st.sampled_from(sorted(set(configure.UPX_GROUPS) | set(configure.UPX_TARGETS))),
                    max_size=3), ENABLED)
    def test_upx_expands_to_what_is_built_or_refuses(self, enabled, disabled, targets):
        cfg = {"upx": {"enabled": enabled, "disabled": disabled}}
        try:
            got = configure.expand_upx(cfg, targets)
        except configure.ConfigError as refused:
            first = next(n for n in enabled
                         if n not in configure.UPX_GROUPS and n not in configure.UPX_TARGETS)
            self.assertIn(f"{first!r} cannot be compressed" if first in configure.UPX_REFUSED
                          else f"unknown target or group {first!r}", str(refused))
            return

        def resolved(names):
            return set().union(*(set(configure.UPX_GROUPS.get(n, [n])) for n in names))
        self.assertEqual(got, [t for t in configure.UPX_TARGETS
                               if t in (resolved(enabled) - resolved(disabled)) & set(targets)])


if __name__ == "__main__":
    unittest.main()
