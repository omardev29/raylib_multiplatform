# Justfile — the handful of commands you actually type.
#
#   just            list these
#   just run        play the game (builds first if it has to)
#   just test       everything that can be checked without a phone or a runner
#
# Deliberately short. Everything here is something you do several times a day;
# anything you do twice a year is a command you should look up rather than
# half-remember from a menu. `just --list` is the whole documentation.
#
# Needs: just (https://just.systems), cmake, ninja. Everything else is checked
# by the recipe that needs it.

# The binary's name comes from [project] name in raylib_multiplatform.toml, so
# renaming your game does not break `just run`.
project_name := shell('python3 tools/configure.py --print-name')

_default:
    @just --list --unsorted

# --- day to day -------------------------------------------------------------

# Compile the debug build.
dev:
    cmake --preset debug
    cmake --build build

# Run the game. Builds it first if it is missing or out of date.
run: dev
    ./build/{{ project_name }}

# Compile the release build: optimised, LTO, assets read from ./resources/.
rel:
    cmake --preset release
    cmake --build build

# Delete every build artefact and everything generated from the .toml.
clean:
    rm -rf build raymob/app/generated raymob/generated.properties \
           cmake/generated include/raylib_multiplatform/generated ios/project.yml
    @echo "clean. the next configure regenerates all of it."

# --- checks -----------------------------------------------------------------

# Check everything, or one thing: examples | layout | smoke | config.
test what="all":
    #!/usr/bin/env bash
    # examples  every example still compiles — they are documentation, and
    #           documentation rots silently
    # layout    the UI layout at four resolutions, with no window and no GPU
    # smoke     boot the game headless and prove it drew actual pixels
    # config    the .toml is valid and the pinned versions still agree
    set -euo pipefail
    run_examples() {
        echo "== examples =="
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
        all)      run_config; run_examples; run_layout; run_smoke ;;
        examples) run_examples ;;
        layout)   run_layout ;;
        smoke)    run_smoke ;;
        config)   run_config ;;
        *) echo "unknown: {{ what }} (all | examples | layout | smoke | config)"; exit 1 ;;
    esac
    echo "PASS"

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

# Bundle resources/ into one AES-encrypted resources.rres, as a release ships.
pack:
    # Run this to test the packed path; `just unpack` goes back to loose files,
    # which is what you want while adding art.
    cmake --preset debug
    cmake --build build --target pack_resources

# Delete the pack, so the game reads loose files again.
unpack:
    cmake --build build --target unpack_resources
