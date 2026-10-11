"""tools/cpp_text.blank() -- the scanner tools/naming_check.sh and
tools/style_check.sh read every #if branch through -- held to its promise for
ANY text: every offset and line number still holds, only comments and
literals turn into spaces, and nothing is touched that was not asked for.

And the shape of a property test, for the next one to copy:

  - the module is tests/<what>_property_test.py, so `rmp test property` and the
    CI deep job find it (unittest discover -p '*_property_test.py');
  - `import hypothesis_profiles` comes first: it loads the shared profile
    (derandomized on every push) and skips the module, saying so, where
    Hypothesis is not installed;
  - the strategies lean on the characters the code under test decides on --
    st.text() alone almost never writes `R"x(`.

Every property here was seen red: the commit that added this file says how.
A counterexample Hypothesis finds becomes a named case in the module's own
unit tests, so it is checked on every run and not only when the search finds
it again.
"""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hypothesis_profiles  # noqa: E402,F401  (first: the profile, or the skip)
from hypothesis import given, strategies as st  # noqa: E402


# By its path and as `tools.cpp_text`, the name mutmut gives the file: it
# records which tests reach a function under the module's own name, and a
# test that imported it as plain `cpp_text` reaches nothing it can mutate.
_spec = importlib.util.spec_from_file_location(
    "tools.cpp_text", hypothesis_profiles.REPO / "tools" / "cpp_text.py")
cpp_text = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = cpp_text
_spec.loader.exec_module(cpp_text)

# What the scanner turns on, mixed with anything at all.
DECISIVE = st.sampled_from(list('/*"\'R(\\)\nux8_.L1 ') + ["//", "/*", "*/", 'R"', 'u8R"', "'"])
SOURCE = st.lists(st.one_of(DECISIVE, st.characters()), max_size=120).map("".join)


def newlines(text: str) -> list[int]:
    return [i for i, c in enumerate(text) if c == "\n"]


def blanked(text: str) -> str:
    return "".join(c if c == "\n" else " " for c in text)


# One piece of C++, as written and as blank() must leave it.
_PLAIN = st.characters(exclude_characters='\n"\'\\/*', exclude_categories=("Cs",))
CODE = st.text(alphabet=st.sampled_from("abcxyz019_ ;(){}=+-<>"), max_size=12)
LINE_COMMENT = st.text(alphabet=st.characters(exclude_characters="\n"), max_size=20).map(
    lambda said: "//" + said + "\n")
BLOCK_COMMENT = st.text(alphabet=st.characters(exclude_characters="*"), max_size=20).map(
    lambda said: "/*" + said + "*/")
STRING = st.text(alphabet=_PLAIN, max_size=20).map(lambda s: f'"{s}"')
CHARACTER = _PLAIN.map(lambda c: f"'{c}'")
RAW = st.text(alphabet=st.characters(exclude_characters=")"), max_size=20).map(
    lambda s: f'"x({s})x"')
PREFIX = st.sampled_from(["u8", "u", "U", "L"])
# A literal is blanked from its quote: a prefix -- R, u8, L -- stays, a word
# no rule reads as a name.
PIECE = st.one_of(CODE.map(lambda c: (c, c)),
                  st.one_of(LINE_COMMENT, BLOCK_COMMENT, STRING, CHARACTER).map(
                      lambda w: (w, blanked(w))),
                  st.tuples(PREFIX, st.one_of(STRING, CHARACTER)).map(
                      lambda pw: (pw[0] + pw[1], pw[0] + blanked(pw[1]))),
                  st.tuples(st.sampled_from(["", "u8", "L"]), RAW).map(
                      lambda pw: (pw[0] + "R" + pw[1], pw[0] + "R" + blanked(pw[1]))))


class BlankTest(unittest.TestCase):

    @given(SOURCE, st.booleans(), st.booleans())
    def test_every_offset_and_line_still_holds(self, text, comments, strings):
        out = cpp_text.blank(text, comments, strings)
        self.assertEqual(len(out), len(text))
        self.assertEqual(newlines(out), newlines(text))
        self.assertTrue(all(out[i] == " " for i in range(len(text)) if out[i] != text[i]))

    @given(SOURCE)
    def test_nothing_asked_for_is_nothing_changed(self, text):
        self.assertEqual(cpp_text.blank(text, comments=False, strings=False), text)

    @given(SOURCE, st.booleans(), st.booleans())
    def test_blanking_twice_is_blanking_once(self, text, comments, strings):
        once = cpp_text.blank(text, comments, strings)
        self.assertEqual(cpp_text.blank(once, comments, strings), once)

    @given(st.text(alphabet=st.characters(exclude_characters='/"\'\\'), max_size=120))
    def test_code_with_no_comment_and_no_literal_is_left_alone(self, code):
        self.assertEqual(cpp_text.blank(code), code)

    @given(st.lists(PIECE, max_size=12))
    def test_exactly_the_comments_and_literals_go(self, pieces):
        """A text built of known pieces, each followed by a `,`: what blank()
        gives is known before it runs -- code and the commas kept, comments
        and literals spaces, their newlines kept."""
        text = "".join(f"{written}," for written, _ in pieces)
        expected = "".join(f"{blanked}," for _, blanked in pieces)
        self.assertEqual(cpp_text.blank(text), expected)


if __name__ == "__main__":
    unittest.main()
