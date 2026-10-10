"""C and C++ read as text, for the gates that must see every #if branch.

clang-tidy parses the one branch of every #if that the machine running it
compiles: the web, iOS, Android and Windows code is invisible to a lint run on
Linux. tools/naming_check.sh and tools/style_check.sh restate their rules over
the text instead, and both need the same first step: the comments, strings and
character literals turned into spaces, so that a comment may say what is
forbidden and a string may hold it.
"""

from __future__ import annotations

import pathlib


def blank(text: str, comments: bool = True, strings: bool = True) -> str:
    """The text with comments (when `comments`) and string and character
    literals (when `strings`) turned into spaces, newlines kept, so every
    offset and line number still holds.

    A scanner and not a regex, because the cases interleave: a `//` inside a
    string is not a comment, a quote inside a comment is not a string, a raw
    string R"x( ... )x" may hold both, and `1'000` is a digit separator rather
    than the start of a character literal. What is not blanked is still
    scanned, so a kept comment still hides the quotes inside it."""
    out = list(text)
    n = len(text)
    i = 0

    def wipe(a, b, really):
        if not really:
            return
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            wipe(i, j, comments)
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            wipe(i, j, comments)
            i = j
        elif c == '"':
            # A raw string: R"delim( ... )delim", with an optional prefix.
            if i > 0 and text[i - 1] == "R" and (i < 2 or not (text[i - 2].isalnum() or text[i - 2] == "_")
                                                 or text[i - 2:i] in ("8R", "LR", "uR", "UR")):
                paren = text.find("(", i)
                delim = text[i + 1:paren]
                end = text.find(")" + delim + '"', paren)
                j = n if end < 0 else end + len(delim) + 2
                wipe(i, j, strings)
                i = j
                continue
            j = i + 1
            while j < n and text[j] != '"' and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            wipe(i, min(j + 1, n), strings)
            i = j + 1
        elif c == "'":
            # A digit separator when the token before it is a number.
            k = i - 1
            while k >= 0 and (text[k].isalnum() or text[k] in "_."):
                k -= 1
            token = text[k + 1:i]
            if token[:1].isdigit():
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != "'" and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            wipe(i, min(j + 1, n), strings)
            i = j + 1
        else:
            i += 1
    return "".join(out)


class Lines:
    """Offset -> 1-based line number, for a text whose newlines blank() kept."""

    def __init__(self, text: str):
        self.starts = [0]
        for k, ch in enumerate(text):
            if ch == "\n":
                self.starts.append(k + 1)

    def of(self, offset: int) -> int:
        lo, hi = 0, len(self.starts)
        while lo + 1 < hi:
            mid = (lo + hi) // 2
            if self.starts[mid] <= offset:
                lo = mid
            else:
                hi = mid
        return lo + 1


SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cpp"}


def tree(roots=("include", "src", "tests", "examples")) -> list[pathlib.Path]:
    """Our C and C++ under `roots`, minus generated headers and tests/fixtures/
    (third-party-shaped files kept for the licence tests)."""
    files = []
    for root in roots:
        for path in sorted(pathlib.Path(root).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            posix = path.as_posix()
            if "/generated/" in posix or posix.startswith("tests/fixtures/"):
                continue
            files.append(path)
    return files
