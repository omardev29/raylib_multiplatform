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
rmp test              # fmt + config + gates + configure and rmp tests + unit + layout + render + smoke. Always.
rmp test examples     # builds every example and boots it headless with a screenshot, then PLAYS the
                      # platformer to the end (tests/platformer_play.cpp). Before touching public API,
                      # objects, collision, the map, scenes or the game.
rmp test sanitize     # the unit tests again under ASan + UBSan, every report fatal (~20 s cold). After
                      # touching anything that parses input, casts, indexes or frees: what a test
                      # cannot see, this does.
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
cmake --preset debug -DBUILD_UI_TESTS=ON && cmake --build build --target ui_layout_test
./build/ui_layout_test                 # layout at four resolutions, no window, no GPU
RAY_TEST_MAX_FRAMES=10 ./build/ray_test    # expects RAY_TEST_SAVE_OK, RAY_TEST_BOOT_OK, RAY_TEST_RENDER_OK
```

Things that are easy to get wrong here, each learnt the hard way:

- **A new public header** needs three things before it is pushed: its line in
  `tools/header_budget.txt`, measured INSIDE the pinned image (`podman run --rm
  -v "$PWD":/work -w /work ghcr.io/omardev29/raylib-build@<digest> python3
  tools/header_cost.py`; the `lines` column), a row in README's namespace table
  and an entry in TECHNICAL's tree. The configure tests fail on the last two;
  the budget gate fails on a missing line everywhere now.
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

A push runs the fast set: Linux, Windows, Web, Android, the `lint` job, the
`examples` job, and `rmp_new`, which makes a game from the commit with
`rmp new` and runs that game's own checks on it. In a game, the same `ci.yml`
skips the framework's jobs and steps (`needs.config.outputs.framework`). The
full matrix is manual:

```bash
gh workflow run ci.yml -f full=true
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
that file is where the next one starts.
