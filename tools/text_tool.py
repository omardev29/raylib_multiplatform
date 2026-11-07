#!/usr/bin/env python3
"""rmp text: the game's translation tables, kept in step with its code.

    rmp text scan     add every text the game shows to the tables
    rmp text check    fail on a text a shipped language lacks, or gets wrong

`scan` reads the game's src/ and include/ for the literals rmp::Text takes --
the first argument of rmp::ui's text functions (button, text, checkbox,
slider, dropdown and its items, text_input, tooltip, notify), of rmp::Text(),
and the `.text`, `.placeholder` and `.items` of a kept widget -- with the
context an rmp::Text gives, the line ids of resources/**/*.yarn, and what a
debug build saw and the tables lacked (build/debug/lang_seen.tsv). It adds a row
for each that is not there, keeps every translation, and marks a row nothing
uses any more in an `unused` column instead of deleting it.

`check` holds the tables to [lang] languages (or to every column they have):
a text with no translation, a placeholder a translation lost or invented, a
plural with the wrong number of forms for its language. Each mistake is a
line with the file, the key and the language.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# The functions whose first argument is a text the player reads.
TEXT_FUNCS = ("button", "text", "checkbox", "slider", "dropdown", "text_input", "tooltip",
              "notify", "Text", "translate")
FIELDS = ("text", "placeholder")

# CLDR forms per language, as src/rmp/text.cpp counts them (0 = any).
PLURAL_COUNTS = {}
for _codes, _n in ((("ja", "zh", "ko", "th", "vi", "id", "ms", "lo", "my", "km"), 1),
                   (("en", "de", "es", "it", "nl", "sv", "da", "nb", "nn", "no", "fi", "et",
                     "ca", "el", "hu", "tr", "bg", "eu", "gl", "af", "sq", "ka", "kk", "ky",
                     "mn", "ur", "ta", "te", "ml", "fr", "pt", "hi", "bn", "fa"), 2),
                   (("ru", "uk", "be", "pl", "cs", "sk"), 4)):
    for _c in _codes:
        PLURAL_COUNTS[_c] = _n

LANG_CODE = re.compile(r"^[a-z]{2,3}(-[A-Za-z0-9]{2,8})*$")
# {name} of an rmp::Text, {0} of a Yarn line.
PLACEHOLDER = re.compile(r"(?<!\{)\{([A-Za-z_][A-Za-z0-9_]*|[0-9]+)\}(?!\})")
YARN_LINE_ID = re.compile(r"#line:([A-Za-z0-9_.\-]+)")
# The row a language menu reads its name from, through rmp::lang::name().
LANGUAGE_NAME = "language.name"


class TextError(Exception):
    pass


# --- the C++ scanner --------------------------------------------------------

def _tokens(source: str):
    """(kind, text) for every token that matters: identifiers, string literals
    (adjacent ones joined), and punctuation. Comments are skipped; a raw
    string literal is a literal."""
    i, n = 0, len(source)
    pending = None  # a literal, waiting for the next one to join
    while i < n:
        c = source[i]
        if source.startswith("//", i):
            i = source.find("\n", i)
            i = n if i < 0 else i
            continue
        if source.startswith("/*", i):
            end = source.find("*/", i + 2)
            i = n if end < 0 else end + 2
            continue
        m = re.match(r'(u8|u|U|L)?R"([^(\s]*)\(', source[i:])
        if m:
            close = ")" + m.group(2) + '"'
            end = source.find(close, i + m.end())
            end = n if end < 0 else end
            text = source[i + m.end():end]
            pending = (pending or "") + text
            i = end + len(close)
            continue
        if c == '"' or (c in "uUL8" and re.match(r'(u8|u|U|L)"', source[i:])):
            start = source.index('"', i) + 1
            j, out = start, []
            while j < n and source[j] != '"':
                if source[j] == "\\" and j + 1 < n:
                    esc = source[j + 1]
                    out.append({"n": "\n", "t": "\t", "\\": "\\", '"': '"', "'": "'",
                                "0": "\0"}.get(esc, "\\" + esc))
                    j += 2
                    continue
                if source[j] == "\n":
                    break
                out.append(source[j])
                j += 1
            pending = (pending or "") + "".join(out)
            i = j + 1
            continue
        if c.isspace():
            i += 1
            continue
        if pending is not None:
            yield ("str", pending)
            pending = None
        if c == "'":
            end = i + 1
            while end < n and source[end] != "'":
                end += 2 if source[end] == "\\" else 1
            i = end + 1
            continue
        m = re.match(r"[A-Za-z_][A-Za-z0-9_]*", source[i:])
        if m:
            yield ("id", m.group(0))
            i += m.end()
            continue
        yield ("punct", c)
        i += 1
    if pending is not None:
        yield ("str", pending)


def scan_source(source: str) -> list[tuple[str, str]]:
    """(key, context) of every translatable literal in one C++ file."""
    toks = list(_tokens(source))
    found = []

    def context_after(k):
        # rmp::Text("Open", { .context = "door" }) and the args form:
        # the `.context = "..."` before the call's closing parenthesis.
        depth = 0
        for j in range(k, min(len(toks), k + 200)):
            kind, text = toks[j]
            if kind == "punct" and text in "({[":
                depth += 1
            elif kind == "punct" and text in ")}]":
                depth -= 1
                if depth < 0:
                    return ""
            elif (kind == "id" and text == "context" and j + 2 < len(toks)
                  and toks[j + 1] == ("punct", "=") and toks[j + 2][0] == "str"):
                return toks[j + 2][1]
        return ""

    for k, (kind, text) in enumerate(toks):
        if kind != "str":
            continue
        prev = toks[k - 1] if k >= 1 else ("", "")
        prev2 = toks[k - 2] if k >= 2 else ("", "")
        # f("...") -- the first argument of a text function
        if prev == ("punct", "(") and prev2[0] == "id" and prev2[1] in TEXT_FUNCS:
            found.append((text, context_after(k + 1)))
            continue
        # .text = "..." / .placeholder = "..."
        if prev == ("punct", "=") and prev2[0] == "id" and prev2[1] in FIELDS:
            if k >= 3 and toks[k - 3] == ("punct", "."):
                found.append((text, ""))
                continue
        # the items of a dropdown: dropdown("..", sel, { "a", "b" }) or .items = { ... }
        if prev[0] == "punct" and prev[1] in "{,":
            j, depth = k - 1, 0
            while j >= 0:
                kj, tj = toks[j]
                if kj == "punct" and tj == "}":
                    depth += 1
                elif kj == "punct" and tj == "{":
                    if depth == 0:
                        break
                    depth -= 1
                j -= 1
            if j >= 2 and toks[j - 1] == ("punct", "=") and toks[j - 2] == ("id", "items"):
                found.append((text, ""))
                continue
            # dropdown(<label>, <selected>, { ... })
            if j >= 1 and toks[j - 1] == ("punct", ","):
                m, depth = j - 2, 0
                while m >= 0:
                    km, tm = toks[m]
                    if km == "punct" and tm in ")}]":
                        depth += 1
                    elif km == "punct" and tm in "({[":
                        if depth == 0:
                            break
                        depth -= 1
                    m -= 1
                if m >= 1 and toks[m] == ("punct", "(") and toks[m - 1] == ("id", "dropdown"):
                    found.append((text, ""))
    return found


def scan_tree(root: Path) -> list[tuple[str, str]]:
    """Every translatable text of a game, in the order it is first met: its
    C++ (src/, include/), its dialogue's line ids, and what a debug build saw."""
    seen: dict[tuple[str, str], None] = {}
    for folder in ("src", "include"):
        base = root / folder
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in (".cpp", ".h", ".hpp", ".cc"):
                continue
            for key, context in scan_source(path.read_text(encoding="utf-8", errors="replace")):
                if key:
                    seen.setdefault((key, context), None)
    resources = root / "resources"
    if resources.is_dir():
        for path in sorted(resources.rglob("*.yarn")):
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                for line_id in YARN_LINE_ID.findall(line):
                    seen.setdefault((f"line:{line_id}", ""), None)
    # Where a debug build runs from: next to its executable.
    for path in (root / "build/debug/lang_seen.tsv", root / "build/debug-drm/lang_seen.tsv"):
        if path.is_file():
            for row in parse_tsv(path.read_text(encoding="utf-8", errors="replace"))[1:]:
                if row and row[0]:
                    seen.setdefault((row[0], row[1] if len(row) > 1 else ""), None)
    return list(seen)


def yarn_texts(root: Path) -> dict[str, str]:
    """The English text of every Yarn line with an id: line:<id> -> the text."""
    out = {}
    resources = root / "resources"
    if not resources.is_dir():
        return out
    for path in sorted(resources.rglob("*.yarn")):
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = YARN_LINE_ID.search(line)
            if not m:
                continue
            out[f"line:{m.group(1)}"] = yarn_line_text(line)
    return out


def yarn_line_text(line: str) -> str:
    """What a Yarn line says, as its translation has to say it -- Yarn's own
    string table's form: no #tags, no -> or =>, no <<if ...>> after an
    option, and each {expression} numbered in order, {0}, {1}..., which is
    what the machine fills. "{$gold}" in a cell would be shown as written."""
    text = line[:line.find("#")] if "#" in line else line
    text = text.strip()
    text = re.sub(r"^(->|=>)\s*", "", text)
    text = re.sub(r"\s*<<.*?>>\s*$", "", text)
    count = 0

    def numbered(_m):
        nonlocal count
        count += 1
        return "{" + str(count - 1) + "}"

    return re.sub(r"(?<!\\)\{[^{}]*\}", numbered, text).strip()


# --- the tables -------------------------------------------------------------

def unescape(cell: str) -> str:
    out, i = [], 0
    while i < len(cell):
        if cell[i] == "\\" and i + 1 < len(cell):
            nxt = cell[i + 1]
            out.append({"t": "\t", "n": "\n", "\\": "\\"}.get(nxt, "\\" + nxt))
            i += 2
            continue
        out.append(cell[i])
        i += 1
    return "".join(out)


def escape(cell: str) -> str:
    return cell.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n").replace("\r", "")


def parse_tsv(text: str) -> list[list[str]]:
    """As src/rmp/text.cpp reads it: comments and blank lines skipped."""
    if text.startswith("\ufeff"):
        text = text[1:]
    rows = []
    for line in text.split("\n"):
        line = line.rstrip("\r")
        if not line or line.startswith("#"):
            continue
        rows.append([unescape(c) for c in line.split("\t")])
    return rows


def write_tsv(rows: list[list[str]]) -> str:
    return "".join("\t".join(escape(c) for c in row) + "\n" for row in rows)


def read_toml_lang(root: Path) -> dict:
    """[lang] of the game's raylib_multiplatform.toml, with its defaults."""
    lang = {"default": "en", "file": "lang.tsv", "folder": "lang", "languages": []}
    toml = root / "raylib_multiplatform.toml"
    if toml.is_file():
        try:
            import tomllib
        except ModuleNotFoundError:  # pragma: no cover - Python < 3.11
            import tomli as tomllib  # type: ignore
        data = tomllib.loads(toml.read_text(encoding="utf-8"))
        lang.update(data.get("lang", {}))
    return lang


class Wide:
    """A table with a column per language: key, context, codes..., unused."""

    def __init__(self, text: str = ""):
        rows = parse_tsv(text)
        self.header = rows[0] if rows else ["key", "context"]
        if len(self.header) < 2:
            self.header = ["key", "context"]
        self.rows = [r + [""] * (len(self.header) - len(r)) for r in rows[1:]]

    def codes(self) -> list[str]:
        return [c for c in self.header[2:] if LANG_CODE.match(c)]

    def column(self, name: str) -> int:
        if name not in self.header:
            self.header.append(name)
            for r in self.rows:
                r.append("")
        return self.header.index(name)

    def text(self) -> str:
        return write_tsv([self.header] + self.rows)


def scan(root: Path) -> str:
    """Add every text the game shows to its tables; say what was done."""
    lang = read_toml_lang(root)
    resources = root / "resources"
    resources.mkdir(exist_ok=True)
    table_path = resources / (lang["file"] or "lang.tsv")
    wide = Wide(table_path.read_text(encoding="utf-8") if table_path.is_file() else "")
    default = lang["default"]
    default_col = wide.column(default)
    for code in lang["languages"]:
        if code != default and not (resources / lang["folder"] / f"{code}.tsv").is_file():
            wide.column(code)
    unused_col = wide.column("unused")
    english = yarn_texts(root)
    found = scan_tree(root)
    wanted = set(found)
    have = {(r[0], r[1]): r for r in wide.rows}
    added = 0
    for key, context in found:
        if (key, context) in have:
            continue
        row = [""] * len(wide.header)
        row[0], row[1] = key, context
        row[default_col] = english.get(key, key)
        wide.rows.append(row)
        have[(key, context)] = row
        added += 1
    marked = 0
    for row in wide.rows:
        # language.name is read by rmp::lang::name(), never written in code.
        gone = (row[0], row[1]) not in wanted and row[0] != LANGUAGE_NAME
        new = "x" if gone else ""
        if row[unused_col] != new:
            marked += 1 if gone else 0
        row[unused_col] = new
    table_path.parent.mkdir(parents=True, exist_ok=True)
    table_path.write_text(wide.text(), encoding="utf-8")
    return (f"rmp text scan: {len(found)} text(s), {added} new row(s), "
            f"{marked} marked unused, in {table_path.relative_to(root)}")


def translations(root: Path) -> tuple[dict[tuple[str, str], dict[str, str]], list[str]]:
    """Every row of every table: (key, context) -> {code: text}; and the codes."""
    lang = read_toml_lang(root)
    resources = root / "resources"
    rows: dict[tuple[str, str], dict[str, str]] = {}
    codes: list[str] = []
    retired: set[tuple[str, str]] = set()  # marked unused: not held to anything
    table = resources / lang["file"] if lang["file"] else None
    if table is not None and table.is_file():
        wide = Wide(table.read_text(encoding="utf-8"))
        unused = wide.header.index("unused") if "unused" in wide.header else -1
        for c in wide.codes():
            codes.append(c)
        for r in wide.rows:
            if unused >= 0 and r[unused]:
                retired.add((r[0], r[1]))
                continue
            cells = rows.setdefault((r[0], r[1]), {})
            for i, c in enumerate(wide.header[2:], start=2):
                if LANG_CODE.match(c) and r[i]:
                    cells[c] = r[i]
    folder = resources / lang["folder"] if lang["folder"] else None
    if folder is not None and folder.is_dir():
        for path in sorted(folder.glob("*.tsv")):
            code = path.stem
            if code not in codes:
                codes.append(code)
            parsed = parse_tsv(path.read_text(encoding="utf-8"))
            start = 1 if parsed and parsed[0] and parsed[0][0] == "key" else 0
            for r in parsed[start:]:
                where = (r[0], r[1] if len(r) > 1 else "")
                if len(r) >= 3 and r[2] and where not in retired:
                    rows.setdefault(where, {})[code] = r[2]
    return rows, codes


def check(root: Path) -> list[str]:
    """Every mistake in the tables, a line each; empty when they are right."""
    lang = read_toml_lang(root)
    rows, codes = translations(root)
    if not rows and not codes:
        return []
    languages = list(lang["languages"]) or codes
    default = lang["default"]
    problems = []
    for (key, context), cells in rows.items():
        where = f"{key!r}" + (f" ({context})" if context else "")
        source = cells.get(default, key if not key.startswith("line:") else "")
        wanted = set(PLACEHOLDER.findall(source))
        plural = "|" in source and bool(wanted)
        for code in languages:
            text = cells.get(code)
            if text is None:
                if code == default and not key.startswith("line:"):
                    continue  # the key is the default language's text
                problems.append(f"missing: {where} has no {code} text")
                continue
            got = set(PLACEHOLDER.findall(text))
            for name in sorted(wanted - got):
                problems.append(f"placeholder: {where} in {code} lost {{{name}}}")
            for name in sorted(got - wanted):
                problems.append(f"placeholder: {where} in {code} has {{{name}}}, "
                                "which the text does not")
            if plural:
                base = code.split("-")[0]
                need = PLURAL_COUNTS.get(base, 0)
                forms = text.count("|") + 1
                if need and forms != need:
                    problems.append(f"plural: {where} in {code} has {forms} form(s); "
                                    f"{code} has {need}")
    return problems


def main(argv: list[str]) -> int:
    root = Path.cwd()
    if len(argv) != 1 or argv[0] not in ("scan", "check"):
        print("usage: rmp text scan|check", file=sys.stderr)
        return 2
    if argv[0] == "scan":
        print(scan(root))
        return 0
    problems = check(root)
    for p in problems:
        print(f"rmp text check: {p}")
    if problems:
        print(f"FAIL: {len(problems)} problem(s) in the translation tables")
        return 1
    print("rmp text check: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
