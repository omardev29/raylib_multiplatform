"""tools/text_tool.py -- `rmp text scan` and `rmp text check`: on planted
trees, every text found, every translation kept, every mistake reported."""

from __future__ import annotations

import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
from tools import text_tool  # noqa: E402  (loaded as tools.<name>, as mutmut records it)


def tree(files: dict[str, str]) -> tempfile.TemporaryDirectory:
    tmp = tempfile.TemporaryDirectory()
    root = Path(tmp.name)
    for name, body in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(body), encoding="utf-8")
    return tmp


GAME = """
    #include <rmp/ui.h>
    void menu() {
        rmp::ui::begin();
        if (rmp::ui::button("Play")) play();
        rmp::ui::text(rmp::Text("Score: {score}", { { "score", score } }));
        rmp::ui::button(rmp::Text("Open", { .context = "door" }));
        rmp::ui::text(TextFormat("%d", score));   // runtime: not a key
        rmp::ui::text(name);                       // runtime: not a key
        // rmp::ui::button("Commented out");
        /* rmp::ui::button("Also out"); */
        rmp::ui::dropdown("Difficulty", d, { "Easy", "Hard" });
        rmp::ui::notify("Saved" " now");
        rmp::ui::Button quit{ .text = "Quit" };
        rmp::ui::TextInput field{ .text = "Name", .placeholder = "Type here" };
        rmp::ui::Dropdown size{ .text = "Size", .items = { "Small", "Big" } };
        rmp::ui::end();
    }
"""


class ScanTest(unittest.TestCase):
    def test_every_literal_the_framework_translates_and_nothing_else(self):
        found = text_tool.scan_source(textwrap.dedent(GAME))
        self.assertEqual(found, [
            ("Play", ""), ("Score: {score}", ""), ("Open", "door"), ("Difficulty", ""),
            ("Easy", ""), ("Hard", ""), ("Saved now", ""), ("Quit", ""), ("Name", ""),
            ("Type here", ""), ("Size", ""), ("Small", ""), ("Big", ""),
        ])

    def test_escapes_and_raw_strings(self):
        found = text_tool.scan_source('button("a\\tb"); text(R"x(raw "quoted")x");')
        self.assertEqual(found, [("a\tb", ""), ('raw "quoted"', "")])

    def test_scan_adds_rows_and_keeps_translations(self):
        with tree({"src/main.cpp": GAME,
                   "resources/lang.tsv": "key\tcontext\ten\tes\nPlay\t\tPlay\tJugar\n"
                                         "Gone\t\tGone\tIdo\n",
                   "resources/talk.yarn": "Mika: Hello there. #line:mika_hello\n"
                                          "Mika: You have {$gold} gold, {$name}. #line:gold\n"
                                          "-> Buy one <<if $gold >= 5>> #line:buy #tag\n",
                   "build/debug/lang_seen.tsv": "key\tcontext\nSeen at runtime\t\n"}) as tmp:
            root = Path(tmp)
            said = text_tool.scan(root)
            self.assertIn("new row(s)", said)
            wide = text_tool.Wide((root / "resources/lang.tsv").read_text())
            rows = {(r[0], r[1]): r for r in wide.rows}
            es = wide.header.index("es")
            en = wide.header.index("en")
            unused = wide.header.index("unused")
            self.assertEqual(rows[("Play", "")][es], "Jugar")       # kept
            self.assertEqual(rows[("Gone", "")][es], "Ido")         # kept...
            self.assertEqual(rows[("Gone", "")][unused], "x")       # ...and marked
            self.assertEqual(rows[("Play", "")][unused], "")
            self.assertEqual(rows[("Open", "door")][en], "Open")
            self.assertEqual(rows[("line:mika_hello", "")][en], "Mika: Hello there.")
            # As Yarn's own string table writes them: the expressions numbered,
            # what the machine fills, and an option's condition left out.
            self.assertEqual(rows[("line:gold", "")][en], "Mika: You have {0} gold, {1}.")
            self.assertEqual(rows[("line:buy", "")][en], "Buy one")
            self.assertIn(("Seen at runtime", ""), rows)
            # Twice is the same table.
            before = (root / "resources/lang.tsv").read_text()
            text_tool.scan(root)
            self.assertEqual((root / "resources/lang.tsv").read_text(), before)
            # And the table still reads: the framework skips the unused column.
            self.assertEqual(text_tool.check(root)[:0], [])

    def test_a_row_used_again_loses_its_mark(self):
        with tree({"src/main.cpp": 'button("Gone");',
                   "resources/lang.tsv": "key\tcontext\ten\tunused\nGone\t\tGone\tx\n"}) as tmp:
            text_tool.scan(Path(tmp))
            wide = text_tool.Wide((Path(tmp) / "resources/lang.tsv").read_text())
            self.assertEqual(wide.rows[0][wide.header.index("unused")], "")


class CheckTest(unittest.TestCase):
    TOML = '[lang]\nlanguages = ["en", "es", "ru"]\n'

    def problems(self, table: str, toml: str = TOML, extra=None):
        files = {"raylib_multiplatform.toml": toml, "resources/lang.tsv": table}
        files.update(extra or {})
        with tree(files) as tmp:
            return text_tool.check(Path(tmp))

    def test_a_right_table_passes(self):
        self.assertEqual(self.problems(
            "key\tcontext\ten\tes\tru\n"
            "Play\t\tPlay\tJugar\tИграть\n"
            "{n} gem|{n} gems\t\t{n} gem|{n} gems\t{n} gema|{n} gemas\ta|b|c|d\n"
            .replace("a|b|c|d", "{n} а|{n} б|{n} в|{n} г")), [])

    def test_no_tables_is_nothing_to_check(self):
        with tree({"src/main.cpp": 'button("Play");'}) as tmp:
            self.assertEqual(text_tool.check(Path(tmp)), [])

    def test_a_missing_translation(self):
        got = self.problems("key\tcontext\ten\tes\tru\nPlay\t\tPlay\tJugar\t\n")
        self.assertEqual(got, ["missing: 'Play' has no ru text"])

    def test_a_language_with_no_column_at_all(self):
        got = self.problems("key\tcontext\ten\tes\nPlay\t\tPlay\tJugar\n")
        self.assertEqual(got, ["missing: 'Play' has no ru text"])

    def test_a_lost_and_an_invented_placeholder(self):
        got = self.problems("key\tcontext\ten\tes\tru\n"
                            "Hi {name}\t\tHi {name}\tHola\tПривет {nam}\n")
        self.assertIn("placeholder: 'Hi {name}' in es lost {name}", got)
        self.assertIn("placeholder: 'Hi {name}' in ru lost {name}", got)
        self.assertIn("placeholder: 'Hi {name}' in ru has {nam}, which the text does not", got)

    def test_a_yarn_line_that_lost_its_numbered_expression(self):
        # A Yarn line's {0} is filled by the machine: a translation without it
        # shows no gold at all.
        got = self.problems("key\tcontext\ten\tes\tru\n"
                            "line:gold\t\tYou have {0} gold.\tTienes oro.\tУ тебя {0} золота.\n")
        self.assertEqual(got, ["placeholder: 'line:gold' in es lost {0}"])

    def test_the_wrong_number_of_plural_forms(self):
        got = self.problems("key\tcontext\ten\tes\tru\n"
                            "{n} gem|{n} gems\t\t{n} gem|{n} gems\t{n} gemas\t{n} а|{n} б\n")
        self.assertIn("plural: '{n} gem|{n} gems' in es has 1 form(s); es has 2", got)
        self.assertIn("plural: '{n} gem|{n} gems' in ru has 2 form(s); ru has 4", got)

    def test_a_context_is_named(self):
        got = self.problems("key\tcontext\ten\tes\tru\nOpen\tdoor\tOpen\tAbrir\t\n")
        self.assertEqual(got, ["missing: 'Open' (door) has no ru text"])

    def test_per_language_files_count(self):
        got = self.problems("key\tcontext\ten\tes\nPlay\t\tPlay\tJugar\n",
                            extra={"resources/lang/ru.tsv": "key\tcontext\ttext\nPlay\t\tИграть\n"})
        self.assertEqual(got, [])

    def test_an_unused_row_is_not_held(self):
        got = self.problems("key\tcontext\ten\tes\tru\tunused\nOld\t\tOld\t\t\tx\n")
        self.assertEqual(got, [])

    def test_without_a_list_every_column_is_held(self):
        got = self.problems("key\tcontext\ten\tes\tja\nPlay\t\tPlay\tJugar\t\n", toml="")
        self.assertEqual(got, ["missing: 'Play' has no ja text"])

    def test_main_says_fail_and_exits_1(self):
        import io
        import contextlib
        import os
        with tree({"raylib_multiplatform.toml": self.TOML,
                   "resources/lang.tsv": "key\tcontext\ten\nPlay\t\tPlay\n"}) as tmp:
            old = os.getcwd()
            os.chdir(tmp)
            try:
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    self.assertEqual(text_tool.main(["check"]), 1)
                self.assertIn("FAIL: 2 problem(s)", out.getvalue())
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(text_tool.main(["nothing"]), 2)
            finally:
                os.chdir(old)


class SameTsvAsTheFrameworkTest(unittest.TestCase):
    def test_escapes_round_trip(self):
        for cell in ("plain", "tab\there", "line\nbreak", "back\\slash", ""):
            with self.subTest(cell=cell):
                self.assertEqual(text_tool.parse_tsv(text_tool.write_tsv([[cell, "x"]]))[0][0],
                                 cell)

    def test_the_plural_counts_are_the_frameworks(self):
        source = (REPO / "src/rmp/text.cpp").read_text()
        for code, n in (("en", 2), ("ja", 1), ("ru", 4), ("fr", 2), ("pl", 4)):
            with self.subTest(code=code):
                self.assertEqual(text_tool.PLURAL_COUNTS[code], n)
                self.assertIn(f'"{code}"', source)


if __name__ == "__main__":
    unittest.main()
