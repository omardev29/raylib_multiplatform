#!/usr/bin/env python3
"""rmp yarn: the game's Yarn scripts, held to its C++.

    rmp yarn check [FILE.yarn ...]   compile every script; what it calls, registered?
    rmp yarn ysls                    write rmp.ysls.json for the Yarn editor

`check` compiles each .yarn of resources/ (and, in the framework, of every
example) with the framework's own compiler -- rmp_yarnc --check, the one a
game carries -- and says each error as file:line:column. The functions the
game's C++ registers with a type (`on_function("has_item", [&](std::string
item) -> bool {...})`) are told to the compiler, so a script that calls one
with a number where a string goes fails here, not in the game. Then each
function a script calls that nothing in the C++ registers is an error -- the
game would find it when the line runs -- and each command nothing handles is
a warning: a game may read it itself, through has_command().

`ysls` writes, next to each project's resources/, an `rmp.ysls.json`: the
commands and functions its C++ registers, with their parameters' types, their
result and the `//` comment above each, in the format the Yarn Spinner
extension for VS Code reads (its language server's .ysls.json), so the
editor completes them, shows their documentation and checks their arguments.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

CPP_SUFFIXES = (".cpp", ".h", ".hpp", ".cc")
# The commands the dialogue runs itself when the game has none of the name.
BUILTIN_COMMANDS = ("wait", "stop")
REGISTRATION = re.compile(r"\bon_(function|command)\s*\(")
NUMBER_WORDS = ("float", "double", "int", "long", "short", "unsigned", "size_t", "int8_t",
                "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
                "uint64_t", "char")


@dataclass
class Param:
    name: str
    type: str  # number, string, bool or any


@dataclass
class Registered:
    kind: str  # "function" or "command"
    name: str
    file: str
    line: int
    params: list[Param] = field(default_factory=list)
    result: str = "any"
    typed: bool = True  # False: the raw form, every argument as written
    doc: str = ""


# --- the C++ scanner --------------------------------------------------------

def _strip_comments(source: str) -> str:
    """The source with comments blanked (same length, newlines kept), so
    positions still match; string literals are left alone."""
    out = list(source)
    i, n = 0, len(source)
    while i < n:
        c = source[i]
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n and source[i] != quote:
                i += 2 if source[i] == "\\" else 1
            i += 1
        elif source.startswith("//", i):
            while i < n and source[i] != "\n":
                out[i] = " "
                i += 1
        elif source.startswith("/*", i):
            while i < n and not source.startswith("*/", i):
                if source[i] != "\n":
                    out[i] = " "
                i += 1
            for k in range(i, min(i + 2, n)):
                out[k] = " "
            i += 2
        else:
            i += 1
    return "".join(out)


def _balanced(text: str, at: int, open_c: str, close_c: str) -> int:
    """The index just past the bracket that closes the one at `at`."""
    depth = 0
    i = at
    while i < len(text):
        c = text[i]
        if c == '"':
            i += 1
            while i < len(text) and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif c == open_c:
            depth += 1
        elif c == close_c:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return -1


def _split_top(text: str) -> list[str]:
    parts, depth, cur = [], 0, ""
    for c in text:
        if c in "<([{":
            depth += 1
        elif c in ">)]}":
            depth -= 1
        if c == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += c
    if cur.strip():
        parts.append(cur)
    return parts


def yarn_type(cpp: str) -> str:
    """The Yarn type a C++ parameter or result type is read as."""
    t = re.sub(r"\b(const|volatile|std::|rmp::)\b", " ", cpp)
    t = t.replace("&", " ").replace("*", " ").replace("std::", "").replace("rmp::", "")
    words = t.split()
    if not words:
        return "any"
    if "string" in words or "string_view" in words:
        return "string"
    if "bool" in words:
        return "bool"
    if any(w in NUMBER_WORDS for w in words):
        return "number"
    return "any"


def _param(text: str, index: int) -> Param | None:
    text = text.split("=")[0].strip()
    if not text:
        return None
    m = re.match(r"^(.*?)([A-Za-z_][A-Za-z0-9_]*)$", text)
    if m and m.group(1).strip() and not m.group(1).strip().endswith("::"):
        return Param(m.group(2), yarn_type(m.group(1)))
    return Param(f"arg{index + 1}", yarn_type(text))


def _doc_above(source: str, offset: int) -> str:
    """The // comment lines just above the line `offset` is on."""
    lines = source[:offset].split("\n")
    lines.pop()  # the call's own line
    doc = []
    while lines and lines[-1].strip().startswith("//"):
        doc.append(lines.pop().strip()[2:].strip())
    return " ".join(reversed(doc)).strip()


def scan_source(source: str, file: str) -> list[Registered]:
    code = _strip_comments(source)
    out = []
    for m in REGISTRATION.finditer(code):
        kind = m.group(1)
        rest = code[m.end():]
        name = re.match(r'\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*', rest)
        if not name:
            continue
        at = m.end() + name.end()
        reg = Registered(kind, name.group(1), file, code.count("\n", 0, m.start()) + 1,
                         doc=_doc_above(source, m.start()))
        if at < len(code) and code[at] == "[":
            end = _balanced(code, at, "[", "]")
            after = code[end:] if end > 0 else ""
            sig = re.match(r"\s*\(", after)
            params_text = ""
            tail = after
            if sig:
                p_from = end + sig.end() - 1
                p_to = _balanced(code, p_from, "(", ")")
                params_text = code[p_from + 1:p_to - 1]
                tail = code[p_to:]
            raw = ("span" in params_text and (kind == "command" or "DialogueValue" in params_text)) \
                or re.search(r"\bauto\b", params_text) is not None
            if raw:
                reg.typed = False
            else:
                reg.params = [p for i, t in enumerate(_split_top(params_text))
                              if (p := _param(t, i)) is not None]
                ret = re.match(r"\s*(?:mutable\s*)?(?:noexcept\s*)?->\s*([A-Za-z_:<> ]+?)\s*\{",
                               tail)
                if ret:
                    reg.result = yarn_type(ret.group(1))
        else:
            reg.typed = False  # a callable kept in a variable: nothing to read
        if kind == "command":
            reg.result = ""
        out.append(reg)
    return out


def scan_cpp(project: Path, root: Path) -> list[Registered]:
    found: dict[tuple[str, str], Registered] = {}
    for folder in ("src", "include"):
        base = project / folder
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.suffix not in CPP_SUFFIXES or not path.is_file():
                continue
            # The framework's own src/rmp is not a game's code.
            if project == root and "rmp" in path.relative_to(base).parts[:1]:
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for r in scan_source(text, path.relative_to(root).as_posix()):
                found.setdefault((r.kind, r.name), r)
    return list(found.values())


# --- the projects and their scripts -------------------------------------------

def projects(root: Path) -> dict[Path, list[Path]]:
    """Each project (the root, and in the framework each example) and its
    resources/**/*.yarn."""
    out: dict[Path, list[Path]] = {}
    candidates = [root]
    examples = root / "examples"
    if examples.is_dir():
        candidates += sorted(p.parent for p in examples.rglob("resources") if p.is_dir())
    for project in candidates:
        scripts = sorted((project / "resources").rglob("*.yarn")) \
            if (project / "resources").is_dir() else []
        if scripts:
            out[project] = scripts
    return out


def _where(script: Path, pattern: str) -> int:
    for n, line in enumerate(script.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        if re.search(pattern, line):
            return n
    return 1


def function_spec(r: Registered) -> str:
    return f"{r.name}={','.join(p.type for p in r.params)}>{r.result}"


def check(root: Path, yarnc: str, only: list[str]) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []
    wanted = {Path(f).resolve() for f in only}
    for project, scripts in projects(root).items():
        if wanted:
            scripts = [s for s in scripts if s.resolve() in wanted]
            if not scripts:
                continue
        registered = scan_cpp(project, root)
        functions = {r.name: r for r in registered if r.kind == "function"}
        commands = {r.name for r in registered if r.kind == "command"}
        argv = [yarnc, "--check"]
        for r in functions.values():
            if r.typed:
                argv += ["--function", function_spec(r)]
        argv += [s.relative_to(root).as_posix() for s in scripts]
        got = subprocess.run(argv, cwd=root, capture_output=True, text=True,
                             stdin=subprocess.DEVNULL, check=False)
        for line in got.stderr.splitlines():
            if line.strip() and not line.startswith("rmp_yarnc: "):
                errors.append(line)
            elif "cannot read" in line or "--function" in line:
                errors.append(line)
        for line in got.stdout.splitlines():
            parts = line.split(" ", 2)
            if len(parts) != 3:
                continue
            what, file, name = parts
            script = root / file
            if what == "function" and name not in functions:
                n = _where(script, rf"\b{re.escape(name)}\s*\(")
                errors.append(f"{file}:{n}: the script calls {name}(), and the C++ of "
                              f"{project.relative_to(root).as_posix() or '.'} registers no "
                              f'on_function("{name}", ...)')
            elif what == "command" and name not in commands and name not in BUILTIN_COMMANDS:
                n = _where(script, rf"<<\s*{re.escape(name)}\b")
                warnings.append(f"{file}:{n}: <<{name}>> has no on_command(\"{name}\", ...) "
                                f"in the C++: it waits in has_command() for the game")
        if got.returncode not in (0, 1):
            errors.append(f"rmp_yarnc --check exited with {got.returncode}")
    return errors, warnings


# --- the editor's file ----------------------------------------------------------

def ysls_entry(r: Registered) -> dict:
    if r.typed:
        params = [{"Name": p.name, "Type": p.type, "Documentation": "",
                   "IsParamsArray": False} for p in r.params]
    else:
        params = [{"Name": "args", "Type": "any", "Documentation": "every argument, as "
                   "written", "IsParamsArray": True}]
    if r.kind == "function":
        signature = f"{r.name}({', '.join(p['Name'] for p in params)})"
    else:
        signature = " ".join([r.name] + [p["Name"] for p in params])
    entry = {"YarnName": r.name, "DefinitionName": r.name, "FileName": r.file,
             "Language": "cpp", "Documentation": r.doc, "Signature": signature,
             "Parameters": params}
    if r.kind == "function":
        entry["ReturnType"] = r.result or "any"
    return entry


def ysls(project: Path, root: Path) -> dict:
    registered = scan_cpp(project, root)
    return {
        "Commands": [ysls_entry(r) for r in registered if r.kind == "command"],
        "Functions": [ysls_entry(r) for r in registered if r.kind == "function"],
    }


def write_ysls(root: Path) -> list[Path]:
    written = []
    for project in projects(root) or {root: []}:
        path = project / "rmp.ysls.json"
        path.write_text(json.dumps(ysls(project, root), indent=4) + "\n", encoding="utf-8")
        written.append(path)
    return written


def main(argv: list[str]) -> int:
    root = Path.cwd()
    if not argv or argv[0] not in ("check", "ysls"):
        print("usage: rmp yarn check [FILE.yarn ...] | rmp yarn ysls", file=sys.stderr)
        return 2
    if argv[0] == "ysls":
        for path in write_ysls(root):
            print(f"rmp yarn ysls: wrote {path.relative_to(root).as_posix()}")
        return 0
    args = argv[1:]
    yarnc = "build/debug/rmp_yarnc"
    if len(args) >= 2 and args[0] == "--yarnc":
        yarnc, args = args[1], args[2:]
    if not projects(root):
        print("rmp yarn check: no .yarn under resources/: nothing to check")
        return 0
    errors, warnings = check(root, str(Path(yarnc).resolve()), args)
    for w in warnings:
        print(f"warning: {w}")
    for e in errors:
        print(e)
    if errors:
        print(f"FAIL: {len(errors)} problem(s) in the Yarn scripts")
        return 1
    print("rmp yarn check: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
