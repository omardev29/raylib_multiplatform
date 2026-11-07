"""tools/ink_to_yarn.py: an Ink story converted to Yarn, what it converts and
what it says it did not. tests/dialogue/ink/story.yarn is the converter's
output for story.ink, and tests/dialogue_test.cpp runs it."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from tools import ink_to_yarn  # noqa: E402


class InkToYarnTest(unittest.TestCase):
    def test_the_committed_story_is_what_the_converter_writes(self):
        ink = (REPO / "tests/dialogue/ink/story.ink").read_text()
        yarn, problems = ink_to_yarn.convert(ink)
        self.assertEqual(yarn, (REPO / "tests/dialogue/ink/story.yarn").read_text())
        self.assertEqual(problems, [])

    def test_each_construct(self):
        cases = {
            "VAR hp = 10\n=== k ===\n~ hp = hp - 1\n~ hp++\n": ["<<declare $hp = 10>>",
                                                              "<<set $hp = $hp - 1>>",
                                                              "<<set $hp += 1>>"],
            "=== k ===\n* [Once] text\n+ Again\n": ["-> Once <<once>>", "    text", "-> Again"],
            "VAR a = 1\n=== k ===\n* {a > 0} [Hi]\n": ["-> Hi <<once if $a > 0>>"],
            "=== k ===\n-> END\n-> DONE\n-> other\n= st\nx\n": ["<<stop>>", "<<jump other>>",
                                                               "title: k_st"],
            "=== k ===\nLine # tag one # two\n": ["Line #tag_one #two"],
            "VAR x = true\n=== k ===\n{x: Yes|No}\n": ["<<if $x>>", "Yes", "<<else>>", "No",
                                                     "<<endif>>"],
            "=== k ===\n* A\n- Gathered\n": ["-> A <<once>>", "", "Gathered"],
        }
        for ink, wanted in cases.items():
            with self.subTest(ink=ink):
                yarn, _ = ink_to_yarn.convert(ink)
                for w in wanted:
                    self.assertIn(w + "\n", yarn)

    def test_what_is_not_converted_is_reported_with_its_line_and_kept(self):
        ink = ("=== k ===\n* * nested\nHi <> there\n-> t ->->\n<- thread\n"
               "A {x: b} c\n=== f(x) ===\n- (label) g\n~ f(1)\n")
        yarn, problems = ink_to_yarn.convert(ink)
        joined = "\n".join(problems)
        for needle in ("2: nested choices", "3: glue", "4: tunnels and threads",
                       "5: tunnels and threads", "6: a condition inside a line",
                       "7: knot parameters", "8: labelled gathers", "9: '~ f(1)'"):
            self.assertIn(needle, joined)
        self.assertIn("// * * nested", yarn)
        self.assertIn("// A {x: b} c", yarn)

    def test_an_empty_top_starts_at_the_first_knot(self):
        yarn, _ = ink_to_yarn.convert("=== first ===\nHello\n")
        self.assertTrue(yarn.startswith("title: Start\n---\n<<jump first>>\n===\n"))

    def test_main_writes_the_file_and_says_how_many(self):
        import contextlib
        import io
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "s.ink"
            dst = Path(tmp) / "s.yarn"
            src.write_text("Hello <> world\n")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(ink_to_yarn.main([str(src), str(dst)]), 0)
            self.assertIn("1 thing(s) not converted", out.getvalue())
            self.assertIn("Hello  world", dst.read_text())
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(ink_to_yarn.main([]), 2)


if __name__ == "__main__":
    unittest.main()
