# Justfile — the handful of commands you actually type.
#
#   just            list these
#   just run        play the game (builds first if it has to)
#   just test       everything that can be checked without a phone or a runner
#   just fmt        format every file we own
#   just lint       clang-tidy over our own sources
#
# Deliberately short. Everything here is something you do several times a day;
# anything you do twice a year is a command you should look up rather than
# half-remember from a menu. `just --list` is the whole documentation.
#
# Needs: just (https://just.systems), cmake, ninja. Everything else is checked
# by the recipe that needs it.

# The binary's name comes from [project] name in raylib_multiplatform.toml, so
# renaming your game does not break `just run`.
#
# The fallback is not paranoia: this runs before EVERY recipe, so without it a
# broken .toml takes out the whole file — including `just clean`, which is the
# one you would reach for to get out of the mess. The real error still shows up
# where it belongs, from the configure step of the recipe that needs it.
project_name := shell('python3 tools/configure.py --print-name 2>/dev/null || echo game')

_default:
    @just --list --unsorted

# --- day to day -------------------------------------------------------------

# Compile the debug build.
dev:
    cmake --preset debug
    cmake --build build


# The test below is on the FILE, not on the run. Written the other way round —
# `run && game || build` — a game that exits non-zero, a crash or your own error
# path, would trigger a rebuild and a second launch, which is a confusing thing
# to watch happen.

# Run the game. Only builds it if the binary is not there yet.
run:
    @[ -f build/{{ project_name }} ] || just dev
    ./build/{{ project_name }}

# Compile the release build: optimised, LTO, assets read from ./resources/.
rel:
    cmake --preset release
    cmake --build build

# Delete every build artefact and everything generated from the .toml.
clean:
    rm -rf build raymob/app/generated raymob/generated.properties \
           cmake/generated include/rmp/generated ios/project.yml
    @echo "clean. the next configure regenerates all of it."

# --- checks -----------------------------------------------------------------

# The set of files that are OURS. thirdparty/ is frozen and generated/ is
# rewritten on every configure, so neither is formatted or linted — see
# .clang-format-ignore and the filters in .clang-tidy.
_our_sources := "find include src tests examples -name '*.h' -o -name '*.cpp' -o -name '*.c' | grep -v generated | sort"

# Format every file we own. `just fmt check` only reports, which is what CI runs.
fmt what="write":
    #!/usr/bin/env bash
    set -euo pipefail
    files=$({{ _our_sources }})
    case "{{ what }}" in
      write) clang-format -i $files; echo "formatted $(echo "$files" | wc -l) files" ;;
      check)
        bad=0
        for f in $files; do
            clang-format "$f" | diff -q "$f" - >/dev/null || { echo "  unformatted  $f"; bad=1; }
        done
        if [ "$bad" -ne 0 ]; then
            echo; echo "FALLA: run \`just fmt\` and commit the result."; exit 1
        fi
        echo "  ok    every file is formatted"
        ;;
      *) echo "unknown: {{ what }} (write | check)"; exit 1 ;;
    esac

# Only the .cpp files are passed to clang-tidy: it reaches the headers through
# them, and HeaderFilterRegex in .clang-tidy decides which of those it reports
# on. The two *_impl.cpp are excluded because they exist to compile a vendored
# header once, and its warnings are not ours to fix.
#
# (The blank line below is load-bearing: `just --list` shows the LAST comment
# line above a recipe as its summary.)

# Run clang-tidy over our own sources. `just lint fix` applies what it is sure of.
lint what="check":
    #!/usr/bin/env bash
    set -euo pipefail
    cmake --preset debug >/dev/null   # clang-tidy needs build/compile_commands.json
    files=$(find src tests -name '*.cpp' | grep -v _impl | sort)
    case "{{ what }}" in
      check) clang-tidy -p build --quiet --warnings-as-errors='*' $files && echo "  ok    no warnings" ;;
      fix)   clang-tidy -p build --quiet --fix --fix-errors $files; just fmt ;;
      *) echo "unknown: {{ what }} (check | fix)"; exit 1 ;;
    esac

# Check what matters locally: config, layout, smoke. Or name one, or "examples".
test what="all":
    #!/usr/bin/env bash
    # fmt       every file we own is clang-format clean
    # config    the .toml is valid and the pinned versions still agree
    # layout    the UI layout at four resolutions, with no window and no GPU
    # smoke     boot the game headless and prove it drew actual pixels
    # examples  every example still compiles. NOT part of "all", on purpose:
    #           there is no reason not to keep writing examples, and nobody
    #           wants their machine compiling a growing folder of them every
    #           time they check their own change. CI has a job of its own for
    #           it, on its own runner. Run it by name before touching the API.
    set -euo pipefail
    run_examples() {
        echo "== examples =="
        # The examples include <rmp/app.h>, which pulls in the generated
        # rmp/config.h. After `just clean` that file does not exist
        # yet, and every example would fail for a reason that has nothing to do
        # with the examples. CI gets this for free by generating first.
        python3 tools/configure.py >/dev/null
        local failed=0
        for f in $(find examples -name '*.cpp' | sort); do
            if g++ -fsyntax-only -std=c++20 -Iinclude -Ithirdparty/raylib/src \
                 -Ithirdparty/rres -Ithirdparty/raymob -Ithirdparty/clay -Itests \
                 -DRESOURCES_PATH='"./resources/"' -DPRODUCTION_BUILD=0 "$f"; then
                echo "  ok    $f"
            else
                echo "  FAIL  $f"; failed=1
            fi
        done
        gcc -fsyntax-only -std=c99 -Ithirdparty/raylib/src \
            -DRESOURCES_PATH='"./resources/"' examples/plain_c/main.c \
            && echo "  ok    examples/plain_c/main.c" || { echo "  FAIL  main.c"; failed=1; }
        [ "$failed" -eq 0 ]
    }
    run_layout() {
        echo "== layout =="
        cmake --preset debug -DBUILD_UI_TESTS=ON >/dev/null
        cmake --build build --target ui_layout_test >/dev/null
        ./build/ui_layout_test
    }
    run_smoke() {
        echo "== smoke =="
        cmake --preset debug >/dev/null
        cmake --build build >/dev/null
        # The same gate CI uses: boot, render a bounded number of frames, and
        # check the frame is not blank.
        out=$(RAY_TEST_MAX_FRAMES=10 ./build/{{ project_name }} 2>&1)
        echo "$out" | grep -q "RAY_TEST_BOOT_OK assets_failed=0 " || { echo "$out"; echo "FAIL: boot"; exit 1; }
        echo "$out" | grep -q "RAY_TEST_RENDER_OK"                || { echo "$out"; echo "FAIL: nothing was drawn"; exit 1; }
        echo "  ok    booted and rendered"
    }
    run_config() {
        echo "== config =="
        python3 tools/configure.py --check
        bash tools/versions_check.sh
    }
    case "{{ what }}" in
        all)      just fmt check; run_config; run_layout; run_smoke ;;
        examples) run_examples ;;
        layout)   run_layout ;;
        smoke)    run_smoke ;;
        config)   run_config ;;
        *) echo "unknown: {{ what }} (all | examples | layout | smoke | config)"; exit 1 ;;
    esac
    echo "PASS"

# --- releasing --------------------------------------------------------------

# Cut a release: tag it and push the tag. CI builds all 14 targets and publishes.
deploy version:
    #!/usr/bin/env bash
    # The version comes from the tag and nowhere else — there is no number to
    # bump in a file first. `just deploy 1.4.0` and `just deploy v1.4.0` are the
    # same thing.
    #
    # Run `just test` before this. What follows only checks the things that
    # would waste a twenty-minute pipeline, not whether your game works.
    set -euo pipefail
    v="{{ version }}"
    case "$v" in v*) ;; *) v="v$v" ;; esac

    # A tag on a dirty tree is a lie about what shipped: the artifacts would be
    # built from the commit, not from what you were looking at.
    if ! git diff --quiet || ! git diff --cached --quiet; then
        echo "FALLA: uncommitted changes. Commit or stash them first."
        git status --short
        exit 1
    fi

    if git rev-parse -q --verify "refs/tags/$v" >/dev/null; then
        echo "FALLA: the tag $v already exists here."
        echo "  git tag -d $v                 # if it was never pushed"
        exit 1
    fi
    if git ls-remote --exit-code --tags origin "$v" >/dev/null 2>&1; then
        echo "FALLA: $v is already on the remote. Releases are not re-cut; bump the version."
        exit 1
    fi

    # CI checks out the tag from the remote, so a tag on a commit that only
    # exists here builds nothing — or worse, builds the wrong thing.
    branch=$(git rev-parse --abbrev-ref HEAD)
    if ! git rev-parse -q --verify "origin/$branch" >/dev/null; then
        echo "FALLA: origin/$branch does not exist. Push the branch first:"
        echo "  git push -u origin $branch"
        exit 1
    fi
    if [ "$(git rev-parse HEAD)" != "$(git rev-parse "origin/$branch")" ]; then
        echo "FALLA: HEAD is not what origin/$branch points at (as of your last fetch)."
        echo "  git push        # then try again"
        exit 1
    fi

    # The exact gate CI runs on a tag, with the tag you are about to create.
    # Catches, in this order: an invalid .toml, identifiers still left at
    # com.example.*, and a tag that is not vMAJOR.MINOR.PATCH. All three would
    # otherwise fail in CI, twenty minutes and one dead tag later — and a Play
    # application id is permanent, so the second one is worth catching twice.
    echo "== checking $v the way CI will =="
    GITHUB_REF_TYPE=tag GITHUB_REF_NAME="$v" \
        python3 tools/configure.py --print-config --strict-release >/dev/null
    echo "  ok    $v is release-ready"

    git tag -a "$v" -m "Release $v"
    git push origin "$v"

    echo
    echo "pushed $v. CI is building all 14 targets and will attach them to the release."
    echo "  gh run watch"
    echo
    echo "to undo, if you were quick enough:"
    echo "  git push --delete origin $v && git tag -d $v"

# --- the other platforms ----------------------------------------------------

# Build for the web. Needs the emsdk on PATH (EMSDK set).
web:
    cmake --preset web
    cmake --build --preset web
    @echo "build/web/{{ project_name }}.html — serve it, do not open the file directly:"
    @echo "  python3 -m http.server 8000 --directory build/web"

# Build the Android APK. Needs the Android SDK and NDK.
android:
    python3 tools/configure.py
    cd raymob && ./gradlew assembleDebug
    @find raymob -name '*.apk' -newermt '-2 minutes'

# --- assets -----------------------------------------------------------------

# Bundle resources/ into resources.rres (AES) — the path a release ships.
pack:
    cmake --preset debug
    cmake --build build --target pack_resources

# Delete the pack, so the game reads loose files again while you add art.
unpack:
    cmake --build build --target unpack_resources
