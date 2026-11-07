#!/usr/bin/env python3
"""ink_to_yarn: an Ink story as a Yarn Spinner script, for rmp::Dialogue.

    python3 tools/ink_to_yarn.py story.ink resources/story.yarn

Ink is read as written (the .ink source, no inklecate needed), and the part
of it that has a Yarn equivalent is converted, line by line:

    === knot ===          title: knot        (a node)
    = stitch              title: knot_stitch
    VAR gold = 5          <<declare $gold = 5>>     (in the first node)
    ~ gold = gold + 1     <<set $gold = $gold + 1>>
    Mika: Hello. # happy  Mika: Hello. #happy
    * [Ask] Ask nicely    -> Ask <<once>>           (* once; + every time)
    + {gold > 2} [Buy]    -> Buy <<if $gold > 2>>
    -> knot               <<jump knot>>
    -> END / -> DONE      <<stop>>
    {gold > 2: Rich|Poor} <<if $gold > 2>> Rich <<else>> Poor <<endif>>  (a line of its own)
    - (a gather)          the end of the choices above it

A choice's body is the lines after it, indented one level in the Yarn. What
Ink has and this does not convert -- nested choices (* *), labelled gathers,
tunnels (->->), threads (<-), lists, functions, sequences ({a|b|c} without a
condition), glue (<>) -- is a line of the report, with its line number, and
is left as a // comment in the output so nothing is lost silently.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

KNOT = re.compile(r"^\s*={2,}\s*([A-Za-z_][A-Za-z0-9_]*)\s*(\(.*\))?\s*=*\s*$")
STITCH = re.compile(r"^\s*=\s*([A-Za-z_][A-Za-z0-9_]*)\s*$")
VAR = re.compile(r"^\s*(VAR|CONST)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+?)\s*$")
TILDE = re.compile(r"^\s*~\s*(.+?)\s*$")
CHOICE = re.compile(r"^\s*([*+])\s*(.*)$")
DIVERT = re.compile(r"->\s*([A-Za-z_][A-Za-z0-9_.]*)")
GATHER = re.compile(r"^\s*-(?!>)\s*(.*)$")
INLINE_IF = re.compile(r"\{([^{}:|]+):([^{}]*)\}")


class Converter:
    def __init__(self) -> None:
        self.variables: set[str] = set()
        self.declarations: list[str] = []
        self.nodes: list[tuple[str, list[str]]] = []
        self.problems: list[str] = []
        self.knot = ""

    def expr(self, text: str) -> str:
        """An Ink expression in Yarn's words: variables get their $."""
        out = []
        for token in re.split(r'("[^"]*")', text):
            if token.startswith('"'):
                out.append(token)
                continue
            token = re.sub(r"\bnot\b", "!", token)
            token = re.sub(r"\btrue\b", "true", token)
            for name in sorted(self.variables, key=len, reverse=True):
                token = re.sub(rf"(?<![$\w.]){re.escape(name)}\b", f"${name}", token)
            out.append(token)
        return "".join(out).strip()

    def text(self, line: str, number: int) -> tuple[str, str]:
        """A line's text and its tags, with inline conditions made Yarn."""
        tags = ""
        if "#" in line:
            line, _, rest = line.partition("#")
            tags = " ".join("#" + t.strip().replace(" ", "_") for t in rest.split("#") if t.strip())
        if "<>" in line:
            self.problems.append(f"{number}: glue (<>) is not converted")
            line = line.replace("<>", "")
        line = line.strip()
        line = re.sub(r"\{([A-Za-z_][A-Za-z0-9_]*)\}", lambda m: "{" + self.expr(m.group(1)) + "}", line)
        return line, tags

    def node(self, name: str) -> list[str]:
        body: list[str] = []
        self.nodes.append((name, body))
        return body

    def convert(self, source: str) -> str:
        body = self.node("Start")
        in_choice = False
        lines = source.splitlines()
        # Variables first, so every expression knows them.
        for line in lines:
            m = VAR.match(line)
            if m:
                self.variables.add(m.group(2))
        for number, raw in enumerate(lines, start=1):
            line = raw.split("//", 1)[0].rstrip() if "//" in raw and '"' not in raw else raw.rstrip()
            if not line.strip():
                if not in_choice:
                    body.append("")
                continue
            if m := KNOT.match(line):
                self.knot = m.group(1)
                body = self.node(self.knot)
                in_choice = False
                if m.group(2):
                    self.problems.append(f"{number}: knot parameters are not converted")
                continue
            if m := STITCH.match(line):
                body = self.node(f"{self.knot}_{m.group(1)}" if self.knot else m.group(1))
                in_choice = False
                continue
            if m := VAR.match(line):
                self.declarations.append(f"<<declare ${m.group(2)} = {self.expr(m.group(3))}>>")
                continue
            if m := TILDE.match(line):
                stmt = m.group(1)
                if m2 := re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*(\+\+|--)$", stmt):
                    op = "+" if m2.group(2) == "++" else "-"
                    body.append(("    " if in_choice else "") + f"<<set ${m2.group(1)} {op}= 1>>")
                elif m2 := re.match(r"([A-Za-z_][A-Za-z0-9_]*)\s*([+\-*/]?=)\s*(.+)$", stmt):
                    body.append(("    " if in_choice else "") +
                                f"<<set ${m2.group(1)} {m2.group(2)} {self.expr(m2.group(3))}>>")
                else:
                    body.append(f"// {raw.strip()}")
                    self.problems.append(f"{number}: '~ {stmt}' is not converted")
                continue
            if m := CHOICE.match(line):
                sticky, rest = m.group(1) == "+", m.group(2)
                if rest.lstrip().startswith(("*", "+")):
                    self.problems.append(f"{number}: nested choices are not converted")
                    body.append(f"// {raw.strip()}")
                    continue
                cond = ""
                while (c := re.match(r"\s*\{([^{}]*)\}\s*", rest)):
                    cond = (cond + " && " if cond else "") + self.expr(c.group(1))
                    rest = rest[c.end():]
                shown = rest
                after = ""
                if (b := re.match(r"(.*?)\[(.*?)\](.*)$", rest)):
                    shown = b.group(1) + b.group(2)
                    after = (b.group(1) + b.group(3)).strip()
                divert = DIVERT.search(shown)
                target = ""
                if divert:
                    target = divert.group(1)
                    shown = shown[:divert.start()]
                    after = after.replace(divert.group(0), "")
                elif (d2 := DIVERT.search(after)):
                    target = d2.group(1)
                    after = after[:d2.start()]
                text, tags = self.text(shown, number)
                suffix = ""
                if cond and not sticky:
                    suffix = f" <<once if {cond}>>"
                elif cond:
                    suffix = f" <<if {cond}>>"
                elif not sticky:
                    suffix = " <<once>>"
                body.append(f"-> {text}{suffix}" + (f" {tags}" if tags else ""))
                if after.strip():
                    t, tg = self.text(after, number)
                    body.append(f"    {t}" + (f" {tg}" if tg else ""))
                if target:
                    body.append("    " + self.divert(target))
                in_choice = True
                continue
            if m := GATHER.match(line):
                in_choice = False
                rest = m.group(1)
                if rest.startswith("("):
                    self.problems.append(f"{number}: labelled gathers are not converted")
                    rest = re.sub(r"^\(\w+\)\s*", "", rest)
                body.append("")
                if rest.strip():
                    self.line(body, rest, number, False)
                continue
            if "->->" in line or line.strip().startswith("<-"):
                self.problems.append(f"{number}: tunnels and threads are not converted")
                body.append(f"// {raw.strip()}")
                continue
            self.line(body, line, number, in_choice)
        return self.write()

    def divert(self, target: str) -> str:
        if target in ("END", "DONE"):
            return "<<stop>>"
        return f"<<jump {target.replace('.', '_')}>>"

    def line(self, body: list[str], line: str, number: int, indent: bool) -> None:
        pad = "    " if indent else ""
        # {cond: a line|another}: on a line of its own, an <<if>> block.
        if (whole := re.match(r"^\s*\{([^{}:|]+):([^{}]*)\}\s*$", line)):
            yes, _, no = whole.group(2).partition("|")
            body.append(pad + f"<<if {self.expr(whole.group(1))}>>")
            self.line(body, yes, number, indent)
            if no.strip():
                body.append(pad + "<<else>>")
                self.line(body, no, number, indent)
            body.append(pad + "<<endif>>")
            return
        if INLINE_IF.search(line):
            self.problems.append(f"{number}: a condition inside a line is not converted")
            body.append(pad + "// " + line.strip())
            return
        divert = DIVERT.search(line)
        before = line[:divert.start()] if divert else line
        if before.strip():
            text, tags = self.text(before, number)
            body.append(pad + text + (f" {tags}" if tags else ""))
        if divert:
            body.append(pad + self.divert(divert.group(1)))

    def write(self) -> str:
        out = []
        first = True
        for name, body in self.nodes:
            lines = [l for l in body]
            while lines and not lines[0].strip():
                lines.pop(0)
            while lines and not lines[-1].strip():
                lines.pop()
            if name == "Start" and not lines and not first:
                continue
            if name == "Start" and not lines and len(self.nodes) > 1:
                # An Ink story begins at its top; with nothing there, at the first knot.
                lines = [self.divert(self.nodes[1][0])]
            out.append(f"title: {name}\n---")
            if first:
                out.extend(self.declarations)
            out.extend(lines)
            out.append("===")
            first = False
        return "\n".join(out) + "\n"


def convert(source: str) -> tuple[str, list[str]]:
    c = Converter()
    yarn = c.convert(source)
    return yarn, c.problems


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: python3 tools/ink_to_yarn.py story.ink story.yarn", file=sys.stderr)
        return 2
    source = Path(argv[0]).read_text(encoding="utf-8")
    yarn, problems = convert(source)
    Path(argv[1]).write_text(yarn, encoding="utf-8")
    for p in problems:
        print(f"ink_to_yarn: {argv[0]}:{p}")
    print(f"ink_to_yarn: wrote {argv[1]} ({len(problems)} thing(s) not converted)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
