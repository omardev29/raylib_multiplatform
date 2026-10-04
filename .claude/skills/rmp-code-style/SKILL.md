---
name: rmp-code-style
description: The house style for raylib_multiplatform — naming, formatting, what modern C++ is used for, when macros are fine, and how comments are written. Load before writing or editing any C++, CMake, Python or TOML in raylib_multiplatform/.
---

# House style

## Naming

Not "everything is snake_case" — that was the old rule and it was replaced,
because at the point of use it made a type and a value look the same.

| Kind | Case | Example |
|---|---|---|
| Types — class, struct, enum, alias | `PascalCase` | `rmp::ui::ButtonOptions`, `rmp::Object` |
| Enum members | `SCREAM_CASE` | `rmp::ui::Align::TOP_LEFT` |
| Functions and methods | `snake_case` | `rmp::ui::current_theme()` |
| Variables, parameters, fields | `snake_case` | `min_height`, `delta` |
| Namespaces | `snake_case` | `rmp::ui::detail` |
| Macros | `SCREAM_CASE` | `RMP_ENTRY_POINT` |
| Compile-time constants | `kPascalCase` | `kMaxLabels` |
| File-scope mutable state | `g_snake_case` | `g_frame_open` |

`Align::TOP_LEFT` is unmistakably a constant of a type; `align::top_left` could
have been a member of a variable called `align`. raylib's own types are
PascalCase too, so `rmp::Texture` sits next to `Texture2D` without looking
foreign — the **namespace** is what says whose is whose, not the case.

All of it is enforced by `readability-identifier-naming` in `.clang-tidy`, so it
is checked rather than remembered.

**The access rule.** Two ways the framework calls your code, and the name says
which: `_name` is a **method on your type** that we call (`_ready`, `_update`,
`_draw`, `_end`, `_collision` — on scenes, objects and behaviors alike);
`on_name` is a **function you hand us at runtime** (`on_click`, `on_drag`,
`on_collision`, and the free-function entry-point hooks `on_ready` / `on_frame`
/ `on_exit`). An `on_*` never takes a leading underscore.

This is legal precisely where it is used: `_name` is reserved at *global* scope,
not as a class member — which is also why the entry-point hooks are `on_*` and
not `_*`. And it is why the scene hook is `_end` and not `_exit`: `_exit(int)`
is POSIX. Everything you call on us is unaffected: `rmp::ui::button()`,
`object.apply_force()`.

Private implementation lives in `namespace detail`, in a header under `src/`
that the user cannot reach. `include/` is the user's world.

## Formatting

**It is enforced, so do not hand-format.** `.clang-format` and `.clang-tidy` are
committed at the repo root, pinned to clang 22.1.8 in
`thirdparty/FROZEN_VERSIONS.md`, and both run in the CI `lint` job.

```
just fmt          format every file we own
just fmt check    what CI runs
just lint         clang-tidy, warnings-as-errors
just lint fix     apply what it is sure about
```

`just test` runs `just fmt check` first, so an unformatted file fails locally
before it fails in CI. `tools/versions_check.sh` compares the clang-format on
PATH against the pin — if yours is a different minor version it will reformat
files that were already correct, and it tells you so.

The style itself, for when you are writing rather than fixing:

- **4 spaces**, no tabs. Braces on the same line. 90 columns.
- **No alignment.** Not one kind of it — no columns of `=`, no padded trailing
  comments, no ruler-straight tables. A rename should reflow one line, not
  twenty, and `git blame` should point at whoever wrote the code.
- `.h`, not `.hpp` — the repo is consistent and this was decided once.
- Headers use `#pragma once` (`portability-avoid-pragma-once` is off for this).
- Single-line guards stay on one line: `if (!frame_open()) return;`.
- Options-struct fields align as one column, across blank lines and comments.

One exemption is left in the repository, and it is **not** about alignment: the
platform macros in `rmp/app.h`, whose bodies contain block comments that every
formatter mangles. The trailing backslashes there are a language requirement,
not a ruler.

`thirdparty/` and `generated/` are excluded from both tools
(`.clang-format-ignore`, and the filters in `.clang-tidy`). `tests/smoke_test.h`
is excluded from clang-tidy too, and not because it is exempt from review: it is
compiled as C99 by `examples/plain_c/main.c`, so `(void)` parameter lists,
C-style casts and `NULL` are required there and every `modernize-*` check has
the opposite opinion.

## Modern C++, and macros

C++20, and the modern parts are used where they buy something real:

- **Designated initialisers** for every options struct. This is the reason the
  API reads the way it does.
- **Lambdas as container bodies**, so the compiler closes what you opened and
  "I forgot the `end()`" stops being a category of bug.
- `std::string_view` on the way in, `constexpr` where it is free, RAII guards
  around anything that must be closed.

**Macros are not the enemy and are never banned.** This is a raylib project;
raylib is a C library and its macros (`CLITERAL`, `RAYWHITE`) are used directly.
Clay's macros are *fully supported from userland* — there is an example that
does exactly that, `examples/ui/03_clay_direct.cpp`, and CI compiles it.

What the rule actually is: **our own headers do not force a macro on the user.**
Inside `src/rmp/ui/` we call `Clay__OpenElement` /
`Clay__ConfigureOpenElement` / `Clay__CloseElement` — which are public API —
rather than `CLAY({...})`, because a macro in our implementation would leak
Clay's types into a header the user includes. That is a boundary decision, not a
dislike of macros. Anyone who wants total control gets it, exactly as they can
call raylib or OpenGL directly.

## Comments

Comments say **why**, not what. `// increment i` is noise; the comment that
saved someone an afternoon is the one that says Clay emits a `RECTANGLE` after
the `IMAGE` and that is why the tint travels through `userData`.

- Each file opens with a block saying what it is and what decision it embodies.
- A workaround gets the reason next to it, and if it patches a vendored file, in
  `thirdparty/FROZEN_VERSIONS.md` as well.
- Write English in the code and the docs. Talk to Omar in Spanish.
- Don't write comments that will age: no "TODO(2026)", no "recently changed".

## Where things go

```
include/rmp/*.h                     one header per module, no umbrella — public, no Clay type may appear
src/rmp/                            the implementation, compiled once into the `rmp` library
src/rmp/ui/                         the only place Clay is allowed to exist
src/rmp/internal.h                  private seams: report_once, the resources root, the pack
src/main.cpp                        RMP_GAME(MainMenuScene); and nothing else
src/scenes/                         the sample game's scenes
examples/<area>/NN_name/src/main.cpp   one mini-project per example, and where to go big
```

`src/rmp/` is globbed by all four build systems, so a new `.cpp` needs no CMake
change. A new header search path goes in FOUR places, not one: the `rmp`
library target in the root `CMakeLists.txt` (every CMake target inherits it),
`raymob/app/src/main/cpp/CMakeLists.txt` (Android), `HEADER_SEARCH_PATHS` in
`tools/configure.py` (iOS), and the MSVC syntax pass in
`.github/workflows/_windows.yml`. `tests/configure_test.py` compares the four.

**Warn once, never per frame.** A diagnostic the game can trigger every frame
goes through `RMP_REPORT_ONCE(fmt, ...)` / `RMP_REPORT_ONCE_KEYED(key, fmt, ...)`
from `src/rmp/internal.h`, keyed by call site; not through a `warned` flag of
your own. `rmp::detail::report_count()` is how a test sees it.
That is for mistakes in the GAME. A condition of the MACHINE -- no sound device,
a read-only install folder, a refused write -- is a plain `TraceLog`, said once
by the state that already remembers it, and never `RMP_REPORT_ONCE`: `[dev]
strict` aborts on those, and must not abort a correct game on a CI runner. Say
why in a comment on the line, or somebody will "fix" it. And a read never
reports at all (see `rmp::Value::Ref`).

**No owning raw pointer.** `std::unique_ptr` and `make_unique` for what we own,
`rmp::Handle<T>` for what we refer to across frames, a plain `T *` only when
it is non-owning and the comment says so. `tools/ownership_check.sh` greps for
`new`/`delete`/`malloc`/`free` under `include/rmp/` and `src/rmp/`.
