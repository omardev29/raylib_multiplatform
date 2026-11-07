"""tools/yarn_tool.py -- `rmp yarn check` and `rmp yarn ysls` -- on planted trees.

Each mistake the checker is for is planted and seen red: a script that does not
compile, a typed function called with the wrong kind or count, a function
nothing registers, a command nothing handles (a warning, not a failure). The
scanner reads typed lambdas, raw callbacks, results, the comment above, and
skips what a comment hides. The .ysls.json holds to the language server's
schema: its keys, its required fields, its four types.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))

from tools import yarn_tool  # noqa: E402

# The language server's .ysls.json (YarnSpinner.LanguageServer, v2.4.2's
# ysls.schema.json), as its constraints: no other keys, these required, these types.
TYPES = {"number", "string", "bool", "any"}
COMMAND_KEYS = {"YarnName", "DefinitionName", "FileName", "Language", "Documentation",
                "Signature", "Parameters"}
FUNCTION_KEYS = COMMAND_KEYS | {"ReturnType"}
PARAMETER_KEYS = {"Name", "Type", "Documentation", "DefaultValue", "IsParamsArray"}

GAME_CPP = r'''
#include <rmp/dialogue.h>
void setup(rmp::Dialogue &talk) {
    // Whether the player carries `item`.
    // Keys count too.
    talk.on_function("has_item", [&](const std::string &item, int count) -> bool {
        return count > 0;
    });
    talk.on_function("price", [](std::string_view what) mutable -> float { return 2; });
    talk.on_function("raw", [](rmp::DialogueValue &out,
                               std::span<const rmp::DialogueValue> args) { out = 1; });
    // Shakes the camera.
    talk.on_command("shake", [&](float strength, bool hard) {});
    talk.on_command("log", [&](std::span<const std::string> words) {});
    talk.on_command("beep", [&] {});
    talk.on_function("kept", kept_somewhere);
    // talk.on_function("hidden", [] { return 1; });
    /* talk.on_command("also_hidden", [] {}); */
}
'''


def yarnc() -> str:
    """The framework's compiler, built if it is not there yet."""
    exe = REPO / "build" / "debug" / ("rmp_yarnc.exe" if os.name == "nt" else "rmp_yarnc")
    if not exe.is_file():
        subprocess.run(["cmake", "--build", "--preset", "debug", "--target", "rmp_yarnc"],
                       cwd=REPO, check=True, stdin=subprocess.DEVNULL)
    return str(exe)


class Planted(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = Path(tempfile.mkdtemp())
        (self.dir / "src").mkdir()
        (self.dir / "resources").mkdir()
        (self.dir / "src" / "main.cpp").write_text(GAME_CPP, encoding="utf-8")

    def tearDown(self) -> None:
        shutil.rmtree(self.dir)

    def script(self, name: str, body: str) -> None:
        (self.dir / "resources" / name).write_text(f"title: Start\n---\n{body}\n===\n",
                                                   encoding="utf-8")

    def check(self) -> tuple[list[str], list[str]]:
        return yarn_tool.check(self.dir, yarnc(), [])


class ScannerTest(Planted):
    def test_typed_functions_raw_ones_and_commands_are_read(self) -> None:
        got = {(r.kind, r.name): r for r in yarn_tool.scan_cpp(self.dir, self.dir)}
        has = got[("function", "has_item")]
        self.assertTrue(has.typed)
        self.assertEqual([(p.name, p.type) for p in has.params],
                         [("item", "string"), ("count", "number")])
        self.assertEqual(has.result, "bool")
        self.assertEqual(has.doc, "Whether the player carries `item`. Keys count too.")
        self.assertEqual(has.file, "src/main.cpp")
        self.assertEqual(has.line, 6)
        price = got[("function", "price")]
        self.assertEqual([p.type for p in price.params], ["string"])
        self.assertEqual(price.result, "number")
        self.assertFalse(got[("function", "raw")].typed)
        self.assertFalse(got[("function", "kept")].typed)
        shake = got[("command", "shake")]
        self.assertEqual([(p.name, p.type) for p in shake.params],
                         [("strength", "number"), ("hard", "bool")])
        self.assertEqual(shake.doc, "Shakes the camera.")
        self.assertFalse(got[("command", "log")].typed)
        self.assertEqual(got[("command", "beep")].params, [])

    def test_what_a_comment_hides_is_not_registered(self) -> None:
        names = {r.name for r in yarn_tool.scan_cpp(self.dir, self.dir)}
        self.assertNotIn("hidden", names)
        self.assertNotIn("also_hidden", names)

    def test_types_read_off_cpp(self) -> None:
        for cpp, want in (("const std::string &", "string"), ("std::string_view", "string"),
                          ("bool", "bool"), ("unsigned int", "number"), ("double", "number"),
                          ("std::int64_t", "number"), ("rmp::DialogueValue", "any"),
                          ("Thing", "any")):
            self.assertEqual(yarn_tool.yarn_type(cpp), want, cpp)


class CheckTest(Planted):
    def test_a_good_script_passes(self) -> None:
        self.script("good.yarn", '<<if has_item("key", 1)>>\nYes {price("pie")}.\n<<endif>>\n'
                                 '{raw(1, "a")}\n<<shake 2 true>>\n<<log a b>>\n<<beep>>\n'
                                 '<<wait 1>>')
        errors, warnings = self.check()
        self.assertEqual(errors, [])
        self.assertEqual(warnings, [])

    def test_a_script_that_does_not_compile_is_said_where(self) -> None:
        self.script("bad.yarn", "{1 +}")
        errors, _ = self.check()
        self.assertTrue(any(e.startswith("resources/bad.yarn:3:") for e in errors), errors)

    def test_a_typed_function_called_with_the_wrong_kind_fails(self) -> None:
        self.script("kind.yarn", '<<if has_item(3, 1)>>\nNo.\n<<endif>>')
        errors, _ = self.check()
        self.assertTrue(any("resources/kind.yarn:3:" in e and "has_item" in e for e in errors),
                        errors)

    def test_a_typed_function_called_with_the_wrong_count_fails(self) -> None:
        self.script("count.yarn", '<<if has_item("key")>>\nNo.\n<<endif>>')
        errors, _ = self.check()
        self.assertTrue(any("has_item" in e and "argument" in e for e in errors), errors)

    def test_a_function_nothing_registers_fails(self) -> None:
        self.script("missing.yarn", "Hi.\n{nowhere()}")
        errors, _ = self.check()
        self.assertIn('resources/missing.yarn:4: the script calls nowhere(), and the C++ of . '
                      'registers no on_function("nowhere", ...)', errors)

    def test_a_command_nothing_handles_is_a_warning(self) -> None:
        self.script("cmd.yarn", "<<open_door now>>\nHi.")
        errors, warnings = self.check()
        self.assertEqual(errors, [])
        self.assertEqual(len(warnings), 1)
        self.assertIn("<<open_door>>", warnings[0])
        self.assertIn("resources/cmd.yarn:3", warnings[0])

    def test_naming_files_checks_only_those(self) -> None:
        self.script("bad.yarn", "{1 +}")
        self.script("good.yarn", "Fine.")
        errors, _ = yarn_tool.check(self.dir, yarnc(),
                                    [str(self.dir / "resources" / "good.yarn")])
        self.assertEqual(errors, [])

    def test_the_command_line_fails_and_passes(self) -> None:
        self.script("missing.yarn", "{nowhere()}")
        run = subprocess.run([sys.executable, str(REPO / "tools" / "yarn_tool.py"), "check",
                              "--yarnc", yarnc()], cwd=self.dir, capture_output=True,
                             text=True, stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(run.returncode, 1, run.stdout + run.stderr)
        self.assertIn("FAIL: 1 problem(s)", run.stdout)
        (self.dir / "resources" / "missing.yarn").unlink()
        self.script("fine.yarn", "Fine.")
        run = subprocess.run([sys.executable, str(REPO / "tools" / "yarn_tool.py"), "check",
                              "--yarnc", yarnc()], cwd=self.dir, capture_output=True,
                             text=True, stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("rmp yarn check: ok", run.stdout)

    def test_no_scripts_is_nothing_to_check(self) -> None:
        run = subprocess.run([sys.executable, str(REPO / "tools" / "yarn_tool.py"), "check",
                              "--yarnc", "/nonexistent"], cwd=self.dir, capture_output=True,
                             text=True, stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(run.returncode, 0)
        self.assertIn("nothing to check", run.stdout)

    def test_examples_are_projects_of_their_own(self) -> None:
        example = self.dir / "examples" / "ui" / "talk"
        (example / "src").mkdir(parents=True)
        (example / "resources").mkdir()
        (example / "src" / "main.cpp").write_text(
            'void f(rmp::Dialogue &d) { d.on_function("own", [] { return 1; }); }\n',
            encoding="utf-8")
        (example / "resources" / "t.yarn").write_text(
            "title: Start\n---\n{own()} {has_item(\"x\", 1)}\n===\n", encoding="utf-8")
        errors, _ = self.check()
        # own() is the example's; has_item() is the root's, not the example's.
        self.assertEqual(len(errors), 1, errors)
        self.assertIn("has_item", errors[0])
        self.assertIn("examples/ui/talk", errors[0])


class YslsTest(Planted):
    def test_the_file_holds_to_the_schema(self) -> None:
        self.script("t.yarn", "Hi.")
        written = yarn_tool.write_ysls(self.dir)
        self.assertEqual(written, [self.dir / "rmp.ysls.json"])
        data = json.loads(written[0].read_text(encoding="utf-8"))
        self.assertEqual(set(data), {"Commands", "Functions"})
        for entry in data["Commands"]:
            self.assertLessEqual(set(entry), COMMAND_KEYS)
            self.assertIn("YarnName", entry)
            self.assertIn("Parameters", entry)
        for entry in data["Functions"]:
            self.assertLessEqual(set(entry), FUNCTION_KEYS)
            for key in ("YarnName", "ReturnType", "Parameters"):
                self.assertIn(key, entry)
            self.assertIn(entry["ReturnType"], TYPES)
        for entry in data["Commands"] + data["Functions"]:
            for p in entry["Parameters"]:
                self.assertLessEqual(set(p), PARAMETER_KEYS)
                self.assertIn(p["Type"], TYPES)

    def test_what_it_says_of_each(self) -> None:
        data = yarn_tool.ysls(self.dir, self.dir)
        functions = {f["YarnName"]: f for f in data["Functions"]}
        commands = {c["YarnName"]: c for c in data["Commands"]}
        has = functions["has_item"]
        self.assertEqual(has["ReturnType"], "bool")
        self.assertEqual([(p["Name"], p["Type"]) for p in has["Parameters"]],
                         [("item", "string"), ("count", "number")])
        self.assertEqual(has["Signature"], "has_item(item, count)")
        self.assertEqual(has["Documentation"],
                         "Whether the player carries `item`. Keys count too.")
        self.assertEqual(has["FileName"], "src/main.cpp")
        raw = functions["raw"]
        self.assertEqual(raw["ReturnType"], "any")
        self.assertTrue(raw["Parameters"][0]["IsParamsArray"])
        self.assertEqual(commands["shake"]["Signature"], "shake strength hard")
        self.assertNotIn("ReturnType", commands["shake"])
        self.assertEqual(commands["beep"]["Parameters"], [])

    def test_the_command_line_writes_it(self) -> None:
        self.script("t.yarn", "Hi.")
        run = subprocess.run([sys.executable, str(REPO / "tools" / "yarn_tool.py"), "ysls"],
                             cwd=self.dir, capture_output=True, text=True,
                             stdin=subprocess.DEVNULL, check=False)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("wrote rmp.ysls.json", run.stdout)
        self.assertTrue((self.dir / "rmp.ysls.json").is_file())


if __name__ == "__main__":
    unittest.main()
