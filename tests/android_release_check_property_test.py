"""tools/android_release_check.py, the Android job's proof that the release
variant was compiled as a release, held to its docstring for ANY compile
database: what it says is the rule, and nothing it reads is a traceback.

AGP writes the databases; a build stopped halfway writes half of one. The
check's word on that must be FAIL and why -- exit 1 -- not a JSONDecodeError
in the job's log. And for databases that are well formed, the verdict is the
rule the docstring states, computed here from a model of each entry (whose
file it is, which flags it was given, as `arguments` or as one `command`)
rather than from the strings the check reads.

Every property was seen red; the commit that added this file says how, and
each counterexample is a named case in tests/gates_red_test.py.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import shlex
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hypothesis_profiles  # noqa: E402  (first: the profile, or the skip)
from hypothesis import given, strategies as st  # noqa: E402

# As `tools.android_release_check`: the name mutmut records hits under.
_spec = importlib.util.spec_from_file_location(
    "tools.android_release_check",
    hypothesis_profiles.REPO / "tools" / "android_release_check.py")
arc = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = arc
_spec.loader.exec_module(arc)

VARIANTS = ["RelWithDebInfo", "Release", "MinSizeRel", "Debug"]
KINDS = {
    "game": ["/w/src/scenes/main_menu.cpp", "/w/src/rmp/app.cpp", "C:\\w\\src\\main.cpp"],
    "raylib": ["/w/thirdparty/raylib/src/rcore.c", "/w/thirdparty/raylib/src/rtext.c"],
    "other": ["/w/thirdparty/clay/clay_impl.cpp", "/w/raymob/native_app_glue.c",
              "/w/thirdparty/src/vendored.c"],
}
FLAGS = ["-DRMP_PRODUCTION_BUILD=1", "-O0", "-O2", "-O3", "-D_DEBUG", "-DDEBUG", "-DNDEBUG",
         "-g", "-DRMP_PRODUCTION_BUILD=0"]


@st.composite
def entry(draw):
    kind = draw(st.sampled_from(sorted(KINDS)))
    # Half of them the flags a release has, so that a database that passes is
    # as common as one that fails.
    if draw(st.booleans()):
        flags = ["-DRMP_PRODUCTION_BUILD=1", *draw(st.lists(
            st.sampled_from(["-O2", "-O3", "-g", "-DNDEBUG"]), max_size=3))]
    else:
        flags = draw(st.lists(st.sampled_from(FLAGS), max_size=5))
    form = draw(st.sampled_from(["arguments", "command"]))
    return {"kind": kind, "file": draw(st.sampled_from(KINDS[kind])), "flags": flags,
            "form": form}


DATABASES = st.lists(st.tuples(st.sampled_from(VARIANTS), st.lists(entry(), max_size=4)),
                     max_size=3)


def written(e) -> dict:
    """One entry as AGP writes it."""
    argv = ["clang++", *e["flags"], "-c", "x.cpp"]
    out = {"directory": "/b", "file": e["file"]}
    if e["form"] == "arguments":
        out["arguments"] = argv
    else:
        out["command"] = shlex.join(argv)
    return out


def expected(databases) -> int:
    """The docstring's rule, over the model: every file of the game and the
    framework in a release database has RMP_PRODUCTION_BUILD=1, no -O0 and
    no _DEBUG/DEBUG; raylib's no -O0; and there is at least one of ours."""
    release = [entries for variant, entries in databases if variant != "Debug"]
    if not release:
        return 1
    ours = [e for entries in release for e in entries if e["kind"] == "game"]
    if not ours:
        return 1
    for entries in release:
        for e in entries:
            if e["kind"] == "game" and ("-DRMP_PRODUCTION_BUILD=1" not in e["flags"]
                                        or {"-O0", "-D_DEBUG", "-DDEBUG"} & set(e["flags"])):
                return 1
            if e["kind"] == "raylib" and "-O0" in e["flags"]:
                return 1
    return 0


def check(root: Path):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = arc.main(["android_release_check.py", str(root)])
    return code, out.getvalue()


def lay_out(root: Path, databases) -> None:
    for i, (variant, entries) in enumerate(databases):
        folder = root / variant / f"h{i}" / "arm64-v8a"
        folder.mkdir(parents=True)
        (folder / "compile_commands.json").write_text(json.dumps([written(e) for e in entries]))


class TheRuleTest(unittest.TestCase):
    """A database AGP could write: the verdict is the docstring's rule."""

    @given(DATABASES)
    def test_the_verdict_is_the_rule(self, databases):
        with tempfile.TemporaryDirectory() as tmp:
            lay_out(Path(tmp), databases)
            code, out = check(Path(tmp))
        self.assertEqual(code, expected(databases), out)
        self.assertIn("PASS: " if code == 0 else "FAIL", out)


    @given(entry(), st.sampled_from(VARIANTS[:3]))
    def test_one_entry_beside_a_release_decides(self, one, variant):
        """Beside a file of ours compiled as a release, one entry: the verdict
        is that entry's alone."""
        good = {"kind": "game", "file": "/w/src/main.cpp", "form": "arguments",
                "flags": ["-DRMP_PRODUCTION_BUILD=1", "-O2"]}
        databases = [(variant, [good, one])]
        with tempfile.TemporaryDirectory() as tmp:
            lay_out(Path(tmp), databases)
            code, out = check(Path(tmp))
        self.assertEqual(code, expected(databases), out)


# What a database can be once nobody vouches for it: cut short, of another
# shape, an entry whose fields are of the wrong type, a command that does not
# split as a shell line, bytes that are not UTF-8.
JSONS = st.recursive(st.none() | st.booleans() | st.integers() | st.text(max_size=6),
                     lambda inner: st.lists(inner, max_size=3)
                     | st.dictionaries(st.sampled_from(["file", "command", "arguments",
                                                        "directory", "x"]), inner, max_size=4),
                     max_leaves=10)
TEXTS = st.one_of(
    JSONS.map(json.dumps),
    st.lists(entry(), min_size=1, max_size=3).map(
        lambda es: json.dumps([written(e) for e in es])).flatmap(
        lambda text: st.integers(0, len(text)).map(lambda n: text[:n])),
    st.lists(st.fixed_dictionaries({"file": st.sampled_from(KINDS["game"]),
                                     "command": st.text(alphabet="-O0 \"'\\a=", max_size=12)}),
             max_size=2).map(json.dumps),
    st.text(max_size=40))


class AnyDatabaseTest(unittest.TestCase):
    """Whatever the file holds, the check exits 0 or 1 and says which: a
    database it cannot read is a FAIL that names it."""

    @given(st.lists(st.tuples(st.sampled_from(VARIANTS), TEXTS | st.binary(max_size=40)),
                    min_size=1, max_size=3))
    def test_exit_0_or_1_and_a_sentence(self, files):
        with tempfile.TemporaryDirectory() as tmp:
            for i, (variant, content) in enumerate(files):
                folder = Path(tmp) / variant / f"h{i}"
                folder.mkdir(parents=True)
                db = folder / "compile_commands.json"
                if isinstance(content, bytes):
                    db.write_bytes(content)
                else:
                    db.write_text(content)
            code, out = check(Path(tmp))
        self.assertIn(code, (0, 1), out)
        self.assertIn("PASS: " if code == 0 else "FAIL", out)


if __name__ == "__main__":
    unittest.main()
