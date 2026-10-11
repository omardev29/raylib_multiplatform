"""tools/header_cost.py's budget file, tools/header_budget.txt, read for ANY
text: a budget, or a FAIL that names the line -- never a traceback in
`rmp test cost` -- and what --write-budget writes reads back as it was.

The file is edited by hand (a line raised in the commit that earned it, a
note to say why), so what it holds is whatever a person typed.

Every property was seen red; the commit that added this file says how, and
each counterexample is a named case in tests/gates_red_test.py.
"""

from __future__ import annotations

import importlib.util
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hypothesis_profiles  # noqa: E402  (first: the profile, or the skip)
from hypothesis import given, strategies as st  # noqa: E402

# As `tools.header_cost`: the name mutmut records hits under.
_spec = importlib.util.spec_from_file_location(
    "tools.header_cost", hypothesis_profiles.REPO / "tools" / "header_cost.py")
hc = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = hc
_spec.loader.exec_module(hc)

# Up to past the 16 columns --write-budget pads a name to.
NAMES = st.from_regex(r"[a-z_]{1,20}\.h", fullmatch=True)
BUDGETS = st.dictionaries(NAMES, st.integers(0, 10**7), max_size=8)
# What a line can hold: the words a budget is made of, and what a hand adds.
WORD = st.one_of(NAMES, st.integers(-5, 10**6).map(str),
                 st.sampled_from(["#", "# note", "1_000", "+3", "\u0663", "12.5", "x", "\t"]),
                 st.text(alphabet=st.characters(exclude_characters="\r\n"), max_size=6))
LINES = st.lists(st.lists(WORD, max_size=4).map(" ".join), max_size=8).map("\n".join)


class BudgetFileTest(unittest.TestCase):

    @given(BUDGETS)
    def test_what_is_written_reads_back(self, budget):
        self.assertEqual(hc.parse_budget(hc.format_budget(budget.items())), budget)

    @given(BUDGETS, st.lists(st.sampled_from(["", "   ", "# a note", "#", "  # indented"]),
                             max_size=4), st.randoms(use_true_random=False))
    def test_notes_and_blank_lines_change_nothing(self, budget, notes, random):
        lines = hc.format_budget(budget.items()).splitlines()
        for note in notes:
            lines.insert(random.randint(0, len(lines)), note)
        self.assertEqual(hc.parse_budget("\n".join(lines)), budget)

    @given(BUDGETS.filter(bool), st.data())
    def test_a_header_twice_is_refused_at_its_second_line(self, budget, data):
        """Two budgets for one header: which one holds is nobody's guess."""
        lines = hc.format_budget(budget.items()).splitlines()
        name = data.draw(st.sampled_from(sorted(budget)))
        first = next(i for i, line in enumerate(lines) if line.split()[:1] == [name])
        at = data.draw(st.integers(first + 1, len(lines)))
        lines.insert(at, f"{name} {data.draw(st.integers(0, 99))}")
        with self.assertRaisesRegex(hc.BudgetError,
                                    rf"^tools/header_budget\.txt:{at + 1}: {re.escape(name)} "):
            hc.parse_budget("\n".join(lines))

    @given(LINES)
    def test_any_text_is_a_budget_or_names_its_line(self, text):
        try:
            budget = hc.parse_budget(text)
        except hc.BudgetError as e:
            m = re.match(r"tools/header_budget\.txt:(\d+): ", str(e))
            self.assertIsNotNone(m, str(e))
            line = text.splitlines()[int(m.group(1)) - 1].strip()
            self.assertTrue(line and not line.startswith("#"), f"{e} names a line of nothing")
            return
        for name, lines in budget.items():
            self.assertIsInstance(lines, int)
            self.assertGreaterEqual(lines, 0)
            self.assertRegex(text, rf"(^|\n)\s*{re.escape(name)}\s+0*{lines}\s*(\n|$)")


class VerdictTest(unittest.TestCase):
    """main()'s verdict, with the compiler stood in for: for any headers
    measured at any size, any budget, inside the image or not, --check and
    --strict or not -- exit 1 exactly when the docstring says.

      - a header over its budget by more than BUDGET_TOLERANCE GREW, under it
        by as much is a STALE BUDGET: fatal in the image or with --strict,
        a note elsewhere;
      - a header with no budget, and a budget for a header that is gone, fail
        everywhere: whether a name is in a file is the same on every machine;
      - without --check nothing is compared, and --write-budget writes what
        was measured."""

    @given(st.dictionaries(NAMES, st.integers(0, 10**5), min_size=1, max_size=5),
           st.dictionaries(NAMES, st.integers(0, 10**5), max_size=5), st.data(),
           st.booleans(), st.booleans(), st.booleans(), st.booleans())
    def test_exit_1_exactly_when_the_rule_says(self, measured, budget, data, check, strict,
                                                in_image, write):
        import contextlib
        import io
        import tempfile
        from unittest import mock
        # Budgets for headers that are gone, or none: either decides alone.
        if data.draw(st.booleans()):
            budget = {name: n for name, n in budget.items() if name in measured}
        # Some budgets near what was measured: the tolerance's edges.
        for name in measured:
            if data.draw(st.booleans()):
                budget[name] = data.draw(st.sampled_from(
                    [measured[name], int(measured[name] / 1.2), int(measured[name] / 0.8) + 1,
                     int(measured[name] / 1.2) + 1, int(measured[name] / 0.8)]))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "tools").mkdir()
            (root / "include" / "rmp").mkdir(parents=True)
            target = root / "tools" / "header_budget.txt"
            target.write_text(hc.format_budget(budget.items()))
            manifest = root / "image.json"
            if in_image:
                manifest.write_text("{}")
            headers = [root / "include" / "rmp" / name for name in sorted(measured)]
            argv = ["--check"] * check + ["--strict"] * strict + ["--write-budget"] * write
            out = io.StringIO()
            with mock.patch.object(hc, "REPO", root), mock.patch.object(hc, "BUDGET", target), \
                    mock.patch.object(hc, "IMAGE_MANIFEST", manifest), \
                    mock.patch.object(hc, "headers", lambda: headers), \
                    mock.patch.object(hc, "measure",
                                      lambda cxx, header, runs=5: (measured[header.name], 1.0)), \
                    contextlib.redirect_stdout(out):
                code = hc.main(argv)
            written = target.read_text()
        gate = check and (in_image or strict)
        tolerance = hc.BUDGET_TOLERANCE

        def off(name):
            want = budget[name]
            return (measured[name] > want * (1 + tolerance)
                    or measured[name] < want * (1 - tolerance))

        fails = check and (any(name not in budget or (gate and off(name)) for name in measured)
                           or bool(set(budget) - set(measured)))
        self.assertEqual(code, 1 if fails else 0, out.getvalue())
        self.assertIn("FAIL: " if fails else "PASS: ", out.getvalue())
        if write:
            self.assertEqual(hc.parse_budget(written), measured)
        else:
            self.assertEqual(hc.parse_budget(written), budget)


if __name__ == "__main__":
    unittest.main()
