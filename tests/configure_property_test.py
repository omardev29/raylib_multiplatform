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
import re
import shutil
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


def is_table(path: tuple, value) -> bool:
    """A table the schema has, written as a [section] -- every other table is
    a value, written inline."""
    if not isinstance(value, dict):
        return False
    node = configure.DEFAULTS
    for step in path:
        if not isinstance(node, dict) or step not in node:
            return len(path) == 1   # an unknown top-level table: [name]
        node = node[step]
    return isinstance(node, dict)


def toml_document(user: dict) -> tuple[str, dict]:
    """The .toml a person writes for `user`: values above every section, then a
    [section] per table with its keys under it. And the line of every key,
    as (section, key) -> line, and of every header, as (section, None)."""
    lines: list[str] = []
    where: dict = {}

    def table(path: tuple, node: dict):
        name = ".".join(path)
        if path:
            lines.append(f"[{'.'.join(toml_key(p) for p in path)}]")
            where[(name, None)] = len(lines)
        subtables = []
        for key, value in node.items():
            if is_table(path + (key,), value):
                subtables.append(key)
                continue
            lines.append(f"{toml_key(key)} = {toml_value(value)}")
            where[(name, key)] = len(lines)
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
        SETTING, st.one_of(TEXT, st.booleans(), st.integers(-5, 5), st.lists(TEXT, max_size=3)),
        max_size=3))
    return cfg


def taken(cfg: dict) -> bool:
    """validate() takes it. A refusal is R7's to check (TotalTest)."""
    try:
        with quiet():
            configure.validate(cfg, False)
    except configure.ConfigError:
        return False
    return True


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

    def run_toml(self, user: dict, strict: bool):
        text, where = toml_document(user)
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

    @example(user={"zzz": 1})
    @example(user={"app": {"max_delta": math.nan}})
    @given(user_tomls())
    def test_any_toml_is_refused_at_its_line_or_taken(self, user):
        refused, spot, where, text = self.run_toml(user, strict=False)
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

    @given(user_tomls())
    def test_a_release_check_is_total_too(self, user):
        self.run_toml(user, strict=True)


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

    @example(cfg=dict(base_config(), project={"name": "game\n"}))
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
            with self.subTest(name=name):
                if render is configure._c_string:
                    self.assertEqual(c_string(literal), value.encode("utf-8"))
                elif render is configure._c_float:
                    self.assertRegex(literal, rf"\A-?(?:{C_FLOAT.pattern})\Z")
                    self.assertEqual(float(literal[:-1]), float(value))
                elif render is configure._c_int:
                    self.assertRegex(literal, r"\A-?[0-9]+\Z")
                    self.assertEqual(int(literal), value)
                else:
                    self.assertEqual(literal, "1" if value else "0")

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
    @given(changed_configs())
    def test_xcodegen_reads_back_the_project(self, cfg):
        self.assertIsNotNone(yaml, "PyYAML is missing inside the build image")
        if not taken(cfg):
            return
        spec = yaml.safe_load(generate(cfg)["ios/project.yml"])
        name = cfg["project"]["name"]
        self.assertEqual(spec["name"], name)
        base = spec["targets"][name]["settings"]["base"]
        self.assertEqual(base["PRODUCT_BUNDLE_IDENTIFIER"], cfg["ios"]["bundle_id"])
        self.assertEqual(base["INFOPLIST_KEY_CFBundleDisplayName"], cfg["window"]["title"])
        self.assertEqual(base["IPHONEOS_DEPLOYMENT_TARGET"] if "IPHONEOS_DEPLOYMENT_TARGET" in base
                         else spec["settings"]["base"]["IPHONEOS_DEPLOYMENT_TARGET"],
                         str(cfg["ios"]["deployment_target"]))
        for key, value in cfg["ios"]["settings"].items():
            if key == "OTHER_CPLUSPLUSFLAGS":
                continue
            with self.subTest(setting=key):
                self.assertIn(key, base)
                if isinstance(value, bool):
                    self.assertIs(base[key], value)
                elif isinstance(value, (str, list)):
                    self.assertEqual(base[key], value)

    @given(changed_configs())
    def test_the_manifest_is_xml(self, cfg):
        if not taken(cfg):
            return
        root = ElementTree.fromstring(generate(cfg)["raymob/app/generated/AndroidManifest.xml"])
        self.assertEqual(root.tag, "manifest")


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
            if isinstance(entry, str) or entry[2] is not configure._c_string:
                continue
            name, key, _ = entry
            data = configure.setting(cfg, key).encode("utf-8") + b"\0"
            checks.append(f"static_assert(sizeof({name}) == {len(data)} && "
                          f"same({name}, {octal(data)}, {len(data)}), \"{name}\");")
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
    @given(st.one_of(st.floats(), st.integers(-2**63, 2**63 - 1)))
    def test_a_float_is_a_c_float_or_refused(self, value):
        try:
            literal = configure._c_float(value)
        except configure.ConfigError:
            self.assertFalse(math.isfinite(float(value)))
            return
        self.assertRegex(literal, rf"\A-?(?:{C_FLOAT.pattern})\Z")
        self.assertEqual(float(literal[:-1]), float(value))

    @example(value="a\nb")
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
# Anything but NUL, which no environment variable -- and no git ref -- holds.
REF_TEXT = st.text(alphabet=st.characters(exclude_characters="\x00"), max_size=12)
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
        lines = self.toml.read_text(encoding="utf-8").splitlines()
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
    def test_a_key_written_in_its_section_is_found_on_its_line(self, section, key, before, after):
        """Whatever comes before the section (but another header) and after
        it: the first line that writes the key in it is the one."""
        before = "\n".join(line for line in before.splitlines() if not line.lstrip().startswith("["))
        text = f"{before}\n[{section}]\n{key} = 1\n{after}"
        number = len(f"{before}\n[{section}]\n".splitlines()) + 1
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
           st.text(alphabet=st.characters(exclude_characters="\r\n\x0b\x0c\x1c\x1d\x1e\x85"
                                          "  "), max_size=20))
    def test_the_comment_above_a_key_is_its_own(self, section, key, said):
        self.toml.write_text(f"[{section}]\n# {said}\n{key} = 1 # trailing\n", encoding="utf-8")
        comments = configure.toml_comments(self.toml)
        self.assertEqual(comments[f"{section}.{key}"],
                         "\n".join(x for x in (said.rstrip(), "trailing") if x))


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
        except configure.ConfigError:
            return
        self.assertTrue(got)
        self.assertEqual(got, [t for t in targets if t in got])
        asked = [name.strip() for name in value.split(",")]
        self.assertEqual(len(asked), len(set(asked)), "a name given twice is refused")
        named = set()
        for name in value.split(","):
            named |= set(configure.GROUPS.get(name.strip(), [name.strip()]))
        self.assertEqual(set(got), named & set(targets))

    @given(st.lists(st.one_of(st.sampled_from(TARGET_NAMES), TEXT), max_size=4),
           st.lists(st.one_of(st.sampled_from(TARGET_NAMES), TEXT), max_size=4))
    def test_targets_expand_or_refuse(self, enabled, disabled):
        try:
            got = configure.expand_targets(enabled, disabled)
        except configure.ConfigError:
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
        except configure.ConfigError:
            self.assertTrue(set(enabled) - set(configure.UPX_GROUPS) - set(configure.UPX_TARGETS))
            return

        def resolved(names):
            return set().union(*(set(configure.UPX_GROUPS.get(n, [n])) for n in names))
        self.assertEqual(got, [t for t in configure.UPX_TARGETS
                               if t in (resolved(enabled) - resolved(disabled)) & set(targets)])


if __name__ == "__main__":
    unittest.main()
