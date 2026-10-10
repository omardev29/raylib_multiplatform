#!/usr/bin/env python3
"""No raw pointer and no C string in a declaration a game can name.

    python3 tools/pointer_check.py              every include/rmp/*.h
    python3 tools/pointer_check.py --include DIR --ratchet FILE
                                                the headers DIR/rmp/*.h (the tests' fixtures)

The rule, Omar's of 2026-10-10: what rmp:: hands a game is values, references,
std::span, std::optional, rmp::Ref and rmp::Handle -- never a `T *` and never a
`const char *`. A pointer in the API asks the reader whether it may be null,
who owns it and how long it lives, and the type cannot answer any of the
three; the replacements each answer all of them.

HOW. clang reads each header on its own and dumps its declarations as JSON
(-ast-dump=json, filtered to names with `rmp` in them), and this walks them:
every function's return and parameters, every field, every variable at
namespace or class scope, every type alias. A type spelt with a `*` -- or that
an alias hides one behind, which is why the desugared type is read too -- or a
`char[N]`, which is a C string with its length welded on, is a finding. Reading
the AST and not the text is the point: a grep cannot tell a pointer in a
comment from one in a declaration, a public member from a private one, or see
through `using SampleFn = ...`.

WHERE IT IS ALLOWED, and nowhere else:

  operator->   rmp::Handle's and rmp::Ref's: the language requires it to
               return a pointer, and it is how `h->hp` reads.
  rmp::Ref     its one private member IS the optional reference; the type
               exists so that nothing else has to hold one.
  the ratchet  tools/pointer_ratchet.txt: pointers in a `detail` namespace or
               a private/protected member, each with its reason. They are not
               a game's to name; they are listed so that the list can only
               shrink. A new one fails until it is either replaced or written
               down, and a line that matches nothing any more fails too, so
               the list cannot keep a debt that was paid.

Bodies are not read: a pointer inside an inline function is implementation,
and the lint job's clang-tidy is what looks there.

Exit: 0 clean (or no clang++ to read with, said so -- RMP_REQUIRE_CLANG=1 makes
that a failure, as CI sets it); 1 a finding, a dead ratchet line, or a header
clang could not read.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RATCHET = REPO / "tools" / "pointer_ratchet.txt"

# The include set tools/header_check.sh compiles each header with.
INCLUDES = ["include", "thirdparty/raylib/src", "thirdparty/rres", "thirdparty/raymob",
            "thirdparty/clay", "thirdparty"]
DEFINES = ['-DRMP_RESOURCES_PATH="./resources/"', "-DRMP_PRODUCTION_BUILD=0"]

# Exempt by name, each with the reason. Nothing else is.
EXEMPT = {
    "rmp::Handle::operator->": "operator-> must return a pointer; it is how `h->hp` reads",
    "rmp::Ref::operator->": "operator-> must return a pointer; it is how `r->hp` reads",
    "rmp::Ref::_target": "rmp::Ref's inside: the optional reference itself",
}

FUNCTIONS = {"FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl", "CXXConversionDecl",
             "CXXDestructorDecl", "CXXDeductionGuideDecl"}
RECORDS = {"CXXRecordDecl", "ClassTemplatePartialSpecializationDecl"}
C_STRING = re.compile(r"\bchar\s*\[")
ENTRY = re.compile(r"^(\S+)\s+(\S+)\s+--\s+(\S.*)$")

HINT = ("a raw pointer or a C string in rmp's API: a value, a reference, std::span, "
        "std::optional, std::string_view in and std::string out, rmp::Ref for an "
        "optional reference, rmp::Handle across frames")


class Finding:
    def __init__(self, file: str, line: int, name: str, role: str, type_: str,
                 public: bool):
        self.file, self.line, self.name = file, line, name
        self.role, self.type, self.public = role, type_, public

    def key(self):
        return (self.file, self.line, self.name, self.role)

    def __str__(self):
        return f"{self.file}:{self.line}: {self.name} -- {self.role} `{self.type}`"


def bad(type_obj) -> str | None:
    """The spelling that makes a type a finding, or None."""
    if not isinstance(type_obj, dict):
        return None
    for field in ("qualType", "desugaredQualType"):
        spelt = type_obj.get(field, "")
        if "*" in spelt or C_STRING.search(spelt):
            return type_obj.get("qualType", spelt)
    return None


def split_function_type(spelt: str) -> str:
    """The return type out of a function type's spelling: `Slot *(int, T) const`
    is `Slot *`. The parameter list is the last balanced (...) before the
    qualifiers, which a function-pointer return type nests inside of."""
    s = spelt.rstrip()
    for tail in (" noexcept", " &&", " &", " const", " volatile"):
        while s.endswith(tail):
            s = s[: -len(tail)].rstrip()
    if not s.endswith(")"):
        return s
    depth = 0
    for i in range(len(s) - 1, -1, -1):
        if s[i] == ")":
            depth += 1
        elif s[i] == "(":
            depth -= 1
            if depth == 0:
                return s[:i].rstrip()
    return s


class Walker:
    """One header's JSON AST, in the order clang printed it. Locations are
    printed with the file and line left out when they did not change, so the
    walk keeps the last ones it saw -- exactly as the printer did."""

    def __init__(self, root: Path, aliases: dict[str, str]):
        self.root = root.resolve()
        self.file = ""
        self.line = 0
        self.findings: list[Finding] = []
        self.aliases = aliases  # alias name -> what makes it bad, across headers
        # Every namespace and class by its id: a declaration written outside
        # the class it belongs to -- `class Value::Ref {`, `B Object::get()` --
        # names it by id, and its scope is that one and not where it stands.
        self.contexts: dict[str, tuple[str, bool, bool]] = {}  # id -> (name, public, record)
        self.declared_public: dict[str, bool] = {}  # a class's access where it was declared

    # ---- locations --------------------------------------------------------
    def bare(self, loc):
        if "file" in loc:
            self.file = loc["file"]
        if "line" in loc:
            self.line = loc["line"]

    def location(self, loc):
        if not isinstance(loc, dict):
            return
        if "spellingLoc" in loc:
            self.bare(loc["spellingLoc"])
            self.bare(loc.get("expansionLoc", {}))
        else:
            self.bare(loc)

    def track(self, obj):
        """Every location inside a value, in order, for the state's sake."""
        if isinstance(obj, dict):
            if "offset" in obj or "spellingLoc" in obj:
                self.location(obj)
                return
            for key, value in obj.items():
                if key == "includedFrom":
                    continue
                self.track(value)
        elif isinstance(obj, list):
            for value in obj:
                self.track(value)

    def ours(self) -> bool:
        path = Path(self.file)
        if not path.is_absolute():
            path = self.root.parent / path  # clang ran there
        try:
            rel = path.resolve().relative_to(self.root)
        except ValueError:
            return False
        return rel.parts[:1] == ("rmp",)

    def shown(self) -> str:
        """The file as a person reads it: from the repository when it is in it."""
        path = Path(self.file)
        if not path.is_absolute():
            path = self.root.parent / path
        try:
            return str(path.resolve().relative_to(REPO))
        except ValueError:
            return str(path)

    # ---- declarations -----------------------------------------------------
    def report(self, name: str, role: str, type_: str, public: bool, at):
        if name in EXEMPT:
            return
        self.findings.append(Finding(at[0], at[1], name, role, type_, public))

    def visit(self, node, scope: list[str], public: bool, inspect: bool):
        """`public`: a game can name it -- outside every `detail` namespace and
        not behind private or protected. `inspect`: a declaration to judge, as
        opposed to a function body or an instantiation the compiler made."""
        if not isinstance(node, dict):
            return
        at = None
        for key, value in node.items():
            if key == "loc":
                self.location(value)
                at = (self.shown(), self.line)
            elif key != "inner":
                self.track(value)
        if at is None:
            at = (self.shown(), self.line)
        kind = node.get("kind", "")
        name = node.get("name", "")
        # A class template's constructor is named `Resource<T, K>`; it is the
        # same declaration as `Resource` for every purpose here.
        if not name.startswith("operator"):
            name = re.sub(r"<.*>$", "", name)
        inner = node.get("inner", [])
        outside = self.contexts.get(node.get("parentDeclContextId", ""))
        if outside is not None:
            scope = outside[0].split("::") if outside[0] else []
            if kind in FUNCTIONS or kind == "FunctionTemplateDecl":
                # A member defined outside its class: the declaration inside
                # the class was judged already, with the access it has there.
                inspect = inspect and not outside[2]
        judged = inspect and not node.get("isImplicit", False) and self.ours()
        qualified = "::".join([*scope, name] if name else scope)

        if kind == "NamespaceDecl":
            inside = public and name != "detail"
            self.contexts[node.get("id", "")] = (qualified, inside, False)
            for child in inner:
                self.visit(child, [*scope, name or "(anonymous)"], inside, inspect)
            return
        if kind in ("LinkageSpecDecl", "ExportDecl"):
            for child in inner:
                self.visit(child, scope, public, inspect)
            return
        if kind in RECORDS or kind == "ClassTemplateSpecializationDecl":
            # An instantiation is the compiler's copy of a pattern already read.
            keep = inspect and kind != "ClassTemplateSpecializationDecl" and not node.get(
                "isImplicit", False)
            access = "private" if node.get("tagUsed") == "class" else "public"
            inner_scope = [*scope, name or "(anonymous)"]
            if outside is not None:
                public = self.declared_public.get(qualified, public)
            elif name:
                self.declared_public.setdefault(qualified, public)
            self.contexts[node.get("id", "")] = ("::".join(inner_scope), public, True)
            for child in inner:
                if child.get("kind") == "AccessSpecDecl":
                    self.track({k: v for k, v in child.items() if k != "inner"})
                    access = child.get("access", access)
                    continue
                self.visit(child, inner_scope, public and access == "public", keep)
            return
        if kind == "ClassTemplateDecl":
            for child in inner:
                self.visit(child, scope, public, inspect)
            return
        if kind == "FunctionTemplateDecl":
            # The pattern comes first; what follows it is instantiations.
            seen = False
            for child in inner:
                is_function = child.get("kind") in FUNCTIONS
                self.visit(child, scope, public, inspect and not (is_function and seen))
                seen = seen or is_function
            return
        if kind == "TypeAliasTemplateDecl":
            for child in inner:
                self.visit(child, scope, public, inspect)
            return
        if kind == "FriendDecl":
            # A friend function declared in a class is callable by anyone.
            for child in inner:
                self.visit(child, scope[:-1], True if judged else public, inspect)
            return
        if kind in FUNCTIONS:
            if judged:
                spelt = node.get("type", {}).get("qualType", "")
                returned = split_function_type(spelt)
                hidden = self.aliases.get(re.sub(r"^const |[ &]+$", "", returned).strip())
                if "*" in returned or C_STRING.search(returned) or hidden:
                    self.report(qualified, "returns", returned, public, at)
            for child in inner:
                if child.get("kind") == "ParmVarDecl":
                    self.visit_param(child, qualified, public, judged)
                else:
                    self.visit(child, scope, public, False)  # the body
            return
        if kind in ("FieldDecl", "VarDecl", "TypeAliasDecl", "TypedefDecl"):
            if judged:
                spelt = bad(node.get("type"))
                if spelt is not None:
                    role = {"FieldDecl": "field", "VarDecl": "variable"}.get(kind, "alias")
                    self.report(qualified, role, spelt, public, at)
                    if role == "alias":
                        self.aliases[name] = spelt
            for child in inner:
                self.visit(child, scope, public, False)  # the initialiser
            return
        for child in inner:
            self.visit(child, scope, public, False)

    def visit_param(self, node, function: str, public: bool, judged: bool):
        at = None
        for key, value in node.items():
            if key == "loc":
                self.location(value)
                at = (self.shown(), self.line)
            elif key != "inner":
                self.track(value)
        spelt = bad(node.get("type")) if judged else None
        if spelt is not None:
            label = node.get("name") or "(unnamed)"
            self.report(function, f"parameter {label}", spelt, public,
                        at or (self.shown(), self.line))
        for child in node.get("inner", []):
            self.visit(child, [], public, False)  # the default argument


def documents(text: str):
    """-ast-dump-filter prints one JSON document per declaration it matched."""
    decoder = json.JSONDecoder()
    i, n = 0, len(text)
    while i < n:
        while i < n and text[i].isspace():
            i += 1
        if i >= n:
            break
        if text[i] != "{":
            end = text.find("\n", i)
            i = n if end < 0 else end + 1
            continue
        doc, i = decoder.raw_decode(text, i)
        yield doc


def dump(clang: str, include: Path, includes: list[str], header: Path, tmp: Path):
    unit = tmp / f"{header.stem}.cpp"
    unit.write_text(f"#include <rmp/{header.name}>\n")
    argv = [clang, "-std=c++20", "-fsyntax-only", "-Xclang", "-ast-dump=json",
            "-Xclang", "-ast-dump-filter=rmp", *[f"-I{i}" for i in includes], *DEFINES,
            str(unit)]
    got = subprocess.run(argv, capture_output=True, text=True, cwd=include.parent)
    return header, got


def read_ratchet(path: Path):
    """(header, qualified name) -> reason, and the problems with the file."""
    entries, problems = {}, []
    if not path.exists():
        return entries, problems
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        m = ENTRY.match(line)
        if m is None:
            problems.append(f"{path.name}:{number}: `header name -- reason`, and the "
                            f"reason is not optional: `{line}`")
            continue
        if (m.group(1), m.group(2)) in entries:
            problems.append(f"{path.name}:{number}: `{m.group(1)} {m.group(2)}` is "
                            "listed twice")
        entries[(m.group(1), m.group(2))] = m.group(3)
    return entries, problems


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--include", type=Path, default=REPO / "include",
                        help="the directory holding rmp/*.h (default: include/)")
    parser.add_argument("--ratchet", type=Path, default=RATCHET)
    args = parser.parse_args(argv)

    clang = os.environ.get("RMP_CLANGXX") or shutil.which("clang++")
    if clang is None:
        if os.environ.get("RMP_REQUIRE_CLANG") == "1":
            print("FAIL: no clang++ to read the headers with, and RMP_REQUIRE_CLANG=1")
            return 1
        print("  skip  no clang++ on PATH: the public headers' pointers were not read")
        return 0

    include = args.include.resolve()
    real = include == (REPO / "include").resolve()
    if real and not (include / "rmp" / "generated" / "config.h").exists():
        subprocess.run([sys.executable, str(REPO / "tools" / "configure.py")],
                       capture_output=True, cwd=REPO, check=False)
    includes = [str(REPO / i) for i in INCLUDES] if real else [str(include)]
    headers = sorted(include.glob("rmp/*.h"))
    if not headers:
        print(f"FAIL: no headers under {include}/rmp")
        return 1

    failed = False
    findings: dict[tuple, Finding] = {}
    aliases: dict[str, str] = {}
    with tempfile.TemporaryDirectory() as tmp, ThreadPoolExecutor() as pool:
        results = list(pool.map(lambda h: dump(clang, include, includes, h, Path(tmp)),
                                headers))
    for header, got in results:
        if got.returncode != 0:
            print(f"FAIL: clang could not read rmp/{header.name}:")
            print("\n".join("      " + l for l in got.stderr.splitlines()[:8]))
            failed = True
            continue
        walker = Walker(include, aliases)
        for doc in documents(got.stdout):
            walker.visit(doc, [], True, True)
        for finding in walker.findings:
            findings.setdefault(finding.key(), finding)

    entries, problems = read_ratchet(args.ratchet)
    for problem in problems:
        print(f"FAIL: {problem}")
        failed = True

    used = set()
    public = [f for f in findings.values() if f.public]
    private = [f for f in findings.values() if not f.public]
    for finding in sorted(public, key=lambda f: (f.file, f.line)):
        print(f"FAIL: {finding}")
        failed = True
    for finding in sorted(private, key=lambda f: (f.file, f.line)):
        entry = (Path(finding.file).name, finding.name)
        if entry in entries:
            used.add(entry)
            continue
        print(f"FAIL: {finding}")
        print(f"      in detail or private, and not in {args.ratchet.name}: replace it, "
              "or add `header name -- reason` there")
        failed = True
    for entry in entries:
        if entry not in used:
            listed_public = any((Path(f.file).name, f.name) == entry for f in public)
            why = ("it is public, and the ratchet is for detail and private members only"
                   if listed_public else "it matches nothing any more: remove the line")
            print(f"FAIL: {args.ratchet.name}: `{entry[0]} {entry[1]}` -- {why}")
            failed = True

    if failed:
        print()
        print(f"FAIL: {HINT}.")
        return 1
    count = f"{len(headers)} public header{'s' if len(headers) != 1 else ''}"
    print(f"  ok    {count}: no raw pointer and no C string a game can name "
          f"({len(used)} ratchet lines, each still owed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
