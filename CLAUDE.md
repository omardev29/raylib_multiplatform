# CLAUDE.md

Read this first. It is the map of the project and the short list of things that
have already cost time once.

## Where you are

This is the framework's own repository, `raylib_multiplatform`. It is usually
checked out inside `project_raylib/`, the repository of plans and docs, whose
CLAUDE.md imports this one and adds the map of the repositories beside it --
among them `../next_architecture/` (where the framework is going, and the
authority on it), `../PROGRESS.md` (the live state of execution: **start every
cold session there**) and `../rmp-docs/` (the documentation site).

## What the project is

A C++20 **framework** (it used to call itself a template) for shipping raylib
games to **17 targets** from one codebase — Windows, Linux, macOS, Web, Android,
iOS and the three BSDs — with CI that builds them, boots them, and checks they
put pixels on the screen.

Everything it adds on top of raylib lives under `rmp::`: `rmp::app`,
`rmp::Scene`/`rmp::Object` with `rmp::behavior`, `rmp::input`, `rmp::ui`,
`rmp::assets`, `rmp::Tilemap`, `rmp::Camera`, `rmp::audio`, `rmp::save` with
`rmp::Value`, `rmp::random`, `rmp::ads`. **There is no
umbrella header**: one header per module under `include/rmp/`, and a game
includes only the ones it uses. One config file, `raylib_multiplatform.toml`,
which generates the CMake, Gradle and Xcode inputs so they cannot drift out of
sync with it. The framework is compiled once into the `rmp` library; the game
and every example link it.

## The rule that outranks the others

**The API has to be as simple as possible for the user.** Complexity is allowed
to exist for advanced cases; it is not allowed to be in the way of the simple
one. The benchmark, and it is not a slogan, is that a main menu is three
functions:

```cpp
rmp::ui::begin();
if (rmp::ui::button("Play")) play();
if (rmp::ui::button("Quit")) quit();
rmp::ui::end();
```

Anything that makes that longer is wrong, whatever else it buys. Three questions
decide whether a feature enters, in this order:

1. Is there a real game that needs it? "Someone might want it" is a no.
2. Can it be composed from what already exists? Then document the pattern in
   `examples/` instead of adding API.
3. Does it complicate the three-function menu? Then redesign it until it does not.

## Naming

Omar's rule, and checked rather than remembered: `readability-identifier-naming`
in `.clang-tidy` over `src/`, `tests/` and `examples/`, `-Wshadow`, and
`tools/naming_check.sh` for what clang-tidy cannot see -- a branch of `#if` for
another platform, member access, macro names, retired names. The reasons are
here so that nobody "fixes" one back.

| What | Spelling | Example |
|---|---|---|
| Types: class, struct, enum, alias, template parameter | `PascalCase` | `rmp::ui::ButtonOptions` |
| Enum members | `CONSTANT_CASE` | `rmp::ui::Align::TOP_LEFT` |
| Constants: every `constexpr`; a `const` at namespace, class or `static` scope | `CONSTANT_CASE` | `MAX_SPAWNED`, `DeviceState::KEYS` |
| Functions, methods, variables, parameters, public fields, namespaces | `snake_case` | `current_theme()`, `min_height` |
| Private and protected data members | `_snake_case` | `_slot`, `_generation` |
| The hooks we call on your type -- these eight, no other method | `_snake_case` | `_ready` `_update` `_late_update` `_draw` `_collision` `_end` `_suspend` `_resume` |
| Functions you hand us | `on_snake_case` | `on_click`, `on_ready` |
| File-scope mutable state in `src/` and `tests/` | a struct per concern, read as `concern.field` | `context.started`, `pointer.down` |
| File-scope mutable state in `examples/` | `snake_case` | `player` (`examples/assets/01_loading`) |
| Our macros, and every value the `.toml` generates | `RMP_CONSTANT_CASE` | `RMP_GAME`, `RMP_WINDOW_WIDTH` |

- **One spelling per kind, and no prefixes.** No `k`, no `g_`, no trailing `_`.
  A type and a value never look alike at the point of use, and raylib's types
  are PascalCase too, so `rmp::Texture` sits next to `Texture2D`. CONSTANT_CASE
  means "known when it compiles": `const float half = width / 2;` inside a
  function is a variable that does not change, and stays snake_case.
- **An underscore never touches a dot or an arrow.** `foo_.x` and `other._x`
  are both wrong, in the framework too. Another object's private field is
  reached through a private method that says what is taken -- `other.slot()`,
  `other.trade(nullptr)`, `object.take_force()`. The one `x._name` is a hook
  call: `a._collision(b)`.
- **A leading underscore on a METHOD means "the framework calls this".** Only
  the eight hooks wear it (clang-tidy lists them), so a private field never
  takes a hook's name: Object's callbacks are `_click_handler`,
  `_drag_handler`, `_collision_handler`. `_name` is legal as a member and
  reserved at global scope -- which is why the entry-point hooks are `on_*`,
  and the scene hook is `_end`, not `_exit` (POSIX).
- **File state is grouped, and the dot is the mark.** `g_` made a global
  visible at every use. Without it, `started` collided with `started()` in 26
  places, `index`, `log` and `free` with a C library that declares them on
  some platforms and not others, and a parameter `scale` would silently have
  hidden the file's `scale`. A struct per concern keeps the mark and cannot
  collide; `-Wshadow` catches a local that hides one (two functions took
  `int pass`, so the group is `this_pass`). In `examples/`, where one variable
  is often the whole story, a plain name reads better, under the same
  `-Wshadow`.
- **A constant's name must not be somebody else's macro.** `PI` and `DEG2RAD`
  (raylib.h), `EPSILON` (raymath.h), `MIN`/`MAX` (BSD `<sys/param.h>`, rlgl.h),
  `CHECK` (doctest), `DEBUG` (raymob's debug build). The preprocessor replaces
  the name on exactly the platforms that define it: green on Linux, red on
  NetBSD twenty minutes later. Name what the number is for -- `NEAR_ZERO`,
  `TOLERANCE`, `SECTION_COUNT` -- and for pi, `std::numbers::pi_v<float>`
  (clang-tidy's `modernize-use-std-numbers` refuses raylib's `PI`).
  `tools/naming_check.sh --macros` prints the set R6 refuses.
- **Macros are `RMP_`, never `__`.** The preprocessor has one namespace, shared
  with raylib, the C library and the game, and `__x` and `_X` belong to the
  implementation. Generated values follow their table: `[window] width` is
  `RMP_WINDOW_WIDTH`, `[project] name` is `RMP_PROJECT_NAME`. A vendored
  library's knobs keep their names in the one file that configures it
  (`CLAY_IMPLEMENTATION` in `clay_impl.cpp`). **A retired name in `#if` is not
  an error, it is 0** -- `#if APP_SAVE_PORTABLE` after the rename would have
  turned portable saves off on Windows and nowhere else -- so
  `naming_check.sh` refuses every one, and every `#ifndef RMP_X` fallback must
  guard a name the generator emits.
- **CMake options keep their names** (`PRODUCTION_BUILD`, `RESOURCES_PATH`,
  `BUILD_TESTS`). They live in CMake's cache and never reach the preprocessor;
  what the C++ sees is `RMP_PRODUCTION_BUILD` and `RMP_RESOURCES_PATH`.
  Renaming an option turns `-DPRODUCTION_BUILD=ON` into a debug build with
  nothing but an "unused variable" warning.
- **A name in a comment is spelled like the code.** A rename renames the
  comments too.

## Standing rules from Omar

- **`src/main.cpp` is a dozen lines and ends at `RMP_GAME(MainMenuScene);`**
  -- phase 4 did that, and it stays that way. The user's game lives in
  `src/scenes/` and `src/objects/`; new mechanics go to `examples/`, never here.
- **`examples/` is where to go big.** They double as tests. Every example is a
  mini-project (`src/main.cpp`, `include/` when it has headers, `src/scenes/`
  when it has more than one scene, `resources/` when it brings art), built as
  a CMake target and **booted headless with a screenshot** by the `examples`
  job. `rmp example <name|path>` builds and runs one. **Omar's machine must
  not compile them constantly**: they are out of `rmp test` (about half a
  minute warm); `rmp test examples` runs `tools/examples_build.sh`, the same
  script CI runs.
- **A game in `examples/games/` is playable from start to finish** -- win,
  lose, play again -- or it does not belong there. The job boots them all and
  the screenshot says whether they look like games; four of the six sat broken
  for a phase behind a syntax-only check, and that is what this rule is for.
- **`rmp` is the one command, and a game is a copy made from a list.**
  `tools/rmp.py`, behind `rmp` (POSIX sh), `rmp.ps1` and `rmp.cmd`, builds,
  tests and ships; `rmp help` lists it all, and every gate is a stage of
  `rmp test`. `rmp new DIR` copies what `INCLUDE` in `tools/rmp.py` names and
  nothing else, so a new tracked file fails `ManifestTest` until it is
  classified -- `INCLUDE` when a game needs it, `FRAMEWORK_ONLY` with the
  reason when not -- and a file a game's CI names outside a framework-gated
  step has to be included (`CiModeTest`). `ci.yml` is the same file in both
  but for the block between its `[ci] on_push` markers, which
  `configure.py` writes from the `.toml` (push and pull_request, or tags
  only) and `--check` compares; what a game lacks is gated on
  `needs.config.outputs.framework`, never cut out of a copy, and the
  `rmp_new` job makes a game from every commit and runs its checks on it --
  `[ci] on_push = false` too, and the red of a `ci.yml` that disagrees.
  A game gets `tests/game/` (doctest, built with its `src/` into
  `game_test`, `rmp test unit`), and the framework runs the same folder
  against the demo game, so doctest ships with every game.
  **`new`, `install` and `update` are global commands** (`GLOBAL_COMMANDS`):
  they act on the framework that ships the `rmp` being run and are never
  handed to a game's copy, which refuses `install` and `update` -- a game
  keeps the framework it was made with.
- **Never put secrets in the TOML** — no keystores, no tokens, no passwords.
  Variables yes, secrets no. The PAT can write repo variables but not secrets.
- **Commits are plain and in Omar's name.** No `Co-Authored-By`, no extra
  trailers: the identity is already configured as his.
- **Only interrupt him** for a serious problem, an architectural decision, or
  something wrong with GitHub. Otherwise execute the whole plan.
- **Write to Omar in Spanish.** Code, comments and docs are in English.
- The canary workflow must only ever run on `omardev29/raylib_multiplatform`.
- **A new header has to be a part of the framework you can name.** `rmp::ui`,
  `rmp::assets`, `rmp::input` — a module the user can decide to use or not.
  Anything else goes into a header that already exists. Splitting is what makes
  this opt-in: you take the ecosystem or you take one piece. Splitting for
  tidiness only taxes the user, because **every header is something they have to
  know exists**, and the count of includes a game needs is a number that should
  go down over time and never up.
- **Our headers do not include each other.** A header under `include/rmp/`
  forward-declares rather than including another `rmp/` header; where a type
  by value makes that impossible (`scene.h` needs `Object` and `Tilemap`), it
  includes it and says at the top why it could not be avoided. If two of ours
  need each other, one of them should not exist. **This is not a rule about
  the STL or about third-party headers, and there never was one** -- Omar said
  so on 2026-09-22 when the smart-pointer work asked: `<memory>`, `<vector>`,
  `<string_view>`, `raylib.h`, `raymath.h`, use what the job needs. What the
  rule buys is that a header can grow without dragging the rest of the
  framework into every translation unit that touches it. Cost is still
  measured and written down, not forbidden (gcc 16, best of five: an empty
  file 6 ms, `<memory>` 221, `<functional>` 131, `<vector>` 97, `<string>`
  147, `<string_view>` 77).
- **`rmp/config.h` is in every public header, and the user never types it.**
  It is the one exception to the rule above, and it earns it by measurement:
  one include -- `rmp/generated/config.h`, the `#define`s the `.toml` hands the
  code, which includes nothing -- and 27 ms against an empty file's 28.
  `tools/header_check.sh` compiles every
  `include/rmp/*.h` on its own against `RMP_WINDOW_WIDTH`, so a new header that
  forgets it fails the build instead of handing somebody an undefined macro.
- **An invalid `.toml` has to fail in `configure.py`, in second one, and say
  where.** Not at `cmake --preset`, not at `rmp deploy`, not on a runner
  twenty minutes in. The error names the **exact line**, what is wrong, and what
  to do instead. Every combination that cannot work is a rejection with a
  reason, and every rejection has a test in `tests/configure_test.py`. A config
  mistake that reaches the compiler is a bug in `configure.py`, not in the
  config.
- **Target names are `<os>-<arch>-<libc>-<env>`, and each segment is only there
  when it can differ.** `linux-x64-glibc`, `linux-x64-musl`,
  `linux-x64-glibc-drm`. `windows-x64` and `macos` stop early because Windows
  has one C library and macOS is universal.
  - **libc** is a segment because musl and glibc are two libraries, not two
    versions of one: a glibc binary does not start on Alpine and a musl binary
    does not start on Debian. Naming only the exception (`linux-x64` +
    `linux-x64-musl`) makes the reader know that no suffix means glibc, and
    that is knowledge the name should carry instead.
  - **env** is the windowing system. `drm` today; `wayland` and `x11` when
    raylib ships native backends for them. It is absent when the binary carries
    all of them and picks at startup, which is what GLFW does now.
  - It IS more to read than `linux-x64`, and that is the trade: a longer name
    that says what it is, against a short one that requires knowing what it
    leaves out. Renaming is cheap while nobody has scripts pinned to the old
    ones, and it stops being cheap later.
  - **The CI jobs carry the same names**, minus the `linux-` that the workflow
    file already says: `x64`, `arm64`, `musl-x64`, `riscv64`, `drm-x64`,
    `drm-arm64`, and `-run` after the ones a second job boots in a container
    (`musl-x64-run`, `drm-x64-run`). A job
    called `musl` when the target is `linux-x64-musl` is a name that has to be
    translated every time somebody reads a red run, and the arch it leaves out
    is the one thing you want to know when a second musl target appears. The
    artifacts keep the full name, because they are what the user downloads.
- **A Linux job downloads nothing. That is what the image is for.** Every
  compiler, tool and library the Linux jobs need lives in
  `ghcr.io/omardev29/raylib-build`, pinned by digest — cmake, ninja, zig (with
  its libc++ cache already warm), upx, butler, actionlint **and shellcheck**
  (without it actionlint never reads a `run:` block and says green to
  everything; the hosted runners ship it, the image did not until 2026-09-23),
  PyYAML, the X11/GL/Wayland and DRM headers. The digest lives in THREE
  places — `ci.yml`, `web-backends.yml`, `thirdparty/FROZEN_VERSIONS.md` —
  and `versions_check.sh` fails if any two disagree. Bumping it: push the
  image repo, let its workflow publish and verify both architectures (it
  runs the containers by tag, because `docker run --platform` against a
  digest reference fails with "cannot overwrite digest"), read the digest
  from its summary. A download at job time is a single point of failure that no
  amount of version pinning fixes: the sha256 can be right and the network still
  be wrong, and then twenty minutes of matrix die for a reason that has nothing
  to do with the code. Adding a tool means bumping the image and its digest in
  **both** `ci.yml` and `thirdparty/FROZEN_VERSIONS.md` — `versions_check.sh`
  fails if they disagree. `tools/` scripts use the image's copy when it is on
  `PATH` and download only as a fallback, so they still work on a laptop.
- **Test exhaustively, to the point of paranoia. Assume nothing.** Anything
  that can be tested is tested, and the test asserts what actually happened and
  not that a command exited 0 — this repository has shipped a job that went
  green having run none of its script, a test binary that compiled without
  being in the binary, and a 105-test suite that quietly ran 72. For every
  option: the valid values, every invalid one, the wrong *type* (an array where
  a string goes — that crashed five checks with `TypeError`), the empty value,
  the combination that cannot work. For every gate: prove it fails on the thing
  it was written for, not just that it passes today. A test you have not seen
  go red is not a test, it is a hope with a name.
- **Every deterministic mistake must be catchable by a gate, not by memory.** This
  is Omar's rule and it is the one that compounds. If a class of bug can be
  described precisely, something automated should reject it — a clang-tidy
  check, a validate() branch, a test, a grep in the lint job. The evidence is
  from this repository: `A && B || C` was written, fixed, and then **written
  again in seven more places** two rounds later. The note here used to say
  actionlint caught it; it does not (shellcheck's SC2015 fires only on
  degenerate forms), and the audit of 2026-09-22 found two live instances —
  so it is `tools/shell_pattern_check.sh` now, in `rmp test` and in `lint`,
  and it went red on those two the day it existed. Two disagreeing checks on
  `[dev] compiler` would have made `mingw` unusable, and the configure tests
  found it the day they existed. A context window ends; a gate does not.
- **Ownership is RAII, everywhere, public headers included.** No owning raw
  pointer, no bare `new`/`delete`/`malloc`/`free` under `include/rmp/` or
  `src/rmp/`; `tools/ownership_check.sh` is the ratchet and its exception list
  only shrinks. **No raw pointer and no C string in a declaration a game can
  name**: a value, a `T &`, `std::span`, `std::optional`, two overloads, or
  `rmp::Ref<T>` (`object.h`) for a reference that may be empty --
  `if (auto h = obj.get<Health>()) h->hp -= 1;` -- and `std::string_view` in,
  `std::string` out. `tools/pointer_check.py` (`rmp test pointers`) reads the
  headers' AST and fails a public one; a pointer in `detail::` or a private
  member goes in `tools/pointer_ratchet.txt` with its reason, and that list
  only shrinks. `rmp::Handle<T>` is what you keep across frames. `Callback<A...>` stays
  instead of `std::function` on purpose: `std::function` requires copyable
  callables, and a lambda that captures a `unique_ptr` would stop compiling.
- **We are built on raylib, and the copy we ship is modified.** The zlib
  licence does not ask us to say "based on" instead of "uses"; what its
  clause 2 does require is that an altered copy be plainly marked, so
  `thirdparty/raylib/PATCHES.md` exists (clay, cute_tiled, raylib-cpp and
  raymob have one too) and no document may describe the vendored raylib as
  unchanged. **A new dependency is a licence decision**: it gets a row in the
  components block of `THIRD_PARTY_LICENSES.md` or `tools/license_db.py --check`
  fails; copyleft (LGPL included -- no dynamic linking on iOS or static musl),
  non-commercial and unknown fail; a zlib or Apache component is either
  pinned unmodified by sha256 or marked modified with a `PATCHES.md`. The
  shipped `LICENSES.txt` is generated from that same block, per family, and
  goes next to the binary, into the APK and into the iOS bundle.
- **The framework warns once, through `RMP_REPORT_ONCE`.** A diagnostic the
  game can trigger every frame (an action nobody defined, a `pop()` with
  nothing underneath, a tag not in the sheet) goes through
  `rmp::detail::report_once()` in `src/rmp/internal.h`, keyed by call site
  (and by value with `RMP_REPORT_ONCE_KEYED`), never through a hand-rolled
  `warned` flag. `[dev] strict = true` makes the first one abort a debug build.
  **That is for mistakes in the GAME.** A condition of the MACHINE -- no sound
  device, a portable build in a read-only folder, a disk that refused a write --
  is a plain `TraceLog`, said once by the state that remembers it (`attempted`,
  `chosen`), and never `RMP_REPORT_ONCE`: strict would abort a correct game on a
  CI runner or a player's locked-down PC. Each such line says why in a comment;
  "fixing" one into `RMP_REPORT_ONCE` breaks strict mode. And a READ never
  reports at all -- `rmp::Value` reads through a path that creates nothing.
- **Between phases, the fast lane is enough.** The full 17-target matrix is
  ~20 minutes and belongs to structural phases only (a header split, a new
  vendored dependency, an entry-point change). The BSDs are best effort: a
  failure there after a green fast lane is a **PSB** — a platform-specific bug —
  and the history of this repo says those are cheap to fix once and never come
  back. Running the whole matrix between every phase costs more time than the
  PSBs do.

## Caveats that have already cost time

Each of these was a real bug, found by reproducing rather than by reading.

- **C++20 requires designated initialisers in declaration order.** `{ .grow_y =
  true, .width = 8 }` does not compile. This has bitten twice. Option structs are
  ordered the way they will be written — sizing interleaved by axis
  (`grow_x, width, grow_y, height`), then appearance, then identity.
- **Clay does not copy strings.** It keeps the pointer and reads it during
  `Clay_EndLayout`. Every string that goes in is interned into a frame arena on
  the way through, so `text(std::to_string(score))` is safe.
- **`Clay_GetElementId` and `Clay_GetElementIdWithIndex(…, 0)` hash
  differently.** There is exactly one id scheme in the codebase and it is
  `WithIndex`. Mixing them means an element created one way cannot be found the
  other way.
- **Clay emits `RECTANGLE` in addition to `IMAGE`** for an element with a
  background colour, and the rectangle comes *after*. An image tint put there
  paints a flat square over the picture; ours travels via `userData`.
- **The debug and release presets share `build/`, and a reconfigure does not
  take the compiler back out of the cache.** `[dev] compiler` is written with
  `FORCE`, so `rmp build release` after `rmp test` built with clang instead of the
  platform default — not what CI ships. It surfaced as `libraylib.a: file
  format not recognized`, because clang's `-flto=thin` leaves bitcode in the
  archive where gcc's `-flto=auto` leaves ELF. Guarded now: a release configure
  on a debug cache is a `FATAL_ERROR` that tells you to `rmp clean`. Splitting
  the two binary directories is the real fix and is phase 15 debt.
- **CMake's IPO is per target.** A test target that links raylib in a release
  tree needs the same `INTERPROCEDURAL_OPTIMIZATION` or it cannot read the
  archive. `rmp build`/`rmp build release` also build only the game target now.
- **MSYS2 is Windows, and neither language agrees by default.** `platform.system()`
  there returns `MSYS_NT-…` or `MINGW64_NT-…`, not `Windows`; and in MSYS2's MSYS
  environment CMake sets `MSYS` and `UNIX` but **not `WIN32`**. Both spellings
  rejected `[dev] compiler = "mingw"` on a real Windows box, in the shell that
  ships `x86_64-w64-mingw32-gcc`, saying mingw only makes sense on Windows.
  `on_windows()` in `configure.py` and `if(NOT WIN32 AND NOT MSYS AND NOT CYGWIN)`
  in CMake.
- **mold links ELF and nothing else** — `mold --help` prints its own target list
  and there is no PE/COFF or Mach-O in it. It cannot work on Windows or macOS,
  ever. `find_program(mold)` finding a binary there means nothing.
- **A linker that exists is not a linker you can use.** GCC's `-fuse-ld=lld`
  needs an `ld.lld` driver on `PATH`, not just `lld`, and MSYS2 ships one
  without the other — a clean configure and then a link error about `ld` that
  names neither lld nor the setting that asked for it. The choice is verified
  with `check_linker_flag`, which is why `cmake/generated/dev_linker.cmake` is a
  separate file included **after** `project()`: checking means compiling.
- **A vendored header goes on FOUR include lists, not one.** The `rmp` library
  target in `CMakeLists.txt` carries the one list every CMake target (the game,
  the tests, every example) inherits; Android builds through
  `raymob/app/src/main/cpp/CMakeLists.txt`; iOS through a generated Xcode
  project whose `HEADER_SEARCH_PATHS` live in `tools/configure.py`; and the
  MSVC syntax pass over `examples/` in `_windows.yml` has its own. cute_aseprite
  and cute_tiled went into the first one only, and the matrix came back with
  `fatal error: 'cute_tiled.h' file not found` from iOS and
  `'cute_aseprite.h' file not found` from Android — twenty minutes after the
  push, because getting three of four looks exactly like getting four until
  something else says otherwise. Then `rmp/math.h` needed the BARE
  `thirdparty` root (it includes `<raylib-cpp/Vector2.hpp>`) and three of the
  lists lacked it, so that header did not compile on Android, iOS or MSVC and
  nothing said so. Gated in `tests/configure_test.py`: every directory under
  `thirdparty/` holding a header, and the bare root, must appear in all four,
  and `ci.yml`/`tools/rmp.py` may not grow their own `-I` list again.
- **A marker that only ever prints is not a gate.** `SmokeTest_ReportBoot()`
  logged `RAY_TEST_BOOT_OK` unconditionally, so on the DRM job it printed it
  straight after raylib printed `Failed to initialize EGL device` and `Failed to
  initialize platform`. There was no window at all, and the string every CI job
  greps for said the game had booted. It checks `IsWindowReady()` now. All
  seventeen targets go through that function, so the gate was blind everywhere and
  was found by the one platform that could not start.
- **A BSD job can go green having run almost none of its script.** The same
  syntax error that made FreeBSD and NetBSD exit non-zero made OpenBSD's `sh`
  exit **0**, so that job passed after printing one `echo` and stopping — a
  green that proved nothing, which is worse than a red. The VM script now writes
  `build-memory/.rmp-bsd-complete` as its last line and a following step checks
  for it, because nothing inside a script can catch its own early exit.
- **The BSD jobs' inline script has a SIZE LIMIT.** `cross-platform-actions`
  carries it into the VM through `cpa.sh` and cuts it off somewhere between 4.8
  KB (worked) and 5.6 KB (did not). Both failures looked like something else:
  one cut landed inside a quoted string and came back as `Unterminated quoted
  string` on an unplaceable line, the other landed where the script happened to
  parse, so the shell hit EOF and **exited 0** and the step passed having run
  half of itself. Anything substantial goes in a committed file —
  `tools/render_check.sh` — which rsyncs in with the workspace, costs one line
  in the workflow, and can be run on a laptop instead of twenty minutes at a
  time in a VM.
- **A game started inside the BSD VM's script eats the rest of the script.**
  cross-platform-actions feeds the step's script to the VM's shell on STDIN,
  and raylib's headless platform (`rcore_memory.c`) polls the keyboard with a
  non-blocking `getchar()` -- and stdio reads a whole buffer from the fd. On
  2026-10-05 `shipped_check.sh`, one line before `render_check.sh`, made a 3.1
  KB script die with `sh: 59: Syntax error: Unterminated quoted string` on its
  last line: the same two symptoms the size limit above was blamed for (a cut
  inside a quoted string, a script that runs out and exits 0), well under that
  limit. Reproduced with ten lines of C doing what `kbhit()` does. Every game
  launch in `tools/` reads `< /dev/null`, and
  `GameLaunchesReadNothingFromStdinTest` fails one that does not. The size
  limit may have been this all along; its gate stays, because it costs nothing.
- **The BSD jobs' script must be pure ASCII.** `cross-platform-actions` carries
  it into the VM through its `cpa.sh` shell and re-parses it there, and a
  non-ASCII byte does not survive: an em dash inside a double-quoted `echo` came
  back as `sh: 83: Syntax error: Unterminated quoted string`, with a line number
  from the generated script and in a step whose real job was to build the game.
  The block that had worked for months contained zero of them. Checked by
  `tools/portable_check.sh`.
- **macOS ships bash 3.2 and always will** (bash went GPLv3). `mapfile`,
  `readarray`, `stat -c`, `grep -P`, `readlink -f`, `nproc` and `sed -i` without
  an argument are all GNU or bash-4 only. `tools/portable_check.sh` rejects them
  in `tools/` and in `rmp`, the launcher; the Linux jobs cannot notice, having
  GNU everything, and the macOS job runs `rmp test config` under `/bin/bash` --
  which reaches only the scripts that stage calls.
- **`TakeScreenshot` needs `rlDrawRenderBatchActive()` first**, or the file is
  blank. It also mangles absolute paths.
- **DRM renders a different hash, and that is correct.** `linux-x64-glibc-drm`
  gives `01a90f7a` where every other target gives `e2ffc15d`, because it
  rasterises through OpenGL ES on llvmpipe and the golden hash comes from
  raylib's own software renderer. The pixel count and the coverage ratio are
  identical (`pixels=92432 ratio=0.25676`), which is what says the geometry is
  the same and only the shading differs. Do not "fix" it by copying one hash
  over the other.
- **CI can run DRM, on a kernel display that does not exist.** `vkms` is
  mainline Virtual Kernel Mode Setting: a DRM device with a CRTC, an encoder and
  a connector and no hardware behind them. Two things it needs, and both were
  found the hard way: the runners' Azure kernel does **not** ship the module
  (`modprobe: FATAL: Module vkms not found`), so `linux-modules-extra-$(uname -r)`
  has to be installed -- the one place a Linux job is allowed to download,
  because nothing depends on that job and the binary is already built and
  uploaded when it starts; and Mesa needs **`LIBGL_ALWAYS_SOFTWARE=1`**, not
  `MESA_LOADER_DRIVER_OVERRIDE=kms_swrast`, which left EGL saying only "Failed
  to initialize EGL device".
- **Pushing cancels an in-flight CI run.** `ci.yml`'s concurrency group is
  `CI-refs/heads/main` with `cancel-in-progress`. It happened twice, the note
  said so, and then it happened a **third** time to someone who had just read
  the note — so it is a gate now: **`rmp push`** refuses while a run is in
  flight and names the run it would have killed. `rmp push force` when killing
  it is what you meant. Three occurrences is where a written caveat stops being
  worth writing and starts being worth checking. With `[ci] on_push = false`
  a push starts no run, so `rmp push` checks nothing -- unless `ci.yml` has
  changes that are not committed, because the run follows the committed one.
- **The installers are read the way the user runs them.** `install.sh` IS the
  shell's stdin under `curl ... | sh`: all of it is one `main()` called on the
  last line, so a download cut short runs nothing, and every command it starts
  reads `< /dev/null`, or it eats the rest of the script (the BSD caveat above,
  again; `InstallTest` runs every tool through a stand-in that reads all of
  its stdin, and dash's read-ahead hides the mistake, so bash is the one
  required to go red). `install.ps1` runs inside the user's own session under
  `irm ... | iex`: it never calls `exit`, which would close their window --
  a failure is a `throw`.
- **A blobless clone of a shallow repository never finishes.** `git clone
  --filter=blob:none` from the shallow `actions/checkout` loops in git's
  promisor fetch ("cannot exec '-c': Argument list too long"), so the CI steps
  that try the installers clone from a one-commit repository made with
  `git archive HEAD`.
- **`Path.resolve()` raises on a symlink loop in Python 3.11 and 3.12**
  (`RuntimeError`; 3.13 gives the path back). `rmp install` meets one where a
  user's `~/.local/bin/rmp` or a PATH entry should be; `resolved()` in
  `tools/rmp.py` is the spelling that survives it.
- **iOS must not call `exit()`** — Apple QA1561: the app "will appear to the user
  to have crashed", and App Review rejects it. `rmp::app::quit()` logs and does
  nothing there; the process-ending path is `rmp::app::detail::exit_process()`,
  used only by the CI smoke test.
- **Android: `android:required` is only valid on `<uses-feature>`**, silently
  ignored on `<uses-permission>`. And the app category is `android:appCategory`
  on `<application>` — putting it in the launcher `<intent-filter>` replaced
  `LAUNCHER` and the installed app had no icon anywhere.
- **NetBSD 10.1's GCC 10.5 reports `__cplusplus == 201709L`** under `-std=c++20`,
  which trips version guards. Clay's is patched, documented at the patch site and
  in `FROZEN_VERSIONS.md`.
- **raylib 6 pans from -1 to 1, with 0 in the middle.** raylib 5 used 0..1 with 0.5 as the
  centre, and that value passed to raylib 6 plays three quarters to the right -- every default
  `rmp::audio::play()` did, and no unit test could hear it. `tests/audio_device_test.cpp` opens the
  real device at master volume 0 and reads the mix back through `AttachAudioMixedProcessor`;
  miniaudio keeps its null backend, so it runs on CI runners too.
- **Probing for a file is not loading it.** `rmp::assets::detail::resource_exists()` once went
  through `fallback_path()`, which counts a miss as a failed asset, and `rmp::audio` probes
  `.wav`, `.ogg`, `.mp3`, `.qoa` in turn -- so a correct game booted with `assets_failed=3`, and
  only on machines with a sound device. Anything that asks "is it there?" builds its own path.
- **cJSON prints numbers wrong and reads them by locale.** Its printer keeps 15 digits when they
  read back "within an epsilon" (2^53 came back as 9007199254740990), and its parser follows the
  C locale, so a game with a German UI could not read `0.5`. `src/rmp/save.cpp` writes numbers
  itself (15, 16 or 17 digits, the fewest that read back exactly) and runs every conversion under
  the `"C"` numeric locale for its duration. **`ENABLE_LOCALES` is deliberately NOT defined** in
  `cjson_impl.c`: it was the first fix, and it takes ONE byte of the decimal point -- Pashto's is
  two. cJSON itself stays unmodified; `cjson_impl.c` only sets its nesting limit to 64, the same
  as `rmp::save::detail::MAX_DEPTH` (the web's 64 KB stack overflowed near 400 levels).
- **A new public header needs its line in `tools/header_budget.txt`, measured in the image.** The
  size comparison only gates inside the pinned image, and `rmp/audio.h` reached CI with no line at
  all. A missing or stale line now fails everywhere; the number itself is the `lines` column of
  `podman run ... python3 tools/header_cost.py` against the pinned digest (`--check` prints the
  same table and adds the verdict).
- **One session per working tree.** On 2026-09-27 two sessions resumed the same conversation on
  the same checkout, and one rewrote a test file the other had just written. Before stashing or
  popping, compare against a copy you made yourself.
- **A header's promise is not a feature until a test says so.** `rmp/tilemap.h` said from phase
  10 that the scene "collides against its solid tiles", and nothing did: the collision pass knew
  objects and nothing else. Two phases went by because no game used a map; the platformer of
  12B fell through its first floor. Every behaviour a header comment claims gets a test that
  would fail without it -- `tests/map_collision_test.cpp` is that test now.
- **LDtk can check our LDtk files, and that is how the generated world was accepted.** The 1.5.3
  AppImage extracts with `--appimage-extract`; started with `--remote-debugging-port=9333`
  (Electron 24, so it needs a display: it shows up on Omar's screen) it can be driven over CDP,
  and `page_Editor.ME.onSave()` evaluated inside its main loop saves the project. A generated
  project LDtk refuses fails with a crash page, not a message: the first one was `levelId` in a
  layer instance holding the level's INDEX where LDtk wants its `uid`. Diff what LDtk saves
  against what was generated -- for `world.ldtk` the only difference was LDtk's pixel cache.
- **The UI reads the keyboard through its own seam, not rmp::input's.** `rmp::ui` navigation is
  `nav_from_raylib()` behind `rmp::ui::detail::set_nav_provider()`; feeding keys through
  `rmp::input::detail::set_sample_provider()` moves the player and leaves the menus deaf. A
  headless driver needs both. The first button of a screen is focused already: Enter alone
  presses it.
- **Web has no `while` loop and no `-s ASYNCIFY`.** The browser owns the frame
  loop via `emscripten_set_main_loop`. If you ever put a while loop back you have
  to put ASYNCIFY back with it, and it is expensive. Do not call `SetTargetFPS()`
  on web -- `NoSetTargetFpsOnTheWebTest` walks every `#if` and fails on a call
  the web build compiles (`ui/04_settings` had one). `[window] fps` is the cap,
  and `app.cpp` applies it everywhere else, before `InitWindow` (DRM reads it to
  pick the display mode).

## Verifying

`rmp fmt` and `rmp lint` (both clean is a condition, not an intention — see
below), `rmp test` locally (format, config, the gates — seam, workflows,
portable, shell patterns, ownership, headers, header cost, licences, repo —
the configure tests, the unit tests in two orders (the `audio: device` suite, which listens to the
real mixer for about a second, only in the first), headless layout, render
and smoke), `rmp test examples` before touching the public API (it builds
and boots all of them with a screenshot each, and plays the platformer to the flag at 60 and
240 Hz), `rmp test sanitize` after touching anything that parses, casts or frees (the unit tests
under ASan and UBSan -- it found UB the tests passed over, and three tests reading freed memory,
on its first run), and a screenshot when the
change is visual — looking at pixels is how most of the bugs above were
found. Every stage of `rmp test` has a step in the `lint` job -- one a game's
CI runs too, for the stages a game has -- and `StagesAgreeWithLintTest` says
so. The image-dependent gates (header budget, PyYAML, versions
against the manifest) run for real only inside the pinned image: `podman run
--rm -v "$PWD":/work -w /work ghcr.io/omardev29/raylib-build@sha256:<digest>
bash tools/<x>_check.sh` is how to see what CI sees.
CI: a push runs the fast set, `gh workflow run ci.yml -f full=true` runs all 17
targets. The `docs` job checks `omardev29/rmp-docs` (main) against the commit:
every page's gates and every code block compiled against these headers. It only
warns on a push and blocks a tag's release, so a change to the public API is
followed by the docs change in `../rmp-docs` and a bump of its `FRAMEWORK_REF`
(`python3 tools/build.py check --tier compile` there says what broke).

### Never wait on CI by polling. Put a monitor on it.

A full matrix is ~20 minutes and a BSD VM alone can be 15. Watching it with
repeated `gh run view` calls burns the context window on output nobody will read
again, and the alternative — asking Omar to come back and tell you — wastes his
time on something a machine can watch. **Start a background monitor and keep
working.** Events arrive on their own; the run is watched even while you are
mid-sentence about something else.

The shape that works, and the one to reuse:

```bash
prev=""
while true; do
  s=$(gh run view "$RUN" --json jobs \
        --jq '.jobs[] | select(.conclusion != null and .conclusion != "") | "\(.conclusion)\t\(.name)"' 2>/dev/null || true)
  cur=$(printf '%s\n' "$s" | grep -v '^success' | sort || true)
  comm -13 <(printf '%s\n' "$prev") <(printf '%s\n' "$cur") 2>/dev/null || true
  prev=$cur
  st=$(gh run view "$RUN" --json status --jq .status 2>/dev/null || echo unknown)
  if [ "$st" = "completed" ]; then
    gh run view "$RUN" --json jobs \
      --jq '[.jobs[].conclusion] | group_by(.) | map("\(.[0])=\(length)") | join(" ")'
    break
  fi
  sleep 45
done
```

Five things in there are load-bearing, and each one is a way this goes wrong:

1. **It emits every terminal state, not the good one.** The filter is
   `grep -v '^success'`, so failure, cancelled, skipped and timed_out all
   arrive. A monitor that greps for the success marker stays silent through a
   crashloop, and **silence is indistinguishable from still-running**. Before
   arming one, ask: if this died right now, would anything be printed?
2. **`comm -13` against the previous poll**, so each job is reported once and
   not every 45 seconds. Without it a 20-minute run is forty copies of the same
   line.
3. **A final summary line and a `break`.** The monitor ends itself when the run
   completes; it does not sit armed until the timeout.
4. **`|| true` on every `gh` call.** One flaky request must not kill the watch.
5. **45 seconds.** It is a remote API with rate limits; 5-second polling buys
   nothing because jobs take minutes.

Then, while it runs: **do not poll, do not sleep, do not ask if it is done.**
Get on with the next thing. And remember that `rmp push` refuses while a run is
in flight — if the fix being pushed changes what the running matrix is testing,
`rmp push force` and relaunch is right; if it does not, let the run finish
first, because a cancelled matrix proves nothing.

The same pattern covers anything with a slow, remote, or unobservable end: an
image build in the other repository, a release publishing, a VM that may hang.
Anything with a single, local, fast end — a build that either finishes or does
not — is a backgrounded command with an `until` loop instead, which notifies
once and exits.

**Formatting is enforced.** `.clang-format` and `.clang-tidy` are committed and
pinned to clang 22.1.8 in `thirdparty/FROZEN_VERSIONS.md`; the CI `lint` job runs
both, and `versions_check.sh` tells you if your local clang-format is a different
version (it would reformat files that were already correct). Do not hand-format,
and do not reformat a file you are not otherwise changing.

Skills in `.claude/skills/` carry the detail: code style, API design, the
verified Clay facts, and the verification ladder.
