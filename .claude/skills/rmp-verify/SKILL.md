---
name: rmp-verify
description: How to prove a change to raylib_multiplatform actually works — the local ladder (rmp test, headless layout, screenshots) and the CI ladder (fast set vs the full 17-target matrix), plus the gotchas that have wasted runs. Load before claiming a change is done, and before pushing.
---

# Verifying a change

The standard here is **reproduce it, do not read it**. Almost every bug in this
project was found by looking at pixels, dumping an artefact or writing an assert
— not by reasoning about the code.

## Local, in the order to run them

```bash
rmp test              # every stage `rmp help test` lists before "only by name": fmt, config, the
                      # gates, the configure, rmp, api and binary tests, unit, layout, property,
                      # ui-render, render, smoke. Always. In the image: xvfb-run -a ./rmp test,
                      # as CI's lint job, or ui-render and the smoke have no display.
rmp test examples     # builds every example and boots it headless with a screenshot, PLAYS the
                      # platformer to the end (tests/platformer_play.cpp), then plays every script
                      # of tests/e2e/ (compared exactly in the image). Before touching public API,
                      # objects, collision, the map, scenes or the game.
rmp test sanitize     # the unit and layout tests again under gcc's ASan + UBSan, every report fatal.
                      # After touching anything that parses input, casts, indexes or frees: what a
                      # test cannot see, this does.
rmp test coverage     # the tests under clang's coverage: every src/rmp file against its floor in
                      # tools/coverage_floor.txt (only rises), every public function no test calls
                      # against tools/untested_api.txt (only shrinks). Gates only in the pinned image:
                      # tools/dev_shell.sh bash tools/coverage_check.sh [--update]. After adding or
                      # testing framework code -- a floor two points behind fails too.
rmp test gates        # (in `rmp test`) each tools/*_check.sh seen red: copied into a scratch tree with
                      # a mistake of its kind planted, and FAIL said and named (tests/gates_red_test.py,
                      # whose GATES names every one: a new gate fails there until it is planted)
rmp test property     # (in `rmp test`) the campaign's gates, RapidCheck at its fixed seed, Hypothesis
                      # derandomized -- skipped with a message without hypothesis installed.
rmp test fuzz         # every fuzz target built in build/fuzz (libFuzzer where clang has compiler-rt)
                      # and every committed input replayed; fails unless every file ran. After
                      # touching a parser: save, Value, LDtk, Tiled, Aseprite, rres, the loader.
rmp test mutation     # Mull and mutmut over every module against tests/mutation/floors/ -- hours, in
                      # the image. One module: tools/dev_shell.sh bash tools/mutation.sh run random
rmp example 01_pong   # build one example and run it (by name or path; `rmp example` lists them)
rmp run               # the actual game
```

`rmp test` deliberately does **not** compile `examples/`: Omar's machine must
not be compiling a growing folder on every check. CI has a job for it on its own
runner.

Individually:

```bash
python3 tools/configure.py --check     # the .toml is valid
bash tools/versions_check.sh           # the pinned versions still agree
cmake --preset debug -DBUILD_UI_TESTS=ON && cmake --build --preset debug --target ui_layout_test
./build/debug/ui_layout_test           # layout at four resolutions, no window, no GPU
cmake --build --preset debug --target ui_render_test
./build/debug/ui_render_test           # the UI on a real GPU: the blur and the scissors, read back
RAY_TEST_MAX_FRAMES=10 ./build/debug/ray_test    # expects RAY_TEST_SAVE_OK, RAY_TEST_BOOT_OK, RAY_TEST_RENDER_OK
```

Things that are easy to get wrong here, each learnt the hard way:

- **A new public header** needs three things before it is pushed: its line in
  `tools/header_budget.txt`, measured INSIDE the pinned image (`podman run --rm
  --init -v "$PWD":/work -w /work ghcr.io/omardev29/raylib-build@<digest> python3
  tools/header_cost.py`; the `lines` column), a row in README's namespace table
  and an entry in TECHNICAL's tree. The configure tests fail on the last two;
  the budget gate fails on a missing line everywhere now. And it is an API
  change: its pull request waits for Omar's `api-approved`.
- **The audio device suite** (`tests/audio_device_test.cpp`) runs once per
  `rmp test`, not in the random-order pass, and skips with a MESSAGE where there
  is no device. CI sets `RMP_REQUIRE_AUDIO_DEVICE=1` and runs it on miniaudio's
  null backend, so a skip there is a failure.
- **The locale tests** need the locales `tools/test_locales.sh` builds from
  `tests/fixtures/locale/` (`LOCPATH`); `rmp test` builds them, CI requires them
  with `RMP_REQUIRE_TEST_LOCALES=1`. Without glibc's `localedef` (macOS) they skip.
- **Save tests never touch the real user folder**: they point it at a temporary
  directory with `rmp::save::detail::set_folders_for_tests()`. The smoke boot
  DOES write to the real one (`rmp-ci-smoke.save`, removed right after).
- **A mutation you ran by hand proves nothing if it did not apply.** Assert the
  text you replace is found exactly once before trusting a green result.

## The deep campaign: properties, fuzzing, mutation

Each harness and property is **seen red before it is trusted green**: a real
defect, or a mutation planted by hand (found exactly once, as above) and the
red it causes written in the commit. What the search finds becomes a named
case, so it is checked on every run and not only when the dice find it again:

- A RapidCheck counterexample prints `RC_PARAMS="reproduce=..."`; play it with
  that variable set, then write the case as a doctest TEST_CASE in the module's
  own `*_test.cpp`. The property file says `// covers: <function>` for each
  public function it holds, and that function's line leaves
  `tests/property/waivers.txt` in the same commit.
- A Hypothesis failure prints `@reproduce_failure(...)`; the case goes into the
  module's unit tests as an explicit example.
- A crash libFuzzer finds lands in `build/fuzz/crashes/<target>/`; it goes into
  `tests/fuzz/regressions/<target>/` with a name that says what it is, and
  `bash tools/fuzz.sh replay <target>` is red until the fix and green after.
- A mutant that survives is a missing assertion -- or an equivalent change,
  listed in the module's floor file with why no test can kill it. Raise a
  floor in the commit that earned it (`bash tools/mutation.sh update <module>`).
- Issues labelled `nightly-finding` are what `deep.yml` found overnight, one per
  signature, each with the command that plays it again. Take them first.

## When the change is visual, look at it

A headless assert proves the boxes are where you said. It does not prove the
thing looks right. `tools/examples_build.sh` boots every example under
`PLATFORM=Memory` and writes a PNG each to `build/examples-check/screenshots/`
(`RAY_TEST_SCREENSHOT=<png>` is the knob, in `tests/smoke_test.h`); look at
them. The `examples` job uploads the same PNGs as an artefact.

Two things that will waste your time:

- **`TakeScreenshot` needs `rlDrawRenderBatchActive()` immediately before it**,
  or the PNG is blank.
- **`TakeScreenshot` mangles absolute paths.** Write relative, from the working
  directory you want the file in.
- The demo runs without vsync, so count **frames**, not `GetTime()`.
- After `SetWindowSize`, the framebuffer can lag the request by a few frames on a
  compositor. Print `GetScreenWidth()` next to the screenshot so a stretched
  image is recognisable as a harness artefact and not a layout bug.

## CI

A push to `main` and a pull request run the fast set: Linux, Windows, Web,
Android, and the framework's own jobs side by side -- `lint`, `tidy`
(clang-tidy, `rmp lint`), `examples` (every example built and booted, the
platformer's bot, the input), `e2e` (the scripts played and compared exactly),
`coverage`, `deep` (the deep campaign's deterministic half: the gcc sanitized
unit tests, the properties at fixed seeds, every committed fuzz input), `docs`,
and `rmp_new`, which makes a game from the commit with `rmp new` and runs that
game's own checks on it. In a
game, the same `ci.yml` skips the framework's jobs and steps
(`needs.config.outputs.framework`). A pull request also runs `api`, the
API-change gate (`tools/api_change.py`): a change under `include/` or to a key
of the `.toml` is labelled `api-change` and waits for Omar's `api-approved`.
`deep.yml` runs the long half every night and every Sunday.

One fast lane per two stages; when it is red, the fix goes through a **debug
lane** limited to the platforms involved -- Linux, Windows and macOS by
default, Android or iOS only when the bug is there -- and the full matrix runs
once, at the end of the plan:

```bash
gh workflow run ci.yml -f targets=linux-x64-glibc,windows-x64,macos   # the debug lane
gh workflow run ci.yml -f full=true                                   # all 17
gh run list --limit 5
gh run view <id>
```

**Pushing cancels an in-flight run.** `ci.yml`'s concurrency group is
`CI-refs/heads/main` with `cancel-in-progress: true`, so a push while a
`full=true` dispatch is running kills it. It happened three times, so it is a
gate: `rmp push` refuses while a run is in flight and names it; `rmp push
force` when killing it is what you meant. Put a background monitor on the run
(the loop in CLAUDE.md) and keep working; never poll.

The 17 targets are worth the wait for anything touching a header, the build, or
`thirdparty/`: MSVC, Emscripten, the three BSDs and the iOS xcframework each fail
in ways nothing on Linux predicts.

## Artefacts are evidence

When a platform build is suspect, open what it produced rather than trusting the
green tick. Both of these caught real bugs:

- `aapt2 dump badging` / unzip the APK and read the merged manifest. That is how
  the missing-launcher-icon bug was found, and how AdMob's real cost was measured
  (14.98 MB and 10 permissions with ads, 8.28 MB and zero without).
- Download the web build and check the `.wasm` size after a link-flag change.

## Reporting

Say what actually happened. If a check was skipped, say which one. If something
is green, name the run id — `32319988331: 17/17` is a fact; "CI passes" is a
claim. And update `../PROGRESS.md` as you go: the session can end at any point and
that file is where the next one starts. A cloud session has no `../`: its pull
request's description is that record, filled in as
`.github/pull_request_template.md` asks.
